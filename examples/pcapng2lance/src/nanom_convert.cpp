// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// pcapng -> Lance converter, built on **nanom** instead of nanotins.
//
// This is the nanom sibling of examples/pcapng2lance. It produces the same L1
// output as `pcapng2lance` (one Lance row per packet: the eight scalar packet
// columns + a `lance.blob.v2` external `payload_ref` that points at the packet
// bytes in the source capture, never copied), but the whole parse side — the
// pcap/pcapng block scanner and the per-EPB field decode — runs through nanom's
// parser-combinator core (`nm::strct<T>(order)` over structs described with one
// `NANOM_DESCRIBE`) rather than nanotins' hand-rolled readers + soatins
// reflection. The Lance write side runs entirely on nanolance's compile-time
// typed facade: the L1 and remainder tables declare their schema (including the
// lance.blob.v2 payload_ref via blob_ref_column) as a type, and the per-PDU
// tables derive theirs from each row struct's NANOM_DESCRIBE.
//
// Scope: the L1 packet table (the `packets.lance` that `pcapng2lance` writes)
// plus, under --decode-l2l3, the full L2/L3/L4 protocol walk (Ethernet -> VLAN*
// -> IPv4/IPv6 -> TCP/UDP), INCLUDING the IPv6 extension-header chain / SRv6 SRH
// (Hop-by-Hop, Routing/SRH, Fragment, Dest-Opts, AH), landed as one Lance table
// per PDU type + the SRv6 segment / IPv6 option child tables + remainder_after_l4
// — byte-for-byte identical to the nanotins converter's tables. Not ported: gPTP
// / SOME/IP, staged enrichment (--stage), and windowed streaming; the capture is
// read whole. This example exists so nanom's scan+parse+decode path can be
// checked and benchmarked head-to-head against nanotins on the exact same output.
// See README.md.
//
// Pipeline: read file -> nm scan_blocks (Phase A) -> per-block classify ->
//   nm parse_epb (Phase B) + option walk (ts_resol / epb_flags) -> soa<PacketRow>
//   scalar columns + external payload_ref -> one Lance fragment; with
//   --decode-l2l3, each packet also runs nm walk_packet_ext (descending the IPv6
//   ext-header chain) -> per-PDU soa<Row> -> one Lance table per PDU type.

#include "nm_pcap.hpp"  // nanom pcap/pcapng scanner (from the vendored nanom submodule)
#include "nanom_pdu_tables.hpp"        // per-PDU Lance row types + converters (--decode-l2l3)
#include "nanom_soa_lance_writer.hpp"  // generic nanom soa<Row> -> Lance table writer

#include "nanolance/typed_writer.hpp"
#include "nanolance/blob_v2_external.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
    std::fprintf(stderr, "pcapng2lance_nanom: %s\n", msg.c_str());
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
        } else if (s == "--sequential") {
            // Accepted for the nanotins converter's command line; this converter is sequential already.
        } else if (s == "--threads") {
            // The thread count only tunes how the nanotins converter runs; this one reads the capture on one
            // thread and writes the same tables whatever it is set to.
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
// of the packet table's first field: "pcapng:shb<k>:<name>" and "pcapng:if<j>:<name>". The nanotins converter does
// this while it scans; here the whole capture is walked once, which records the same entries for every capture
// that its one-window scan also covers in full.

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
// beside it, with the rows and remainders the nanotins stage writes.

constexpr std::size_t kPrefixCap = 256;                 // header bytes carved per packet (as the nanotins stage)
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

// Read packet_id, the discriminator column and the payload_ref of every row of a Lance table, in order.
bool read_payload_rows(const fs::path& table, const char* disc_col, std::vector<EnrichRow>& out, std::string& error) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!nano_lance::lance_table_read_dataset(table, schema, batches, error)) return false;
    const auto release_all = [&] {
        for (auto& b : batches) {
            if (b.release) b.release(&b);
        }
        if (schema.release) schema.release(&schema);
    };
    const int pid_col = child_index(schema, "packet_id");
    const int disc_at = child_index(schema, disc_col);
    const int blob_col = child_index(schema, "payload_ref");
    if (pid_col < 0 || disc_at < 0 || blob_col < 0) {
        error = std::string("table missing packet_id / ") + disc_col + " / payload_ref";
        release_all();
        return false;
    }
    for (auto& batch : batches) {
        ArrowArrayView view{};
        ArrowError ae;
        if (ArrowArrayViewInitFromSchema(&view, &schema, &ae) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &ae) != NANOARROW_OK) {
            error = "failed to view payload table batch";
            ArrowArrayViewReset(&view);
            release_all();
            return false;
        }
        nano_lance::BlobV2ColumnView blob;
        if (!blob.init(*schema.children[blob_col], *view.children[blob_col], error)) {
            ArrowArrayViewReset(&view);
            release_all();
            return false;
        }
        for (std::int64_t i = 0; i < view.length; ++i) {
            EnrichRow r;
            r.pid = ArrowArrayViewGetUIntUnsafe(view.children[pid_col], i);
            r.disc = ArrowArrayViewGetUIntUnsafe(view.children[disc_at], i);
            r.off = blob.position(i);
            r.size = blob.byte_size(i);
            const char* data = nullptr;
            std::int64_t n = 0;
            blob.uri(i, &data, &n);
            r.uri.assign(data, static_cast<std::size_t>(n));
            out.push_back(std::move(r));
        }
        ArrowArrayViewReset(&view);
    }
    release_all();
    return true;
}

// ---- the converter -------------------------------------------------------------------------------

class Converter {
public:
    Converter(Args args, fs::path output, std::string payload_uri)
        : args_(std::move(args)), output_(std::move(output)), payload_uri_(std::move(payload_uri)) {}

    int run(const fs::path& input) {
        std::vector<std::uint8_t> buf;
        if (!read_file(input, buf)) return fail("cannot read input file: " + input.string());
        const nm::bytes file(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
        kv_blob_ = arrow_metadata(pcapng_option_entries(buf));

        // Phase A: scan block/record boundaries (nanom).
        std::vector<nmpcap::BlockRef> refs;
        std::string err;
        if (!nmpcap::scan_blocks(file, refs, err)) return fail("scan: " + err);

        // Phase B: classify + parse each packet, tabulate into the row buffers.
        std::vector<PacketRow> rows;
        std::vector<nt::blob_ref> payload;
        std::vector<std::uint16_t> iface_link;  // link_type per interface, in the current section
        std::vector<std::uint8_t> iface_res;    // ts_resol per interface, in the current section
        for (const auto& ref : refs) {
            switch (ref.kind) {
                case nmpcap::Kind::Shb:
                    iface_link.clear();
                    iface_res.clear();
                    ++shb_count_;
                    break;
                case nmpcap::Kind::Idb: {
                    nmpcap::IdbView idb{};
                    if (nmpcap::parse_idb(file, ref, idb)) {
                        iface_link.push_back(idb.link_type);
                        iface_res.push_back(idb_ts_resol(file, ref));
                        ++idb_count_;
                    }
                    break;
                }
                case nmpcap::Kind::Epb:
                case nmpcap::Kind::PcapRecord: {
                    nmpcap::EpbView e{};
                    if (!nmpcap::parse_epb(file, ref, e)) continue;
                    const std::uint64_t seen = seen_;
                    ++seen_;
                    // --drop/--count: packets outside [drop, drop+take) are counted (so packet_id stays global,
                    // as the nanotins converter's slices do) but not emitted; interface state is still applied.
                    if (seen < args_.drop || seen - args_.drop >= args_.take) break;
                    const std::uint16_t link =
                        e.interface_id < iface_link.size() ? iface_link[e.interface_id] : std::uint16_t{0};
                    const std::uint8_t res =
                        e.interface_id < iface_res.size() ? iface_res[e.interface_id] : std::uint8_t{6};
                    rows.push_back(PacketRow{seen, e.interface_id, e.ts_raw, e.caplen, e.origlen, link, res,
                                             epb_flags(file, ref, e.caplen)});
                    payload.push_back(nt::blob_ref{payload_uri_, e.payload_file_offset, e.caplen});
                    if (args_.decode_l2l3) decode_packet(seen, link, file, e.payload_file_offset, e.caplen);
                    break;
                }
                default:
                    ++other_count_;
                    break;
            }
        }

        // An empty slice (--drop past the last packet) writes nothing, as the nanotins converter does.
        if (!args_.no_write && !rows.empty()) {
            if (const int rc = write_batch(rows, payload)) return rc;
            if (args_.decode_l2l3) {
                if (const int rc = write_pdu_tables()) return rc;
            }
        }
        std::fprintf(stderr,
                     "pcapng2lance: %zu of %llu packets, %zu interface(s), %zu section(s), %zu other block(s)%s -> %s\n",
                     rows.size(), static_cast<unsigned long long>(seen_), idb_count_, shb_count_, other_count_,
                     args_.no_write ? " [--no-write]" : "", output_.string().c_str());
        return 0;
    }

private:
    static bool read_file(const fs::path& path, std::vector<std::uint8_t>& out) {
        std::FILE* f = std::fopen(path.string().c_str(), "rb");
        if (!f) return false;
        std::uint8_t chunk[1 << 16];
        std::size_t n = 0;
        while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) out.insert(out.end(), chunk, chunk + n);
        std::fclose(f);
        return true;
    }

    // Write the L1 packet table (8 scalar columns + external payload_ref) as one Lance fragment — the
    // same on-disk shape pcapng2lance's write_batch produces. The AoS PacketRow accumulator is
    // transposed into per-column buffers (one linear pass) and handed to the compile-time typed
    // writer; the blob_ref span carries the shared capture URI per row.
    int write_batch(const std::vector<PacketRow>& rows, const std::vector<nt::blob_ref>& payload) {
        const std::size_t n = rows.size();
        std::vector<std::uint64_t> packet_id(n), ts_raw(n);
        std::vector<std::uint32_t> interface_id(n), caplen(n), origlen(n), epb(n);
        std::vector<std::uint16_t> link_type(n);
        std::vector<std::uint8_t> ts_resol(n);
        for (std::size_t i = 0; i < n; ++i) {
            const PacketRow& r = rows[i];
            packet_id[i] = r.packet_id;
            interface_id[i] = r.interface_id;
            ts_raw[i] = r.ts_raw;
            caplen[i] = r.caplen;
            origlen[i] = r.origlen;
            link_type[i] = r.link_type;
            ts_resol[i] = r.ts_resol;
            epb[i] = r.epb_flags;
        }
        nt::writer<L1Schema>::options opts;
        opts.compression_level = 3;
        opts.compress = args_.compress;
        opts.first_column_metadata = kv_blob_.c_str();
        nt::writer<L1Schema> w(output_.string().c_str(), opts);
        if (!w.ok()) return fail(std::string("writer init: ") + w.last_error());
        if (!w.write_batch(std::span<const std::uint64_t>(packet_id),
                           std::span<const std::uint32_t>(interface_id),
                           std::span<const std::uint64_t>(ts_raw),
                           std::span<const std::uint32_t>(caplen),
                           std::span<const std::uint32_t>(origlen),
                           std::span<const std::uint16_t>(link_type),
                           std::span<const std::uint8_t>(ts_resol),
                           std::span<const std::uint32_t>(epb),
                           std::span<const nt::blob_ref>(payload))) {
            return fail(std::string("write_batch: ") + w.last_error());
        }
        if (!w.commit()) return fail(std::string("commit: ") + w.last_error());
        w.close();
        return 0;
    }

    // ---- L2/L3/L4 decode (--decode-l2l3) ---------------------------------------------------------
    // Visitor for nanom's ext-aware walk_packet_ext. Each visited header (base layers + the IPv6
    // extension-header chain: Hop-by-Hop / SRv6 SRH / Fragment / Dest-Opts / AH) lands, with this packet's
    // id, in its PDU soa table. Every method is optional to walk_packet_ext; the ones we omit (on_fragment
    // / on_ah) are still *descended* to reach L4 — we just don't tabulate those fixed headers here.
    struct DecodeVisitor {
        Converter& c;
        std::uint64_t pid;
        void on_eth(const nmproto::Ethernet& x) { c.eth_.push(p2l_nanom::make_eth(pid, x)); }
        void on_vlan(const nmproto::VlanTag& x) { c.vlan_.push(p2l_nanom::make_vlan(pid, x)); }
        void on_ipv4(const nmproto::Ipv4& x) { c.ipv4_.push(p2l_nanom::make_ipv4(pid, x)); }
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

    // One nanom walk_packet_ext traversal per packet; the L4 boundary yields a remainder row (the
    // application payload after L4 as an external blob.v2 ref, never copied).
    void decode_packet(std::uint64_t pid, std::uint16_t link, nm::bytes file, std::uint64_t poff,
                       std::uint32_t caplen) {
        if (poff + caplen > file.size()) return;
        const nm::bytes pkt = file.subspan(static_cast<std::size_t>(poff), caplen);
        DecodeVisitor visitor{*this, pid};
        const auto wr = nmproto::walk_packet_ext(link, pkt, visitor);
        if (wr.reached_l4 && wr.l4_payload_offset < caplen) {
            rem_pid_.push_back(pid);
            rem_next_.push_back(wr.l4_ports);
            rem_pay_.push_back(nt::blob_ref{payload_uri_, poff + wr.l4_payload_offset,
                                            caplen - wr.l4_payload_offset});
        }
    }

    // Write one Lance table per PDU type (via the generic nanom-soa writer) + remainder_after_l4.
    int write_pdu_tables() {
        const std::string stem = (output_.parent_path() / output_.stem()).string();
        std::string err;
        const auto w = [&](const char* suffix, const auto& table) -> bool {
            if (!p2l_nanom::write_soa_table(stem + suffix, table, args_.compress, err)) {
                fail(std::string("write ") + suffix + ": " + err);
                return false;
            }
            return true;
        };
        if (!(w("_ethernet.lance", eth_) & w("_vlan.lance", vlan_) & w("_ipv4.lance", ipv4_) &
              w("_ipv6.lance", ipv6_) & w("_tcp.lance", tcp_) & w("_udp.lance", udp_) &
              w("_ipv6_hopbyhop.lance", hopbyhop_) & w("_ipv6_destopt.lance", destopt_) &
              w("_ipv6_routing.lance", routing_) & w("_ipv6_srh_segment.lance", srh_segment_) &
              w("_ipv6_option.lance", ipv6_opt_))) {
            return 1;
        }
        if (const int rc = write_remainder_table(stem + "_remainder_after_l4.lance")) return rc;
        std::fprintf(stderr,
                     "pcapng2lance_nanom: decoded L2/L3/L4 -> eth %zu, vlan %zu, ipv4 %zu, ipv6 %zu, tcp %zu, "
                     "udp %zu, remainder %zu\n",
                     eth_.rows(), vlan_.rows(), ipv4_.rows(), ipv6_.rows(), tcp_.rows(), udp_.rows(),
                     rem_pid_.size());
        if (routing_.rows() || hopbyhop_.rows() || destopt_.rows()) {
            std::fprintf(stderr,
                         "pcapng2lance_nanom: IPv6 ext headers -> hopbyhop %zu, destopt %zu, routing %zu, "
                         "srh_segment %zu, option %zu\n",
                         hopbyhop_.rows(), destopt_.rows(), routing_.rows(), srh_segment_.rows(),
                         ipv6_opt_.rows());
        }
        return 0;
    }

    // remainder_after_l4: packet_id + next_protocol (packed L4 ports) + the external payload_ref of the
    // bytes past L4. Its own tiny typed schema (two scalars + the blob.v2 struct); skipped when empty.
    int write_remainder_table(const std::string& path) {
        if (rem_pid_.empty()) return 0;
        nt::writer<RemainderSchema> w(path.c_str(), {.compression_level = 3, .compress = args_.compress});
        if (!w.ok()) return fail(std::string("remainder writer init: ") + w.last_error());
        if (!w.write_batch(std::span<const std::uint64_t>(rem_pid_),
                           std::span<const std::uint64_t>(rem_next_),
                           std::span<const nt::blob_ref>(rem_pay_)) ||
            !w.commit()) {
            return fail(std::string("remainder write: ") + w.last_error());
        }
        w.close();
        return 0;
    }

public:
    // --stage l2|l3|l4 <datadir>: decode one layer of every row of the data folder's previous table, and forward
    // the payload after it. l2 reads packets.lance (the link layer: Ethernet and VLAN), l3 reads
    // remainder_after_l2.lance (IPv4 / IPv6) and l4 reads remainder_after_l3.lance (TCP / UDP). Each writes its
    // PDU tables beside the input and its remainder as remainder_after_<stage>.lance, with the rows and remainders
    // of the nanotins stage. A row whose header is not fully resident, or whose layer is not decoded, produces
    // nothing; a row that fails partway keeps the header rows it already decoded, as the nanotins decoders do.
    int run_enrich(const std::string& stage, const fs::path& datadir) {
        const bool l2 = stage == "l2";
        const bool l3 = stage == "l3";
        const fs::path table = datadir / (l2 ? "packets.lance" : l3 ? "remainder_after_l2.lance" : "remainder_after_l3.lance");
        std::vector<EnrichRow> in;
        std::string err;
        if (!read_payload_rows(table, l2 ? "link_type" : "next_protocol", in, err)) {
            return fail("read " + table.string() + ": " + err);
        }

        std::vector<std::uint8_t> tile;
        char ferr[512]{};
        for (std::size_t i = 0; i < in.size();) {
            const std::uint64_t base = in[i].off;
            const std::string uri = in[i].uri;
            std::size_t j = i;
            std::uint64_t span_end = base;
            for (; j < in.size() && in[j].uri == uri; ++j) {
                const std::uint64_t need = in[j].off + std::min<std::uint64_t>(in[j].size, kPrefixCap);
                if (j > i && need - base > kReadTile) break;
                span_end = std::max(span_end, need);
            }
            const std::size_t span = static_cast<std::size_t>(span_end - base);
            tile.resize(span);
            std::size_t got = 0;
            if (span > 0 && nano_lance_fetch_external_blob(uri.c_str(), base, span, tile.data(), tile.size(), &got,
                                                           ferr, sizeof ferr) != NANO_LANCE_READER_OK) {
                return fail(std::string("fetch_external_blob: ") + ferr);
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

        const auto write = [&](const char* name, const auto& soa) {
            return p2l_nanom::write_soa_table((datadir / name).string(), soa, args_.compress, err);
        };
        bool ok = l2 ? write("ethernet.lance", eth_) && write("vlan.lance", vlan_)
                     : l3 ? write("ipv4.lance", ipv4_) && write("ipv6.lance", ipv6_)
                          : write("tcp.lance", tcp_) && write("udp.lance", udp_);
        if (!ok) return fail("write " + stage + " tables: " + err);
        if (const int rc = write_remainder_table((datadir / ("remainder_after_" + stage + ".lance")).string())) {
            return rc;
        }
        std::fprintf(stderr, "pcapng2lance: stage %s -> %zu input rows, %zu decoded forward\n", stage.c_str(),
                     in.size(), rem_pid_.size());
        return 0;
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
    Args args_;
    fs::path output_;
    std::string kv_blob_;  // the SHB/IDB options as Arrow metadata, set before the packet table is written
    std::string payload_uri_;
    std::uint64_t seen_ = 0;  // packets seen, dropped or kept: the global packet index (packet_id)
    std::size_t shb_count_ = 0, idb_count_ = 0, other_count_ = 0;

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
    std::vector<std::uint64_t> rem_pid_, rem_next_;
    std::vector<nt::blob_ref> rem_pay_;
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
