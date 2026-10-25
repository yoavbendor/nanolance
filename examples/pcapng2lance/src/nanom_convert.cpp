// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// pcapng -> Lance converter, built on nanom (the parser-combinator library) and nanolance's writer.
//
// The whole parse side -- the pcap/pcapng block scanner and the per-EPB field decode -- runs through nanom's
// parser-combinator core (`nm::strct<T>(order)` over structs described with one `NANOM_DESCRIBE`). The Lance write
// side runs on nanolance's compile-time typed facade: the packet and remainder tables declare their schema (including
// the lance.blob.v2 payload_ref via blob_ref_column) as a type, and the per-PDU tables derive theirs from each row
// struct's NANOM_DESCRIBE.
//
// Scope: the packet table (the `packets.lance` a capture becomes), the staged layers (--stage l1..l4), the one-shot
// L2/L3/L4 protocol walk under --decode-l2l3 (Ethernet -> VLAN* -> IPv4/IPv6 -> TCP/UDP, the IPv6 extension-header
// chain and SRv6 SRH, and the IPv4 option table), slices (--drop/--count), windowed and chunked writes, and the SHB/IDB
// options as metadata. See README.md, and tests/golden/ for the reference the tables are checked against.
//
// Pipeline: read file -> nm scan_blocks (Phase A) -> per-block classify ->
//   nm parse_epb (Phase B) + option walk (ts_resol / epb_flags) -> soa<PacketRow>
//   scalar columns + external payload_ref -> one Lance fragment; with
//   --decode-l2l3, each packet also runs nm walk_packet_ext (descending the IPv6
//   ext-header chain) -> per-PDU soa<Row> -> one Lance table per PDU type.

#include "nm_pcap.hpp"  // nanom pcap/pcapng scanner (from the vendored nanom submodule)
#include "nanom_pdu_tables.hpp"        // per-PDU Lance row types + converters (--decode-l2l3)
#include "nanom_soa_lance_writer.hpp"  // generic nanom soa<Row> -> Lance table writer

#include "mem_budget.hpp"
#include "nanolance/typed_writer.hpp"
#include "nanolance/blob_v2_external.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nanoarrow/nanoarrow.h>

namespace fs = std::filesystem;
namespace nm = nanom;
namespace nt = nano_lance::typed;

namespace {

// The L1 packet row — byte-for-byte the same schema pcapng2lance writes (see
// examples/pcapng2lance/include/packet_row.hpp): eight scalar columns + the
// lance.blob.v2 external payload_ref, declared as a compile-time typed schema
// (field order + Arrow types match the nanotins example exactly).
struct PacketRow {
    std::uint64_t packet_id;
    std::uint32_t interface_id;
    std::uint64_t ts_raw;
    std::uint32_t caplen;
    std::uint32_t origlen;
    std::uint16_t link_type;
    std::uint8_t ts_resol;
    std::uint32_t epb_flags;
};

using L1Schema = nt::schema<nt::column<std::uint64_t, "packet_id">,
                            nt::column<std::uint32_t, "interface_id">,
                            nt::column<std::uint64_t, "ts_raw">,
                            nt::column<std::uint32_t, "caplen">,
                            nt::column<std::uint32_t, "origlen">,
                            nt::column<std::uint16_t, "link_type">,
                            nt::column<std::uint8_t, "ts_resol">,
                            nt::column<std::uint32_t, "epb_flags">,
                            nt::blob_ref_column<"payload_ref">>;

using RemainderSchema = nt::schema<nt::column<std::uint64_t, "packet_id">,
                                   nt::column<std::uint64_t, "next_protocol">,
                                   nt::blob_ref_column<"payload_ref">>;

int fail(const std::string& msg) {
    std::fprintf(stderr, "pcapng2lance: %s\n", msg.c_str());
    return 1;
}

std::string to_file_uri(const fs::path& path) {
    const auto abs = fs::absolute(path).generic_string();  // forward slashes
    return abs.size() > 1 && abs[1] == ':' ? ("file:///" + abs) : ("file://" + abs);
}

// ---- pcapng option walk (ts_resol / epb_flags) ---------------------------------------------------
// nm_pcap.hpp's BlockRef gives us the block's offset/length/endianness but not its option area, so we
// walk the trailing option TLVs here (the one thing pcapng2lance reads that the parity scanner leaves
// out). Options are `code:u16 len:u16 value[len]` padded to 4 bytes, terminated by code 0 (opt_endofopt).
void walk_options(nm::bytes file, std::uint64_t opt_off, std::size_t opt_size, bool little,
                  const auto& on_option) {
    if (opt_off + opt_size > file.size()) return;
    nm::input in = nm::from(file).advance(static_cast<std::size_t>(opt_off));
    std::size_t remaining = opt_size;
    const auto rd16 = [&](nm::input& i) { return little ? nm::le_u16(i) : nm::be_u16(i); };
    while (remaining >= 4) {
        auto code = rd16(in);
        auto len = rd16(code->rest);
        const std::uint16_t c = code->value, l = len->value;
        const std::size_t padded = (static_cast<std::size_t>(l) + 3U) & ~std::size_t{3};
        if (c == 0 /*opt_endofopt*/ || 4U + padded > remaining) break;
        on_option(c, len->rest.take_span(l));
        in = len->rest.advance(padded);
        remaining -= 4U + padded;
    }
}

// ts_resol lives in the IDB (if_tsresol, code 9); default 0x06 (microseconds). A synthetic IDB over a
// legacy-pcap global header (length 24) has no options — its resolution rode in on the file magic, which
// nm_pcap folds into the record timestamps, so 0x06/0x09 is not re-derived here (matches pcapng2lance:
// legacy captures carry ts_resol via the magic, reported as-is by the scanner).
std::uint8_t idb_ts_resol(nm::bytes file, const nmpcap::BlockRef& ref) {
    std::uint8_t res = 0x06;
    if (ref.length == 24) return res;  // synthetic IDB (legacy pcap): no option area
    const std::uint64_t opt_off = ref.file_offset + 16U;  // frame(8) + link/reserved/snaplen(8)
    if (ref.length < 20U) return res;
    walk_options(file, opt_off, ref.length - 16U - 4U, ref.little_endian,
                 [&](std::uint16_t code, nm::bytes v) {
                     if (code == 9 /*if_tsresol*/ && !v.empty()) res = std::uint8_t(v[0]);
                 });
    return res;
}

// epb_flags lives in the EPB option area (code 2); 0 when absent. Legacy pcap records have no options.
std::uint32_t epb_flags(nm::bytes file, const nmpcap::BlockRef& ref, std::uint32_t caplen) {
    if (ref.kind != nmpcap::Kind::Epb) return 0;
    const std::size_t data_padded = (static_cast<std::size_t>(caplen) + 3U) & ~std::size_t{3};
    const std::uint64_t opt_off = ref.file_offset + 8U + 20U + data_padded;
    if (opt_off + 4U > ref.file_offset + ref.length) return 0;
    std::uint32_t flags = 0;
    walk_options(file, opt_off, ref.length - (8U + 20U + data_padded) - 4U, ref.little_endian,
                 [&](std::uint16_t code, nm::bytes v) {
                     if (code == 2 /*epb_flags*/ && v.size() >= 4) {
                         std::uint32_t x = 0;
                         std::memcpy(&x, v.data(), 4);
                         if (ref.little_endian != (std::endian::native == std::endian::little)) {
                             x = __builtin_bswap32(x);
                         }
                         flags = x;
                     }
                 });
    return flags;
}

// ---- command line --------------------------------------------------------------------------------

struct Args {
    bool compress = true;
    bool no_write = false;     // scan+parse only, skip the Lance write (isolates Phase A/B for benchmarking)
    bool decode_l2l3 = false;  // also decode L2/L3/L4 via nanom walk_packet -> per-PDU Lance tables
    std::string stage;                      // --stage l1: the packet table into a data folder (see nanom_convert_main)
    std::size_t window_bytes = std::size_t{16} * 1024 * 1024;  // --window-bytes: the capture is read and committed this many bytes at a time
    std::uint64_t mem_bytes = 0;            // --mem-bytes: the enrich memory budget (0 = a share of free RAM)
    std::size_t read_tile_bytes = std::size_t{32} * 1024 * 1024;  // --read-tile-bytes: one fetch of the enrich read
    std::uint64_t drop = 0;                 // -d: skip the first N packets (their packet_id is kept for the rest)
    std::uint64_t take = UINT64_MAX;        // -c: emit at most N packets after the drop (default: all)
    std::vector<std::string> pos;
};

bool parse_args(int argc, char** argv, Args& a, std::string& err) {
    auto value = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            err = std::string(argv[i]) + " requires a value";
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--no-compress") {
            a.compress = false;
        } else if (s == "--no-write") {
            a.no_write = true;
        } else if (s == "--decode-l2l3") {
            a.decode_l2l3 = true;
        } else if (s == "--stage") {
            const char* v = value(i);
            if (!v) return false;
            a.stage = v;
        } else if (s == "--window-bytes") {
            const char* v = value(i);
            if (!v) return false;
            a.window_bytes = static_cast<std::size_t>(std::stoull(v));
            if (a.window_bytes == 0) return (err = "--window-bytes must be > 0", false);
        } else if (s == "--sequential") {
            // This converter reads the capture on one thread; the flag is accepted for compatibility.
        } else if (s == "--mem-bytes") {
            const char* v = value(i);
            if (!v) return false;
            a.mem_bytes = std::stoull(v);
        } else if (s == "--read-tile-bytes") {
            const char* v = value(i);
            if (!v) return false;
            a.read_tile_bytes = static_cast<std::size_t>(std::stoull(v));
        } else if (s == "--threads") {
            // This converter reads the capture on one thread and writes the same tables whatever the count.
            if (!value(i)) return false;
        } else if (s == "-d" || s == "--drop" || s == "-c" || s == "--count") {
            const char* v = value(i);
            if (!v) return false;
            (s == "-d" || s == "--drop" ? a.drop : a.take) = std::stoull(v);
        } else if (!s.empty() && s[0] == '-') {
            err = "unknown option '" + s + "'";
            return false;
        } else {
            a.pos.push_back(s);
        }
    }
    return true;
}

// ---- the SHB/IDB options as dataset metadata ------------------------------------------------------
//
// pcapng2lance records the string options of each section header and interface block as key/value metadata
// of the packet table's first field: "pcapng:shb<k>:<name>" and "pcapng:if<j>:<name>". They are read from the first
// window's blocks, as the nanotins converter reads them from its first scan window.

std::uint32_t read_u32(const std::vector<std::uint8_t>& b, std::size_t at, bool le) {
    const std::uint8_t* p = b.data() + at;
    return le ? std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24
              : std::uint32_t(p[3]) | std::uint32_t(p[2]) << 8 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[0]) << 24;
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& b, std::size_t at, bool le) {
    const std::uint8_t* p = b.data() + at;
    return le ? std::uint16_t(p[0] | p[1] << 8) : std::uint16_t(p[1] | p[0] << 8);
}

// The name of each option code kept, for a section header (SHB) or an interface description (IDB).
const char* shb_option_name(std::uint16_t code) {
    switch (code) {
        case 2: return "hardware";
        case 3: return "os";
        case 4: return "userappl";
        default: return nullptr;
    }
}
const char* idb_option_name(std::uint16_t code) {
    switch (code) {
        case 2: return "name";
        case 3: return "description";
        case 12: return "os";
        default: return nullptr;
    }
}

// Append the options in [at, end) whose codes have a name, under `prefix`.
void collect_options(const std::vector<std::uint8_t>& b, std::size_t at, std::size_t end, bool le,
                     const std::string& prefix, const char* (*name_of)(std::uint16_t),
                     std::vector<std::pair<std::string, std::string>>& out) {
    while (at + 4 <= end) {
        const std::uint16_t code = read_u16(b, at, le);
        const std::uint16_t len = read_u16(b, at + 2, le);
        if (code == 0) break;  // opt_endofopt
        if (at + 4 + len > end) break;
        if (const char* name = name_of(code)) {
            out.emplace_back(prefix + name, std::string(reinterpret_cast<const char*>(b.data() + at + 4), len));
        }
        at += 4 + ((static_cast<std::size_t>(len) + 3) & ~std::size_t{3});
    }
}

// The KV entries of every SHB and IDB of a pcapng capture, in file order. A pcap file has none.
std::vector<std::pair<std::string, std::string>> pcapng_option_entries(const std::vector<std::uint8_t>& b) {
    std::vector<std::pair<std::string, std::string>> out;
    constexpr std::uint32_t kShb = 0x0A0D0D0A, kIdb = 1, kMagic = 0x1A2B3C4D;
    std::size_t pos = 0;
    bool le = true;
    std::size_t shb_index = 0, idb_index = 0;
    if (b.size() < 12 || read_u32(b, 0, true) != kShb) return out;  // not pcapng: no section header first
    while (pos + 12 <= b.size()) {
        const std::uint32_t type = read_u32(b, pos, le);
        if (type == kShb) {
            le = read_u32(b, pos + 8, true) == kMagic;  // byte order of this section
        }
        const std::uint32_t len = read_u32(b, pos + 4, le);
        if (len < 12 || len % 4 != 0 || pos + len > b.size()) break;
        if (type == kShb) {
            if (len >= 28) {
                collect_options(b, pos + 24, pos + len - 4, le, "pcapng:shb" + std::to_string(shb_index) + ":",
                                shb_option_name, out);
            }
            ++shb_index;
        } else if (type == kIdb) {
            if (len >= 20) {
                collect_options(b, pos + 16, pos + len - 4, le, "pcapng:if" + std::to_string(idb_index) + ":",
                                idb_option_name, out);
            }
            ++idb_index;
        }
        pos += len;  // packets and other blocks are skipped by their length
    }
    return out;
}

// Arrow's binary metadata encoding (ArrowSchema.metadata): int32 count, then int32 length and bytes of each key
// and of each value. Little endian, as the rest of the Arrow C data interface here.
std::string arrow_metadata(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    auto put32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    put32(static_cast<std::uint32_t>(kv.size()));
    for (const auto& [k, v] : kv) {
        put32(static_cast<std::uint32_t>(k.size()));
        out += k;
        put32(static_cast<std::uint32_t>(v.size()));
        out += v;
    }
    return out;
}

// ---- --stage l2: the Ethernet and VLAN headers of a data folder's packet table -----------------------
//
// The staged path reads <datadir>/packets.lance, fetches the first bytes of each packet from the capture by its
// external reference, decodes the link layer, and writes ethernet.lance, vlan.lance and remainder_after_l2.lance
// beside it.

constexpr std::size_t kPrefixCap = 256;                 // header bytes carved per packet
constexpr std::size_t kReadTile = std::size_t{32} << 20;  // one fetch covers consecutive packets of one capture

struct EnrichRow {
    std::uint64_t pid = 0;
    std::uint64_t disc = 0;  // link_type for the link layer
    std::uint64_t off = 0;   // byte offset of the packet in the capture
    std::uint64_t size = 0;  // captured length
    std::string uri;
};

int child_index(const ArrowSchema& s, const char* name) {
    for (std::int64_t i = 0; i < s.n_children; ++i) {
        if (s.children[i]->name != nullptr && std::strcmp(s.children[i]->name, name) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ---- writer sessions for a table written in chunks ----------------------------------------------------------
//
// The staged stages commit each chunk of rows as one fragment of its table. A session holds one writer for the
// table, opened on the first rows it is given; flush() commits what it holds as the next fragment and clears it.
// A table that never gets rows is never created.

template <class Row>
class SoaSession {
public:
    SoaSession(std::string path, bool compress) : path_(std::move(path)), compress_(compress) {}

    bool flush(nm::soa<Row>& table, std::string& err) {
        if (table.rows() == 0) return true;
        if (!writer_) {
            writer_ = std::make_unique<Writer>(path_.c_str(), typename Writer::options{.compression_level = 3,
                                                                                    .compress = compress_});
            if (!writer_->ok()) {
                err = std::string("writer init: ") + writer_->last_error();
                return false;
            }
        }
        bool ok = true;
        table.for_each_chunk([&](const auto& ch) {
            ok = ok && p2l_nanom::detail::write_chunk<Row>(*writer_, ch,
                                                            std::make_index_sequence<p2l_nanom::detail::field_count<Row>>{});
        });
        ok = ok && writer_->commit();
        if (!ok) err = std::string("write ") + path_ + ": " + writer_->last_error();
        table = nm::soa<Row>{4096};
        return ok;
    }

    void close() {
        if (writer_) writer_->close();
        writer_.reset();
    }

private:
    using Writer = nt::writer<p2l_nanom::detail::schema_for<Row>>;
    std::string path_;
    bool compress_;
    std::unique_ptr<Writer> writer_;
};

class RemainderSession {
public:
    RemainderSession(std::string path, bool compress) : path_(std::move(path)), compress_(compress) {}

    bool flush(std::vector<std::uint64_t>& pid, std::vector<std::uint64_t>& next, std::vector<nt::blob_ref>& pay,
               std::string& err) {
        if (pid.empty()) return true;
        if (!writer_) {
            writer_ = std::make_unique<nt::writer<RemainderSchema>>(path_.c_str(), nt::writer<RemainderSchema>::options{
                                                                        .compression_level = 3, .compress = compress_});
            if (!writer_->ok()) {
                err = std::string("remainder writer init: ") + writer_->last_error();
                return false;
            }
        }
        const bool ok = writer_->write_batch(std::span<const std::uint64_t>(pid), std::span<const std::uint64_t>(next),
                                             std::span<const nt::blob_ref>(pay)) &&
                        writer_->commit();
        if (!ok) err = std::string("remainder write: ") + writer_->last_error();
        pid.clear();
        next.clear();
        pay.clear();
        return ok;
    }

    void close() {
        if (writer_) writer_->close();
        writer_.reset();
    }

private:
    std::string path_;
    bool compress_;
    std::unique_ptr<nt::writer<RemainderSchema>> writer_;
};

// ---- the converter -------------------------------------------------------------------------------

// The IPv4 options of the header at `off` (IHL in 32-bit words): on_opt(type, on-wire length) for each, the walk of
// the ipv4_option table. The option space is [off + 20, off + IHL*4) clamped to the packet; EOOL ends the
// list; NOP is one byte; every other option's length octet counts the whole option, and an option that does not fit
// ends the walk.
template <class OnOpt>
void ipv4_options(nm::bytes pkt, std::size_t off, std::uint8_t ihl, OnOpt on_opt) {
    const std::size_t hdr = std::size_t(ihl) * 4;
    if (hdr <= 20 || off > pkt.size()) return;
    const std::size_t end = std::min<std::size_t>(off + hdr, pkt.size());
    std::size_t p = off + 20;
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(pkt[i]); };
    while (p < end) {
        const std::uint8_t t = byte(p);
        if (t == 0) {  // EOOL
            on_opt(t, std::uint8_t{1});
            return;
        }
        if (t == 1) {  // NOP
            on_opt(t, std::uint8_t{1});
            ++p;
            continue;
        }
        if (p + 2 > end) return;
        const std::uint8_t wlen = byte(p + 1);  // counts the whole option
        if (wlen < 2 || p + wlen > end) return;
        on_opt(t, wlen);
        p += wlen;
    }
}

// ---- the input, window by window ----------------------------------------------------------------------------------
//
// The capture is read as windows of whole blocks: a window is the next run of complete blocks, at most `budget` bytes
// (a block larger than the budget is a window of its own). The first window starts at the file's first block. Every
// later window starts with a copy of the file's first block (the section header of a pcapng, the global header of a pcap),
// so nanom's scanner, which reads the byte order and link type from the start of the bytes it is given, reads each
// window as it would the whole file. The converter keeps no more than one window of the capture in memory.

class CaptureWindows {
public:
    bool open(const fs::path& path, std::size_t budget, std::string& err) {
        f_.reset(std::fopen(path.string().c_str(), "rb"));
        if (!f_) {
            err = "cannot read input file: " + path.string();
            return false;
        }
        budget_ = budget;
        if (std::fseek(f_.get(), 0, SEEK_END) != 0) return (err = "cannot seek in " + path.string(), false);
        size_ = static_cast<std::uint64_t>(std::ftell(f_.get()));
        std::uint8_t magic_bytes[4]{};
        if (size_ < 4 || !read_at(0, magic_bytes, 4)) return (err = "file too small", false);
        const std::uint32_t magic = read_u32(magic_bytes, true);
        if (magic == nmpcap::kShb) {
            pcapng_ = true;
            std::uint8_t bom[4]{};
            if (!read_at(8, bom, 4)) return (err = "truncated section header", false);
            le_ = read_u32(bom, true) == nmpcap::kByteOrderMagic;
        } else if (magic == nmpcap::kMagicUsLE || magic == nmpcap::kMagicNsLE) {
            pcapng_ = false;
            le_ = true;
        } else if (magic == nmpcap::kMagicUsBE || magic == nmpcap::kMagicNsBE) {
            pcapng_ = false;
            le_ = false;
        } else {
            return (err = "not a pcap or pcapng file (unknown magic)", false);
        }
        std::uint64_t first = 0;
        if (!block_length(0, first, err)) return false;
        prefix_.resize(static_cast<std::size_t>(first));
        if (!read_at(0, prefix_.data(), prefix_.size())) return (err = "truncated first block", false);
        return true;
    }

    // The next window: its bytes (the prefix copy, then the blocks) and where the window starts in the file, and how
    // many leading bytes of the window are the prefix. False at the end of the file, or on error with `err` set.
    bool next(std::vector<std::uint8_t>& out, std::uint64_t& start, std::size_t& prefix_len, std::string& err) {
        if (pos_ >= size_) return false;
        start = pos_;
        std::uint64_t end = pos_;
        while (end < size_) {
            std::uint64_t total = 0;
            if (!block_length(end, total, err)) return false;
            if (end + total > size_) return (err = "truncated block at offset " + std::to_string(end), false);
            if (end > pos_ && (end - pos_) + total > budget_) break;  // this block starts the next window
            end += total;
        }
        prefix_len = start == 0 ? 0 : prefix_.size();
        out.resize(prefix_len + static_cast<std::size_t>(end - start));
        if (prefix_len != 0) std::memcpy(out.data(), prefix_.data(), prefix_len);
        if (!read_at(start, out.data() + prefix_len, static_cast<std::size_t>(end - start))) {
            return (err = "cannot read the capture at offset " + std::to_string(start), false);
        }
        pos_ = end;
        return true;
    }

private:
    static std::uint32_t read_u32(const std::uint8_t* p, bool little) {
        return little ? std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24
                      : std::uint32_t(p[3]) | std::uint32_t(p[2]) << 8 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[0]) << 24;
    }

    bool read_at(std::uint64_t at, std::uint8_t* dst, std::size_t n) {
        if (n == 0) return true;
        if (std::fseek(f_.get(), static_cast<long>(at), SEEK_SET) != 0) return false;
        return std::fread(dst, 1, n, f_.get()) == n;
    }

    // Total length of the block at `at` (a pcap's global header is 24 bytes, a pcap record 16 plus its caplen). A section
    // header sets the byte order of the blocks that follow it.
    bool block_length(std::uint64_t at, std::uint64_t& total, std::string& err) {
        if (!pcapng_ && at == 0) {
            total = 24;
            return true;
        }
        std::uint8_t h[16]{};
        const std::size_t need = pcapng_ ? 12 : 16;
        if (at + need > size_ || !read_at(at, h, need)) {
            return (err = "truncated block header at offset " + std::to_string(at), false);
        }
        if (pcapng_) {
            if (read_u32(h, true) == nmpcap::kShb) le_ = read_u32(h + 8, true) == nmpcap::kByteOrderMagic;
            const std::uint32_t len = read_u32(h + 4, le_);
            if (len < 12 || len % 4 != 0) return (err = "bad block length at offset " + std::to_string(at), false);
            total = len;
        } else {
            total = 16 + std::uint64_t(read_u32(h + 8, le_));
        }
        return true;
    }

    struct FileCloser {
        void operator()(std::FILE* f) const {
            if (f) std::fclose(f);
        }
    };
    std::unique_ptr<std::FILE, FileCloser> f_;
    std::size_t budget_ = 0;
    std::uint64_t size_ = 0, pos_ = 0;
    bool pcapng_ = false, le_ = true;
    std::vector<std::uint8_t> prefix_;
};

// The packet table: one writer session, opened by its first rows with the capture's metadata, one fragment per window.
class PacketTable {
public:
    PacketTable(std::string path, bool compress, std::string metadata)
        : path_(std::move(path)), compress_(compress), metadata_(std::move(metadata)) {}

    bool append(const std::vector<PacketRow>& rows, const std::vector<nt::blob_ref>& payload, std::string& err) {
        if (rows.empty()) return true;
        if (!writer_) {
            nt::writer<L1Schema>::options opts;
            opts.compression_level = 3;
            opts.compress = compress_;
            opts.first_column_metadata = metadata_.c_str();
            writer_ = std::make_unique<nt::writer<L1Schema>>(path_.c_str(), opts);
            if (!writer_->ok()) {
                err = std::string("writer init: ") + writer_->last_error();
                return false;
            }
        }
        return write_packet_chunk(*writer_, rows, payload, 0, rows.size(), err);
    }

    void close() {
        if (writer_) writer_->close();
        writer_.reset();
    }

private:
    // Rows [begin, end) as one batch, committed as the next fragment of the session.
    static bool write_packet_chunk(nt::writer<L1Schema>& w, const std::vector<PacketRow>& rows,
                                   const std::vector<nt::blob_ref>& payload, std::size_t begin, std::size_t end,
                                   std::string& err) {
        const std::size_t n = end - begin;
        std::vector<std::uint64_t> packet_id(n), ts_raw(n);
        std::vector<std::uint32_t> interface_id(n), caplen(n), origlen(n), epb(n);
        std::vector<std::uint16_t> link_type(n);
        std::vector<std::uint8_t> ts_resol(n);
        for (std::size_t i = 0; i < n; ++i) {
            const PacketRow& r = rows[begin + i];
            packet_id[i] = r.packet_id;
            interface_id[i] = r.interface_id;
            ts_raw[i] = r.ts_raw;
            caplen[i] = r.caplen;
            origlen[i] = r.origlen;
            link_type[i] = r.link_type;
            ts_resol[i] = r.ts_resol;
            epb[i] = r.epb_flags;
        }
        if (!w.write_batch(std::span<const std::uint64_t>(packet_id), std::span<const std::uint32_t>(interface_id),
                           std::span<const std::uint64_t>(ts_raw), std::span<const std::uint32_t>(caplen),
                           std::span<const std::uint32_t>(origlen), std::span<const std::uint16_t>(link_type),
                           std::span<const std::uint8_t>(ts_resol), std::span<const std::uint32_t>(epb),
                           std::span<const nt::blob_ref>(payload).subspan(begin, n))) {
            err = std::string("write_batch: ") + w.last_error();
            return false;
        }
        if (!w.commit()) {
            err = std::string("commit: ") + w.last_error();
            return false;
        }
        return true;
    }

    std::string path_;
    bool compress_;
    std::string metadata_;
    std::unique_ptr<nt::writer<L1Schema>> writer_;
};

// The --decode-l2l3 tables: one session per table, named from the output's stem, fed one window at a time.
struct PduSinks {
    PduSinks(const std::string& stem, bool compress)
        : eth(stem + "_ethernet.lance", compress), vlan(stem + "_vlan.lance", compress),
          ipv4(stem + "_ipv4.lance", compress), ipv4_option(stem + "_ipv4_option.lance", compress),
          ipv6(stem + "_ipv6.lance", compress), tcp(stem + "_tcp.lance", compress),
          udp(stem + "_udp.lance", compress), hopbyhop(stem + "_ipv6_hopbyhop.lance", compress),
          destopt(stem + "_ipv6_destopt.lance", compress), routing(stem + "_ipv6_routing.lance", compress),
          srh_segment(stem + "_ipv6_srh_segment.lance", compress), ipv6_option(stem + "_ipv6_option.lance", compress),
          remainder(stem + "_remainder_after_l4.lance", compress) {}

    void close() {
        eth.close();
        vlan.close();
        ipv4.close();
        ipv4_option.close();
        ipv6.close();
        tcp.close();
        udp.close();
        hopbyhop.close();
        destopt.close();
        routing.close();
        srh_segment.close();
        ipv6_option.close();
        remainder.close();
    }

    SoaSession<p2l_nanom::EthRow> eth;
    SoaSession<p2l_nanom::VlanRow> vlan;
    SoaSession<p2l_nanom::Ipv4Row> ipv4;
    SoaSession<p2l_nanom::Ipv4OptionRow> ipv4_option;
    SoaSession<p2l_nanom::Ipv6Row> ipv6;
    SoaSession<p2l_nanom::TcpRow> tcp;
    SoaSession<p2l_nanom::UdpRow> udp;
    SoaSession<p2l_nanom::Ipv6ExtOptRow> hopbyhop;
    SoaSession<p2l_nanom::Ipv6ExtOptRow> destopt;
    SoaSession<p2l_nanom::Ipv6RoutingRow> routing;
    SoaSession<p2l_nanom::Ipv6SrhSegmentRow> srh_segment;
    SoaSession<p2l_nanom::Ipv6OptionRow> ipv6_option;
    RemainderSession remainder;
};

// The payload-table columns a batch is read by.
struct PayloadCols {
    int pid = -1, disc = -1, blob = -1;
};

bool resolve_payload_cols(const ArrowSchema& schema, const char* disc_col, PayloadCols& c, std::string& error) {
    c.pid = child_index(schema, "packet_id");
    c.disc = child_index(schema, disc_col);
    c.blob = child_index(schema, "payload_ref");
    if (c.pid < 0 || c.disc < 0 || c.blob < 0) {
        error = std::string("table missing packet_id / ") + disc_col + " / payload_ref";
        return false;
    }
    return true;
}

// Append the rows of one batch of a payload table: packet_id, the discriminator, and the payload_ref's position and size.
bool append_payload_rows(const ArrowSchema& schema, ArrowArray& batch, const PayloadCols& c,
                         std::vector<EnrichRow>& out, std::string& error) {
    ArrowArrayView view{};
    ArrowError ae;
    if (ArrowArrayViewInitFromSchema(&view, &schema, &ae) != NANOARROW_OK ||
        ArrowArrayViewSetArray(&view, &batch, &ae) != NANOARROW_OK) {
        error = "failed to view payload table batch";
        ArrowArrayViewReset(&view);
        return false;
    }
    nano_lance::BlobV2ColumnView blob;
    if (!blob.init(*schema.children[c.blob], *view.children[c.blob], error)) {
        ArrowArrayViewReset(&view);
        return false;
    }
    for (std::int64_t i = 0; i < view.length; ++i) {
        EnrichRow r;
        r.pid = ArrowArrayViewGetUIntUnsafe(view.children[c.pid], i);
        r.disc = ArrowArrayViewGetUIntUnsafe(view.children[c.disc], i);
        r.off = blob.position(i);
        r.size = blob.byte_size(i);
        const char* data = nullptr;
        std::int64_t n = 0;
        blob.uri(i, &data, &n);
        r.uri.assign(data, static_cast<std::size_t>(n));
        out.push_back(std::move(r));
    }
    ArrowArrayViewReset(&view);
    return true;
}

class Converter {
public:
    Converter(Args args, fs::path output, std::string payload_uri)
        : args_(std::move(args)), output_(std::move(output)), payload_uri_(std::move(payload_uri)) {}

    // The capture, window by window. Each window's packet rows are one fragment of the packet table, and its
    // --decode-l2l3 rows one fragment of each protocol table, so only one window of the capture is in memory at a time.
    int run(const fs::path& input) {
        CaptureWindows windows;
        std::string err;
        if (!windows.open(input, args_.window_bytes, err)) return fail(err);
        const std::string stem = (output_.parent_path() / output_.stem()).string();
        if (!args_.no_write && args_.decode_l2l3) pdu_ = std::make_unique<PduSinks>(stem, args_.compress);

        std::vector<std::uint8_t> bytes;
        std::uint64_t start = 0;
        std::size_t prefix = 0;
        bool first = true;
        while (windows.next(bytes, start, prefix, err)) {
            if (first) {
                // The metadata is the SHB and IDB options of the first window (as the nanotins converter records it),
                // and the table is opened with it.
                kv_blob_ = arrow_metadata(pcapng_option_entries(bytes));
                if (!args_.no_write) packets_ = std::make_unique<PacketTable>(output_.string(), args_.compress, kv_blob_);
                first = false;
            }
            if (const int rc = process_window(bytes, start, prefix)) return rc;
        }
        if (!err.empty()) return fail(err);
        if (packets_) packets_->close();
        if (pdu_) pdu_->close();
        std::fprintf(stderr,
                     "pcapng2lance: %llu of %llu packets, %zu interface(s), %zu section(s), %zu other block(s)%s -> %s\n",
                     static_cast<unsigned long long>(rows_written_), static_cast<unsigned long long>(seen_), idb_count_,
                     shb_count_, other_count_, args_.no_write ? " [--no-write]" : "", output_.string().c_str());
        return 0;
    }

private:
    // One window of the capture: scan its blocks, tabulate its packets, and commit them and their protocol rows.
    int process_window(const std::vector<std::uint8_t>& bytes, std::uint64_t start, std::size_t prefix) {
        win_start_ = start;
        prefix_len_ = prefix;
        const nm::bytes file(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
        std::vector<nmpcap::BlockRef> refs;
        std::string err;
        if (!nmpcap::scan_blocks(file, refs, err)) return fail("scan: " + err);

        std::vector<PacketRow> rows;
        std::vector<nt::blob_ref> payload;
        for (const auto& ref : refs) {
            if (ref.file_offset < prefix) continue;  // the copy of the first block, which the first window read
            switch (ref.kind) {
                case nmpcap::Kind::Shb:
                    iface_link_.clear();
                    iface_res_.clear();
                    ++shb_count_;
                    break;
                case nmpcap::Kind::Idb: {
                    nmpcap::IdbView idb{};
                    if (nmpcap::parse_idb(file, ref, idb)) {
                        iface_link_.push_back(idb.link_type);
                        iface_res_.push_back(idb_ts_resol(file, ref));
                        ++idb_count_;
                    }
                    break;
                }
                case nmpcap::Kind::Epb:
                case nmpcap::Kind::PcapRecord: {
                    nmpcap::EpbView e{};
                    if (!nmpcap::parse_epb(file, ref, e)) continue;
                    const std::uint64_t seen = seen_++;
                    // --drop/--count: packets outside [drop, drop+take) are counted (so packet_id stays global, and
                    // slices stitch into the full dataset) but not emitted; interface state is still applied.
                    if (seen < args_.drop || seen - args_.drop >= args_.take) break;
                    const std::uint16_t link =
                        e.interface_id < iface_link_.size() ? iface_link_[e.interface_id] : std::uint16_t{0};
                    const std::uint8_t res =
                        e.interface_id < iface_res_.size() ? iface_res_[e.interface_id] : std::uint8_t{6};
                    rows.push_back(PacketRow{seen, e.interface_id, e.ts_raw, e.caplen, e.origlen, link, res,
                                             epb_flags(file, ref, e.caplen)});
                    payload.push_back(nt::blob_ref{payload_uri_, to_file(e.payload_file_offset), e.caplen});
                    if (args_.decode_l2l3) decode_packet(seen, link, file, e.payload_file_offset, e.caplen);
                    break;
                }
                default:
                    ++other_count_;
                    break;
            }
        }

        if (packets_ && !packets_->append(rows, payload, err)) return fail(err);
        rows_written_ += rows.size();
        if (args_.decode_l2l3) {
            if (pdu_) {
                if (!flush_pdu(err)) return fail("write protocol tables: " + err);
            } else {
                discard_pdu();
            }
        }
        return 0;
    }

    // A window-relative offset as an offset into the capture file.
    std::uint64_t to_file(std::uint64_t rel) const { return rel - prefix_len_ + win_start_; }

    // Commit this window's protocol rows, one fragment per table; a table with no rows is not created.
    bool flush_pdu(std::string& err) {
        return pdu_->eth.flush(eth_, err) && pdu_->vlan.flush(vlan_, err) && pdu_->ipv4.flush(ipv4_, err) &&
               pdu_->ipv4_option.flush(ipv4opt_, err) && pdu_->ipv6.flush(ipv6_, err) && pdu_->tcp.flush(tcp_, err) &&
               pdu_->udp.flush(udp_, err) && pdu_->hopbyhop.flush(hopbyhop_, err) &&
               pdu_->destopt.flush(destopt_, err) && pdu_->routing.flush(routing_, err) &&
               pdu_->srh_segment.flush(srh_segment_, err) && pdu_->ipv6_option.flush(ipv6_opt_, err) &&
               pdu_->remainder.flush(rem_pid_, rem_next_, rem_pay_, err);
    }

    // With no output, the protocol rows of a window are dropped once they are built.
    void discard_pdu() {
        eth_ = nm::soa<p2l_nanom::EthRow>{4096};
        vlan_ = nm::soa<p2l_nanom::VlanRow>{4096};
        ipv4_ = nm::soa<p2l_nanom::Ipv4Row>{4096};
        ipv6_ = nm::soa<p2l_nanom::Ipv6Row>{4096};
        tcp_ = nm::soa<p2l_nanom::TcpRow>{4096};
        udp_ = nm::soa<p2l_nanom::UdpRow>{4096};
        hopbyhop_ = nm::soa<p2l_nanom::Ipv6ExtOptRow>{4096};
        destopt_ = nm::soa<p2l_nanom::Ipv6ExtOptRow>{4096};
        routing_ = nm::soa<p2l_nanom::Ipv6RoutingRow>{4096};
        srh_segment_ = nm::soa<p2l_nanom::Ipv6SrhSegmentRow>{4096};
        ipv6_opt_ = nm::soa<p2l_nanom::Ipv6OptionRow>{4096};
        ipv4opt_ = nm::soa<p2l_nanom::Ipv4OptionRow>{4096};
        rem_pid_.clear();
        rem_next_.clear();
        rem_pay_.clear();
    }

    // ---- L2/L3/L4 decode (--decode-l2l3) ---------------------------------------------------------
    // Visitor for nanom's ext-aware walk_packet_ext. Each visited header (base layers + the IPv6
    // extension-header chain: Hop-by-Hop / SRv6 SRH / Fragment / Dest-Opts / AH) lands, with this packet's
    // id, in its PDU soa table. Every method is optional to walk_packet_ext; the ones we omit (on_fragment
    // / on_ah) are still *descended* to reach L4 — we just don't tabulate those fixed headers here.
    struct DecodeVisitor {
        Converter& c;
        std::uint64_t pid;
        nm::bytes pkt;               // the captured packet, for the IPv4 option bytes
        std::size_t l3_off = 0;      // offset of the IPv4 header: after Ethernet and the VLAN tags seen so far
        void on_eth(const nmproto::Ethernet& x) {
            c.eth_.push(p2l_nanom::make_eth(pid, x));
            l3_off = nm::wire_size_v<nmproto::Ethernet>;
        }
        void on_vlan(const nmproto::VlanTag& x) {
            c.vlan_.push(p2l_nanom::make_vlan(pid, x));
            l3_off += nm::wire_size_v<nmproto::VlanTag>;
        }
        void on_ipv4(const nmproto::Ipv4& x) {
            c.ipv4_.push(p2l_nanom::make_ipv4(pid, x));
            ipv4_options(pkt, l3_off, x.ihl, [&](std::uint8_t type, std::uint8_t len) {
                c.ipv4opt_.push(p2l_nanom::Ipv4OptionRow{pid, type, len});
            });
        }
        void on_ipv6(const nmproto::Ipv6& x) { c.ipv6_.push(p2l_nanom::make_ipv6(pid, x)); }
        void on_tcp(const nmproto::Tcp& x) { c.tcp_.push(p2l_nanom::make_tcp(pid, x)); }
        void on_udp(const nmproto::Udp& x) { c.udp_.push(p2l_nanom::make_udp(pid, x)); }
        void on_ext_opt(nmproto::Ipv6ExtKind kind, const nmproto::Ipv6ExtOpt& x) {
            (kind == nmproto::Ipv6ExtKind::hop_by_hop ? c.hopbyhop_ : c.destopt_)
                .push(p2l_nanom::make_ext_opt(pid, x));
        }
        void on_srh(const nmproto::Ipv6Srh& x) { c.routing_.push(p2l_nanom::make_ipv6_routing(pid, x)); }
        void on_srh_segment(std::uint8_t order, std::uint8_t idx, const std::array<std::uint8_t, 16>& a) {
            c.srh_segment_.push(p2l_nanom::Ipv6SrhSegmentRow{pid, order, idx, a});
        }
        void on_ipv6_option(std::uint8_t container, std::uint8_t type, std::uint8_t len) {
            c.ipv6_opt_.push(p2l_nanom::Ipv6OptionRow{pid, container, type, len});
        }
    };

public:
    // --stage l2|l3|l4 <datadir>: decode one layer of every row of the data folder's previous table, and forward the
    // payload after it. l2 reads packets.lance (the link layer: Ethernet and VLAN), l3 reads remainder_after_l2.lance
    // (IPv4 / IPv6) and l4 reads remainder_after_l3.lance (TCP / UDP). Each writes its PDU tables beside the input and
    // its remainder as remainder_after_<stage>.lance. The input is read a batch at a time, and each batch is decoded in
    // chunks sized from the memory budget (--mem-bytes); each chunk's rows are committed as one fragment of each table.
    // A row whose header is not fully resident, or whose layer is not decoded, produces nothing; a row that fails
    // partway keeps the header rows it already decoded.
    int run_enrich(const std::string& stage, const fs::path& datadir) {
        const bool l2 = stage == "l2";
        const bool l3 = stage == "l3";
        const fs::path table = datadir / (l2 ? "packets.lance" : l3 ? "remainder_after_l2.lance" : "remainder_after_l3.lance");
        const char* disc_col = l2 ? "link_type" : "next_protocol";
        std::string err;
        ArrowSchema schema{};
        struct SchemaGuard {
            ArrowSchema& s;
            ~SchemaGuard() {
                if (s.release) s.release(&s);
            }
        } schema_guard{schema};
        nano_lance::LanceTableStream stream;
        if (!nano_lance::LanceTableStream::open(table, nullptr, schema, stream, err)) {
            return fail("read " + table.string() + ": " + err);
        }
        PayloadCols cols;
        if (!resolve_payload_cols(schema, disc_col, cols, err)) return fail("read " + table.string() + ": " + err);

        // Rows per chunk from the memory budget of the host this stage runs on, and the fetch tile size.
        const std::size_t per_row = kPrefixCap + sizeof(nmproto::Ipv6) + sizeof(EnrichRow) + 64;
        const std::size_t chunk_rows = membudget::rows_per_chunk(membudget::resolve_budget(args_.mem_bytes), per_row);
        const std::size_t read_tile = args_.read_tile_bytes;

        const auto dir = [&](const char* name) { return (datadir / name).string(); };
        SoaSession<p2l_nanom::EthRow> eth_s(dir("ethernet.lance"), args_.compress);
        SoaSession<p2l_nanom::VlanRow> vlan_s(dir("vlan.lance"), args_.compress);
        SoaSession<p2l_nanom::Ipv4Row> ipv4_s(dir("ipv4.lance"), args_.compress);
        SoaSession<p2l_nanom::Ipv6Row> ipv6_s(dir("ipv6.lance"), args_.compress);
        SoaSession<p2l_nanom::TcpRow> tcp_s(dir("tcp.lance"), args_.compress);
        SoaSession<p2l_nanom::UdpRow> udp_s(dir("udp.lance"), args_.compress);
        RemainderSession rem_s((datadir / ("remainder_after_" + stage + ".lance")).string(), args_.compress);
        const auto flush = [&]() -> bool {
            if (l2) {
                return eth_s.flush(eth_, err) && vlan_s.flush(vlan_, err) && rem_s.flush(rem_pid_, rem_next_, rem_pay_, err);
            }
            if (l3) {
                return ipv4_s.flush(ipv4_, err) && ipv6_s.flush(ipv6_, err) && rem_s.flush(rem_pid_, rem_next_, rem_pay_, err);
            }
            return tcp_s.flush(tcp_, err) && udp_s.flush(udp_, err) && rem_s.flush(rem_pid_, rem_next_, rem_pay_, err);
        };

        std::vector<std::uint8_t> tile;
        char ferr[512]{};
        std::size_t forwarded = 0, read_rows = 0;
        const auto process = [&](const std::vector<EnrichRow>& in) -> bool {
            for (std::size_t chunk = 0; chunk < in.size(); chunk += chunk_rows) {
                const std::size_t chunk_end = std::min(chunk + chunk_rows, in.size());
                for (std::size_t i = chunk; i < chunk_end;) {
                    const std::uint64_t base = in[i].off;
                    const std::string uri = in[i].uri;
                    std::size_t j = i;
                    std::uint64_t span_end = base;
                    for (; j < chunk_end && in[j].uri == uri; ++j) {
                        const std::uint64_t need = in[j].off + std::min<std::uint64_t>(in[j].size, kPrefixCap);
                        if (j > i && need - base > read_tile) break;
                        span_end = std::max(span_end, need);
                    }
                    const std::size_t span = static_cast<std::size_t>(span_end - base);
                    tile.resize(span);
                    std::size_t got = 0;
                    if (span > 0 && nano_lance_fetch_external_blob(uri.c_str(), base, span, tile.data(), tile.size(),
                                                                   &got, ferr, sizeof ferr) != NANO_LANCE_READER_OK) {
                        err = std::string("fetch_external_blob: ") + ferr;
                        return false;
                    }
                    for (std::size_t k = i; k < j; ++k) {
                        const EnrichRow& r = in[k];
                        const std::size_t local = static_cast<std::size_t>(r.off - base);
                        const std::size_t hdr = static_cast<std::size_t>(std::min<std::uint64_t>(r.size, kPrefixCap));
                        if (local + hdr > got) continue;
                        const nm::bytes bytes(reinterpret_cast<const std::byte*>(tile.data() + local), hdr);
                        std::size_t consumed = 0;
                        std::uint64_t next = 0;
                        bool ok = false;
                        if (l2) {
                            std::uint16_t ethertype = 0;
                            ok = r.disc == nmproto::kLinkTypeEthernet && decode_l2(r.pid, bytes, consumed, ethertype);
                            next = ethertype;
                        } else if (l3) {
                            ok = decode_l3(r.pid, static_cast<std::uint16_t>(r.disc), bytes, consumed, next);
                        } else {
                            ok = decode_l4(r.pid, static_cast<std::uint8_t>(r.disc), bytes, consumed, next);
                        }
                        if (ok && consumed < r.size) {
                            rem_pid_.push_back(r.pid);
                            rem_next_.push_back(next);
                            rem_pay_.push_back(nt::blob_ref{r.uri, r.off + consumed, r.size - consumed});
                        }
                    }
                    i = j;
                }
                forwarded += rem_pid_.size();
                if (!flush()) {
                    err = "write " + stage + " chunk: " + err;
                    return false;
                }
            }
            return true;
        };

        for (;;) {
            ArrowArray batch{};
            if (!stream.next(batch, err)) return fail("read " + table.string() + ": " + err);
            if (batch.release == nullptr) break;  // no more data
            std::vector<EnrichRow> in;
            const bool read_ok = append_payload_rows(schema, batch, cols, in, err);
            batch.release(&batch);
            if (!read_ok) return fail("read " + table.string() + ": " + err);
            read_rows += in.size();
            if (!process(in)) return fail(err);
        }
        eth_s.close();
        vlan_s.close();
        ipv4_s.close();
        ipv6_s.close();
        tcp_s.close();
        udp_s.close();
        rem_s.close();
        std::fprintf(stderr,
                     "pcapng2lance: stage %s -> %zu input rows, %zu decoded forward (chunk=%zu rows, tile=%zu B)\n",
                     stage.c_str(), read_rows, forwarded, chunk_rows, read_tile);
        return 0;
    }

    // One nanom walk_packet_ext traversal per packet; the L4 boundary yields a remainder row (the application payload
    // after L4 as an external blob.v2 ref, never copied). `file` is the window the packet is in.
    void decode_packet(std::uint64_t pid, std::uint16_t link, nm::bytes file, std::uint64_t poff,
                       std::uint32_t caplen) {
        if (poff + caplen > file.size()) return;
        const nm::bytes pkt = file.subspan(static_cast<std::size_t>(poff), caplen);
        DecodeVisitor visitor{*this, pid, pkt};
        const auto wr = nmproto::walk_packet_ext(link, pkt, visitor);
        if (wr.reached_l4 && wr.l4_payload_offset < caplen) {
            rem_pid_.push_back(pid);
            rem_next_.push_back(wr.l4_ports);
            rem_pay_.push_back(nt::blob_ref{payload_uri_, to_file(poff + wr.l4_payload_offset),
                                            caplen - wr.l4_payload_offset});
        }
    }

    // The IPv4 or IPv6 header of `bytes`, as the nanotins decoder reads it. A non-first IPv4 fragment has no
    // next layer (0), like the L4 gate of the full walk.
    bool decode_l3(std::uint64_t pid, std::uint16_t ethertype, nm::bytes bytes, std::size_t& consumed,
                   std::uint64_t& next_proto) {
        consumed = 0;
        next_proto = 0;
        if (ethertype == nmproto::kEtherTypeIpv4) {
            auto ip = nm::strct<nmproto::Ipv4>()(nm::from(bytes));
            if (!ip) return false;
            ipv4_.push(p2l_nanom::make_ipv4(pid, ip->value));
            const std::size_t hdr = std::size_t(ip->value.ihl) * 4;
            consumed = hdr >= nm::wire_size_v<nmproto::Ipv4> ? hdr : nm::wire_size_v<nmproto::Ipv4>;
            next_proto = ip->value.frag_offset == 0 ? ip->value.protocol : 0;
            return true;
        }
        if (ethertype == nmproto::kEtherTypeIpv6) {
            auto ip = nm::strct<nmproto::Ipv6>()(nm::from(bytes));
            if (!ip) return false;
            ipv6_.push(p2l_nanom::make_ipv6(pid, ip->value));
            consumed = nm::wire_size_v<nmproto::Ipv6>;  // extension headers are not walked at this stage
            next_proto = ip->value.next_header;
            return true;
        }
        return false;
    }

    // The TCP or UDP header of `bytes`; next_proto is the packed src+dst ports (the L5 dispatch key).
    bool decode_l4(std::uint64_t pid, std::uint8_t ip_proto, nm::bytes bytes, std::size_t& consumed,
                   std::uint64_t& next_proto) {
        consumed = 0;
        next_proto = 0;
        if (ip_proto == nmproto::kIpProtoTcp) {
            auto tcp = nm::strct<nmproto::Tcp>()(nm::from(bytes));
            if (!tcp) return false;
            tcp_.push(p2l_nanom::make_tcp(pid, tcp->value));
            const std::size_t hdr = std::size_t(tcp->value.data_offset) * 4;
            consumed = hdr >= nm::wire_size_v<nmproto::Tcp> ? hdr : nm::wire_size_v<nmproto::Tcp>;
            next_proto = (std::uint64_t(static_cast<std::uint16_t>(tcp->value.src_port)) << 16) | static_cast<std::uint16_t>(tcp->value.dst_port);
            return true;
        }
        if (ip_proto == nmproto::kIpProtoUdp) {
            auto udp = nm::strct<nmproto::Udp>()(nm::from(bytes));
            if (!udp) return false;
            udp_.push(p2l_nanom::make_udp(pid, udp->value));
            consumed = nm::wire_size_v<nmproto::Udp>;
            next_proto = (std::uint64_t(static_cast<std::uint16_t>(udp->value.src_port)) << 16) | static_cast<std::uint16_t>(udp->value.dst_port);
            return true;
        }
        return false;
    }

    // The Ethernet header and VLAN tags at the start of `bytes`, as the nanotins decoder reads them.
    bool decode_l2(std::uint64_t pid, nm::bytes bytes, std::size_t& consumed, std::uint16_t& next_ethertype) {
        consumed = 0;
        next_ethertype = 0;
        auto eth = nm::strct<nmproto::Ethernet>()(nm::from(bytes));
        if (!eth) return false;
        eth_.push(p2l_nanom::make_eth(pid, eth->value));
        std::uint16_t ethertype = eth->value.ethertype;
        nm::input cur = eth->rest;
        std::size_t off = nm::wire_size_v<nmproto::Ethernet>;
        while (ethertype == nmproto::kEtherTypeVlan || ethertype == nmproto::kEtherTypeQinQ) {
            auto tag = nm::strct<nmproto::VlanTag>()(cur);
            if (!tag) return false;
            vlan_.push(p2l_nanom::make_vlan(pid, tag->value));
            ethertype = tag->value.inner_ethertype;
            cur = tag->rest;
            off += nm::wire_size_v<nmproto::VlanTag>;
        }
        consumed = off;
        next_ethertype = ethertype;
        return true;
    }

private:
    // --decode-l2l3 accumulators: one nanom soa per PDU type + the remainder_after_l4 columns.
    nm::soa<p2l_nanom::EthRow> eth_{4096};
    nm::soa<p2l_nanom::VlanRow> vlan_{4096};
    nm::soa<p2l_nanom::Ipv4Row> ipv4_{4096};
    nm::soa<p2l_nanom::Ipv6Row> ipv6_{4096};
    nm::soa<p2l_nanom::TcpRow> tcp_{4096};
    nm::soa<p2l_nanom::UdpRow> udp_{4096};
    // IPv6 extension-header / SRv6 tables (populated only for ext-header traffic).
    nm::soa<p2l_nanom::Ipv6ExtOptRow> hopbyhop_{4096};
    nm::soa<p2l_nanom::Ipv6ExtOptRow> destopt_{4096};
    nm::soa<p2l_nanom::Ipv6RoutingRow> routing_{4096};
    nm::soa<p2l_nanom::Ipv6SrhSegmentRow> srh_segment_{4096};
    nm::soa<p2l_nanom::Ipv6OptionRow> ipv6_opt_{4096};
    nm::soa<p2l_nanom::Ipv4OptionRow> ipv4opt_{4096};
    std::vector<std::uint64_t> rem_pid_, rem_next_;
    std::vector<nt::blob_ref> rem_pay_;
    Args args_;
    fs::path output_;
    std::string kv_blob_;  // the SHB and IDB options of the first window, as Arrow metadata
    std::string payload_uri_;
    std::uint64_t seen_ = 0;          // packets seen, dropped or kept: the global packet index (packet_id)
    std::uint64_t rows_written_ = 0;  // packets written
    std::size_t shb_count_ = 0, idb_count_ = 0, other_count_ = 0;
    std::vector<std::uint16_t> iface_link_;  // link_type per interface, in the current section
    std::vector<std::uint8_t> iface_res_;    // ts_resol per interface, in the current section
    std::uint64_t win_start_ = 0;            // where the current window starts in the file
    std::size_t prefix_len_ = 0;             // leading bytes of the window that are a copy of the first block
    std::unique_ptr<PacketTable> packets_;
    std::unique_ptr<PduSinks> pdu_;
};

}  // namespace

int nanom_convert_main(int argc, char** argv) {
    Args args;
    std::string err;
    if (!parse_args(argc, argv, args, err)) return fail(err);
    if (args.stage == "l2" || args.stage == "l3" || args.stage == "l4") {  // --stage l2|l3|l4 <datadir>
        if (args.pos.empty()) {
            std::fprintf(stderr, "usage: %s --stage l2|l3|l4 <datadir>\n", argv[0]);
            return 2;
        }
        const std::string stage = args.stage;
        const fs::path datadir = args.pos[0];
        return Converter(std::move(args), fs::path(), std::string()).run_enrich(stage, datadir);
    }
    if (args.pos.size() < 2) {
        std::fprintf(stderr,
                     "usage: %s [--no-compress] [--no-write] [--decode-l2l3] <input.pcap|pcapng> "
                     "<output.lance> [payload_uri]\n",
                     argv[0]);
        return 2;
    }
    const fs::path input = args.pos[0];
    fs::path output = args.pos[1];
    if (args.stage == "l1") {  // --stage l1 <input> <datadir>: the packet table is <datadir>/packets.lance
        std::error_code ec;
        fs::create_directories(output, ec);
        output = output / "packets.lance";
        args.decode_l2l3 = false;  // the later stages decode each layer in their own run
    } else if (!args.stage.empty()) {
        return fail("--stage " + args.stage + " is not implemented by the nanom converter");
    }
    std::string payload_uri = (args.pos.size() >= 3) ? args.pos[2] : to_file_uri(input);
    return Converter(std::move(args), std::move(output), std::move(payload_uri)).run(input);
}
