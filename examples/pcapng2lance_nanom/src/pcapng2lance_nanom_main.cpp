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
// reflection. The Lance write side is unchanged: nanoarrow builds the record
// batch and nanolance writes the fragment.
//
// Scope: the L1 packet table only (the `packets.lance` that `pcapng2lance`
// writes). L2/L3/L4 PDU decoding, staged enrichment, and windowed streaming are
// not ported here — this example exists so nanom's scan+parse+tabulate path can
// be benchmarked head-to-head against nanotins on the exact same output. See
// README.md.
//
// Pipeline: read file -> nm scan_blocks (Phase A) -> per-block classify ->
//   nm parse_epb (Phase B) + option walk (ts_resol / epb_flags) -> soa<PacketRow>
//   scalar columns + external payload_ref -> one Lance fragment.

#include "nm_pcap.hpp"  // nanom pcap/pcapng scanner (from the vendored nanom submodule)

#include "nanolance/blob_builder.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace nm = nanom;

namespace {

// The L1 packet row — byte-for-byte the same schema pcapng2lance writes (see
// examples/pcapng2lance/include/packet_row.hpp). Described once for nanom; the
// nanom `soa<PacketRow>` gives the Arrow format string per column for free, but
// here we hand the columns straight to nanoarrow so the on-disk schema matches
// the nanotins example exactly (field order + Arrow types).
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
    bool no_write = false;  // scan+parse only, skip the Lance write (isolates Phase A/B for benchmarking)
    std::vector<std::string> pos;
};

bool parse_args(int argc, char** argv, Args& a, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--no-compress") {
            a.compress = false;
        } else if (s == "--no-write") {
            a.no_write = true;
        } else if (!s.empty() && s[0] == '-') {
            err = "unknown option '" + s + "'";
            return false;
        } else {
            a.pos.push_back(s);
        }
    }
    return true;
}

// ---- Arrow schema (8 scalar columns + external payload_ref), identical to pcapng2lance -----------

struct ScalarCol {
    const char* name;
    ArrowType type;
};
constexpr ScalarCol kScalars[] = {
    {"packet_id", NANOARROW_TYPE_UINT64},    {"interface_id", NANOARROW_TYPE_UINT32},
    {"ts_raw", NANOARROW_TYPE_UINT64},       {"caplen", NANOARROW_TYPE_UINT32},
    {"origlen", NANOARROW_TYPE_UINT32},      {"link_type", NANOARROW_TYPE_UINT16},
    {"ts_resol", NANOARROW_TYPE_UINT8},      {"epb_flags", NANOARROW_TYPE_UINT32},
};
constexpr std::size_t kScalarCols = sizeof(kScalars) / sizeof(kScalars[0]);

bool build_schema(ArrowSchema& schema, std::string& err) {
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, static_cast<int64_t>(kScalarCols + 1)) != NANOARROW_OK) {
        return (err = "alloc combined schema", false);
    }
    for (std::size_t i = 0; i < kScalarCols; ++i) {
        if (ArrowSchemaSetType(schema.children[i], kScalars[i].type) != NANOARROW_OK ||
            ArrowSchemaSetName(schema.children[i], kScalars[i].name) != NANOARROW_OK) {
            return (err = std::string("scalar schema: ") + kScalars[i].name, false);
        }
    }
    ArrowSchema blob{};
    if (!nano_lance::build_blob_v2_payload_schema(blob, err)) {
        return (err = "blob schema: " + err, false);
    }
    ArrowSchemaRelease(schema.children[kScalarCols]);  // swap the placeholder child for the blob struct
    std::memcpy(schema.children[kScalarCols], &blob, sizeof(ArrowSchema));
    blob.release = nullptr;
    schema.flags = 0;
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

        // Phase A: scan block/record boundaries (nanom).
        std::vector<nmpcap::BlockRef> refs;
        std::string err;
        if (!nmpcap::scan_blocks(file, refs, err)) return fail("scan: " + err);

        if (!args_.no_write) {
            if (!build_schema(schema_, err)) return fail(err);
            if (const int rc = open_writer()) return rc;
        }

        // Phase B: classify + parse each packet, tabulate into the row buffers.
        std::vector<PacketRow> rows;
        std::vector<nano_lance::BlobV2Row> payload;
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
                    const std::uint16_t link =
                        e.interface_id < iface_link.size() ? iface_link[e.interface_id] : std::uint16_t{0};
                    const std::uint8_t res =
                        e.interface_id < iface_res.size() ? iface_res[e.interface_id] : std::uint8_t{6};
                    rows.push_back(PacketRow{pid_, e.interface_id, e.ts_raw, e.caplen, e.origlen, link, res,
                                             epb_flags(file, ref, e.caplen)});
                    payload.push_back(nano_lance::BlobV2Row{/*inline_data=*/std::nullopt, payload_uri_,
                                                            e.payload_file_offset, e.caplen});
                    ++pid_;
                    break;
                }
                default:
                    ++other_count_;
                    break;
            }
        }

        if (!args_.no_write) {
            if (const int rc = write_batch(rows, payload)) return rc;
            nano_lance_writer_close(&writer_);
            ArrowSchemaRelease(&schema_);
        }
        std::fprintf(stderr,
                     "pcapng2lance_nanom: %llu packets, %zu interface(s), %zu section(s), %zu other block(s)%s -> %s\n",
                     static_cast<unsigned long long>(pid_), idb_count_, shb_count_, other_count_,
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

    int open_writer() {
        if (nano_lance_writer_init(&writer_, output_.string().c_str(), 3) != NANO_LANCE_OK) {
            return fail(std::string("writer init: ") + nano_lance_writer_last_error(&writer_));
        }
        nano_lance_writer_set_ignore_nullability(&writer_, true);  // blob.v2 data child is null for externals
        nano_lance_writer_set_compression(&writer_, args_.compress);
        return 0;
    }

    // Build the combined Arrow record batch (8 scalar columns + external payload_ref) and commit it as
    // one Lance fragment — the same on-disk shape pcapng2lance's write_batch produces.
    int write_batch(const std::vector<PacketRow>& rows, const std::vector<nano_lance::BlobV2Row>& payload) {
        const std::size_t n = rows.size();
        ArrowArray batch{};
        if (ArrowArrayInitFromSchema(&batch, &schema_, nullptr) != NANOARROW_OK ||
            ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
            return fail("alloc combined array");
        }
        ArrowArray* pay = batch.children[kScalarCols];
        for (std::size_t i = 0; i < n; ++i) {
            const PacketRow& r = rows[i];
            if (ArrowArrayAppendUInt(batch.children[0], r.packet_id) != NANOARROW_OK ||
                ArrowArrayAppendUInt(batch.children[1], r.interface_id) != NANOARROW_OK ||
                ArrowArrayAppendUInt(batch.children[2], r.ts_raw) != NANOARROW_OK ||
                ArrowArrayAppendUInt(batch.children[3], r.caplen) != NANOARROW_OK ||
                ArrowArrayAppendUInt(batch.children[4], r.origlen) != NANOARROW_OK ||
                ArrowArrayAppendUInt(batch.children[5], r.link_type) != NANOARROW_OK ||
                ArrowArrayAppendUInt(batch.children[6], r.ts_resol) != NANOARROW_OK ||
                ArrowArrayAppendUInt(batch.children[7], r.epb_flags) != NANOARROW_OK) {
                return fail("append scalar columns");
            }
            const nano_lance::BlobV2Row& b = payload[i];
            ArrowStringView uri{b.uri->data(), static_cast<int64_t>(b.uri->size())};
            if (ArrowArrayAppendNull(pay->children[0], 1) != NANOARROW_OK ||
                ArrowArrayAppendString(pay->children[1], uri) != NANOARROW_OK ||
                ArrowArrayAppendUInt(pay->children[2], b.position) != NANOARROW_OK ||
                ArrowArrayAppendUInt(pay->children[3], b.size) != NANOARROW_OK) {
                return fail("append payload_ref");
            }
            if (ArrowArrayFinishElement(pay) != NANOARROW_OK ||
                ArrowArrayFinishElement(&batch) != NANOARROW_OK) {
                return fail("finish element");
            }
        }
        if (ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) return fail("finalize array");
        const int wrote = nano_lance_write_batch(&writer_, &batch, &schema_);
        batch.release(&batch);
        if (wrote != NANO_LANCE_OK) {
            return fail(std::string("write_batch: ") + nano_lance_writer_last_error(&writer_));
        }
        if (nano_lance_writer_commit(&writer_, /*is_append=*/false) != NANO_LANCE_OK) {
            return fail(std::string("commit: ") + nano_lance_writer_last_error(&writer_));
        }
        return 0;
    }

    Args args_;
    fs::path output_;
    std::string payload_uri_;
    ArrowSchema schema_{};
    NanoLanceWriter writer_{};
    std::uint64_t pid_ = 0;
    std::size_t shb_count_ = 0, idb_count_ = 0, other_count_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    Args args;
    std::string err;
    if (!parse_args(argc, argv, args, err)) return fail(err);
    if (args.pos.size() < 2) {
        std::fprintf(stderr,
                     "usage: %s [--no-compress] [--no-write] <input.pcap|pcapng> <output.lance> [payload_uri]\n",
                     argv[0]);
        return 2;
    }
    const fs::path input = args.pos[0];
    fs::path output = args.pos[1];
    std::string payload_uri = (args.pos.size() >= 3) ? args.pos[2] : to_file_uri(input);
    return Converter(std::move(args), std::move(output), std::move(payload_uri)).run(input);
}
