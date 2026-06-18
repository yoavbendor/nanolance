// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// pcapng -> Lance converter (step 1). Pipeline, top to bottom:
//   parse args -> (staged enrich l2/l3/l4) OR (L1Converter: stream the capture in bounded windows ->
//   scan blocks -> classify -> bulk parse_epb -> SoA scalar columns + a lance.blob.v2 payload_ref
//   (external uri+off+size) -> write a Lance fragment -> optional L2/L3/L4 decode -> per-PDU tables).
// Packet payloads are never copied: each row stores where its bytes live in the source object.

#include "mem_budget.hpp"
#include "packet_row.hpp"
#include "phase_b_runner.hpp"
#include "soatins/arrow_glue.hpp"
#include "soatins/sink.hpp"
#include "nanotins/pcap_blocks.hpp"
#include "pdu_table_writer.hpp"
#include "dag_decode_window.hpp"
#include "dag_table_writer.hpp"
#include "ipv4_child_table_writer.hpp"
#include "ipv6_child_table_writer.hpp"
#include "nanotins/protocol_decode.hpp"
#include "nanotins/protocol_decode_bulk.hpp"
#include "nanotins/spec_dag.hpp"
#include "staged_pipeline.hpp"
#include "streaming_reader.hpp"

#include "nanolance/blob_builder.hpp"
#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using pcapng2lance::PacketRow;  // the L1 row schema lives in packet_row.hpp (shared, reusable)

int fail(const std::string& msg) {
    std::fprintf(stderr, "pcapng2lance: %s\n", msg.c_str());
    return 1;
}

std::string to_file_uri(const fs::path& path) {
    const auto abs = fs::absolute(path).generic_string();  // forward slashes
    return abs.size() > 1 && abs[1] == ':' ? ("file:///" + abs) : ("file://" + abs);
}

// ---- command line --------------------------------------------------------------------------------

struct Args {
    bool compress = true;
    bool decode_l2l3 = false;
    bool sequential = false;     // run Phase B in-thread (reference/debug) instead of the ex::bulk pool
    bool no_write = false;       // scan+parse+decode only, skip all Lance writes (isolates Phase B for bench)
    unsigned threads = 0;        // ex::bulk pool size; 0 = hardware_concurrency
    std::string stage;           // "" = one-shot; l1 writes packets.lance; l2/l3/l4 enrich a data dir
    std::size_t window_bytes = std::size_t{512} * 1024 * 1024;    // L1 RAM budget per window
    std::uint64_t mem_bytes = 0;                                  // enrich budget; 0 = auto-detect free RAM
    std::size_t read_tile_bytes = std::size_t{32} * 1024 * 1024;  // enrich big-read tile (S3 throughput)
    std::size_t prefix_cap = 256;                                 // header bytes carved per row for enrich
    std::uint64_t drop = 0;                       // -d: skip the first N packets (their packet_id is preserved)
    std::uint64_t take = UINT64_MAX;              // -c: emit at most N packets after the drop (default: all)
    std::vector<std::string> pos;
};

bool parse_args(int argc, char** argv, Args& a, std::string& err) {
    const auto value = [&](int& i) -> const char* {
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
        } else if (s == "--decode-l2l3") {
            a.decode_l2l3 = true;
        } else if (s == "--sequential") {
            a.sequential = true;
        } else if (s == "--no-write") {
            a.no_write = true;
        } else if (s == "--gpu") {
            // GPU bulk decode is a planned future feature: the parsers are device-callable, but the CUDA
            // executor layer (gputins) is developed separately and not built into this example yet.
            err = "--gpu (GPU bulk decode) is a planned future feature, not supported in this build; "
                  "use --threads N or --sequential";
            return false;
        } else if (s == "--threads") {
            const char* v = value(i);
            if (!v) return false;
            a.threads = static_cast<unsigned>(std::stoul(v));
        } else if (s == "--stage") {
            const char* v = value(i);
            if (!v) return false;
            a.stage = v;
        } else if (s == "--window-bytes") {
            const char* v = value(i);
            if (!v) return false;
            a.window_bytes = static_cast<std::size_t>(std::stoull(v));
            if (a.window_bytes == 0) return (err = "--window-bytes must be > 0", false);
        } else if (s == "--mem-bytes") {
            const char* v = value(i);
            if (!v) return false;
            a.mem_bytes = std::stoull(v);
        } else if (s == "--read-tile-bytes") {
            const char* v = value(i);
            if (!v) return false;
            a.read_tile_bytes = static_cast<std::size_t>(std::stoull(v));
            if (a.read_tile_bytes == 0) return (err = "--read-tile-bytes must be > 0", false);
        } else if (s == "-d" || s == "--drop") {
            const char* v = value(i);
            if (!v) return false;
            a.drop = std::stoull(v);
        } else if (s == "-c" || s == "--count") {
            const char* v = value(i);
            if (!v) return false;
            a.take = std::stoull(v);
        } else {
            a.pos.push_back(s);
        }
    }
    return true;
}

// ---- staged enrichment (--stage l2|l3|l4) --------------------------------------------------------

// Each enrich stage reads the previous stage's payload table, fetches each packet's still-unparsed bytes
// via its external reference, decodes exactly one more layer, and appends that layer's PDU tables + the
// advanced remainder. Nothing is recomputed; the data folder simply gains tables.

bool enrich_input(const fs::path& datadir, const std::string& stage, fs::path& input, const char*& disc_col,
                  std::string& err) {
    if (stage == "l2") {
        input = datadir / "packets.lance";  // discriminator column = link_type
        disc_col = "link_type";
    } else if (stage == "l3") {
        input = datadir / "remainder_after_l2.lance";  // discriminator = next_protocol (ethertype)
        disc_col = "next_protocol";
    } else if (stage == "l4") {
        input = datadir / "remainder_after_l3.lance";  // discriminator = next_protocol (ip_proto)
        disc_col = "next_protocol";
    } else {
        err = "unknown --stage '" + stage + "' (expected l1/l2/l3/l4)";
        return false;
    }
    return true;
}

const char* remainder_name(const std::string& stage) {
    return stage == "l2"   ? "remainder_after_l2.lance"
           : stage == "l3" ? "remainder_after_l3.lance"
                           : "remainder_after_l4.lance";
}

// Decode exactly one layer for one row; reports bytes consumed + the next-layer discriminator.
bool decode_layer(const std::string& stage, const staged::PayloadRow& r, protocols::Bytes bytes,
                  protocols::DecodedPdus& pdus, std::size_t& consumed, std::uint64_t& next_disc) {
    if (stage == "l2") {
        std::uint16_t et = 0;
        const bool ok = protocols::decode_l2(r.packet_id, static_cast<std::uint32_t>(r.discriminator), bytes,
                                             pdus, consumed, et);
        next_disc = et;
        return ok;
    }
    if (stage == "l3") {
        std::uint8_t proto = 0;
        const bool ok = protocols::decode_l3(r.packet_id, static_cast<std::uint16_t>(r.discriminator), bytes,
                                             pdus, consumed, proto);
        next_disc = proto;
        return ok;
    }
    return protocols::decode_l4(r.packet_id, static_cast<std::uint8_t>(r.discriminator), bytes, pdus, consumed,
                                next_disc);  // next_disc = packed src+dst ports (L5 dispatch key)
}

// The six per-PDU appenders (held as a tuple so append/close fold over them, paired 1:1 with
// DecodedPdus::columns()) + the remainder appender. Opened once; each appends a fragment per chunk.
struct EnrichTables {
    std::tuple<pdu_io::PduAppender<protocols::Ethernet>, pdu_io::PduAppender<protocols::VlanTag>,
               pdu_io::PduAppender<protocols::Ipv4>, pdu_io::PduAppender<protocols::Ipv6>,
               pdu_io::PduAppender<protocols::Tcp>, pdu_io::PduAppender<protocols::Udp>>
        apps;
    staged::RemainderAppender remainder;

    EnrichTables(const fs::path& d, const char* rem, bool compress)
        : apps(pdu_io::PduAppender<protocols::Ethernet>(d / "ethernet.lance", compress),
               pdu_io::PduAppender<protocols::VlanTag>(d / "vlan.lance", compress),
               pdu_io::PduAppender<protocols::Ipv4>(d / "ipv4.lance", compress),
               pdu_io::PduAppender<protocols::Ipv6>(d / "ipv6.lance", compress),
               pdu_io::PduAppender<protocols::Tcp>(d / "tcp.lance", compress),
               pdu_io::PduAppender<protocols::Udp>(d / "udp.lance", compress)),
          remainder(d / rem, "next_protocol", compress) {}

    bool append(protocols::DecodedPdus& p, std::vector<staged::PayloadRow>& rem, std::string& err) {
        auto cols = p.columns();  // tuple of the six PduColumn refs, in the same order as `apps`
        const bool ok = [&]<std::size_t... I>(std::index_sequence<I...>) {
            return (std::get<I>(apps).append(std::get<I>(cols), err) && ...);  // short-circuits on failure
        }(std::make_index_sequence<6>{});
        return ok && remainder.append(rem, err);
    }
    void close() {
        std::apply([](auto&... a) { (a.close(), ...); }, apps);
        remainder.close();
    }
};

// Fetch + carve + decode one [start,end) chunk: fetch consecutive same-uri rows in big tiles (S3-friendly),
// carve each row's header prefix out of the resident tile, decode one layer, collect pdus + remainder.
bool enrich_chunk(const std::string& stage, const std::vector<staged::PayloadRow>& rows, std::size_t start,
                  std::size_t end, std::size_t prefix_cap, std::size_t read_tile_bytes,
                  std::vector<std::uint8_t>& tile, protocols::DecodedPdus& pdus,
                  std::vector<staged::PayloadRow>& remainder, std::string& err) {
    char ferr[512]{};
    for (std::size_t i = start; i < end;) {
        // Grow a fetch group of consecutive same-uri rows whose byte span stays within read_tile_bytes.
        const std::uint64_t base = rows[i].off;
        const std::string& uri = rows[i].uri;
        std::size_t j = i;
        std::uint64_t span_end = base;
        for (; j < end && rows[j].uri == uri; ++j) {
            const std::uint64_t need = rows[j].off + std::min<std::uint64_t>(rows[j].size, prefix_cap);
            if (j > i && need - base > read_tile_bytes) break;
            span_end = std::max(span_end, need);
        }
        const std::size_t span = static_cast<std::size_t>(span_end - base);
        tile.resize(span);
        std::size_t got = 0;
        if (span > 0 && nano_lance_fetch_external_blob(uri.c_str(), base, span, tile.data(), tile.size(), &got,
                                                       ferr, sizeof ferr) != NANO_LANCE_READER_OK) {
            err = std::string("fetch_external_blob: ") + ferr;
            return false;
        }
        for (std::size_t k = i; k < j; ++k) {
            const staged::PayloadRow& r = rows[k];
            const std::size_t local = static_cast<std::size_t>(r.off - base);
            const std::size_t hdr = static_cast<std::size_t>(std::min<std::uint64_t>(r.size, prefix_cap));
            if (local + hdr > got) continue;  // header prefix not resident (truncated fetch) -> skip
            std::size_t consumed = 0;
            std::uint64_t next_disc = 0;
            if (decode_layer(stage, r, protocols::Bytes(tile.data() + local, hdr), pdus, consumed, next_disc) &&
                consumed < r.size) {
                remainder.push_back({r.packet_id, next_disc, r.uri, r.off + consumed, r.size - consumed});
            }
        }
        i = j;
    }
    return true;
}

int run_enrich_stage(const fs::path& datadir, const Args& a) {
    fs::path input;
    const char* disc_col = nullptr;
    std::string err;
    if (!enrich_input(datadir, a.stage, input, disc_col, err)) return fail(err);

    std::vector<staged::PayloadRow> in_rows;
    if (!staged::read_payload_table(input, disc_col, in_rows, err)) {
        return fail("read " + input.string() + ": " + err);
    }

    // Size the per-chunk bulk from THIS host's memory budget (the enrich host may differ from the chunker).
    const std::size_t per_row = a.prefix_cap + sizeof(protocols::Ipv6) + sizeof(staged::PayloadRow) + 64;
    const std::size_t chunk_rows = membudget::rows_per_chunk(membudget::resolve_budget(a.mem_bytes), per_row);

    EnrichTables tables(datadir, remainder_name(a.stage), a.compress);
    std::vector<std::uint8_t> tile;
    std::size_t forwarded = 0;
    for (std::size_t start = 0; start < in_rows.size(); start += chunk_rows) {
        const std::size_t end = std::min(start + chunk_rows, in_rows.size());
        protocols::DecodedPdus pdus;
        std::vector<staged::PayloadRow> remainder;
        if (!enrich_chunk(a.stage, in_rows, start, end, a.prefix_cap, a.read_tile_bytes, tile, pdus, remainder,
                          err) ||
            !tables.append(pdus, remainder, err)) {
            tables.close();
            return fail(err);
        }
        forwarded += remainder.size();
    }
    tables.close();
    std::fprintf(stderr,
                 "pcapng2lance: stage %s -> %zu input rows, %zu decoded forward (chunk=%zu rows, tile=%zu B)\n",
                 a.stage.c_str(), in_rows.size(), forwarded, chunk_rows, a.read_tile_bytes);
    return 0;
}

// ---- L1 windowed conversion ----------------------------------------------------------------------

// Assembled output for one window's packets: the L1 scalar columns (auto-built by soa<PacketRow> — no
// hand-rolled columns) plus the few per-packet arrays the writer + L2/L3 decoder consume as raw pointers.
struct PacketBatch {
    soatins::soa<PacketRow> rows;          // scalar columns, columnarized from PacketRow by reflection
    std::vector<std::uint16_t> link_type;   // per packet, for L2/L3 decode dispatch
    std::vector<std::uint64_t> poff;        // payload file offset, for the blob ref + the decode span
    std::vector<std::uint32_t> psize;       // payload size (== caplen), likewise
    std::size_t size() const { return link_type.size(); }
};

unsigned pool_threads(unsigned override_count) {
    if (override_count > 0) return override_count;  // --threads N
    const unsigned hc = std::thread::hardware_concurrency();
    return hc == 0 ? 4U : hc;
}

// Streams a capture into a Lance dataset. Owns the cross-window state and the writer; one method per phase
// so the window loop reads as a short sequence of named steps.
class L1Converter {
public:
    L1Converter(Args args, fs::path output, std::string payload_uri)
        : args_(std::move(args)), output_(std::move(output)), payload_uri_(std::move(payload_uri)),
          phase_b_runner_(pool_threads(args_.threads)) {}

    int run(const fs::path& input) {
        streaming::FileSource source(input);
        if (!source.ok()) return fail("cannot open input file: " + input.string());

        const std::size_t window_bytes = args_.window_bytes;
        streaming::Window<streaming::FileSource> win(source, window_bytes);

        std::string err;
        if (!args_.no_write) {
            if (!build_schema(err)) return fail(err);
            if (const int rc = open_writer()) return rc;
        }
        ArrowMetadataBuilderInit(&meta_, nullptr);
        std::fprintf(stderr, "pcapng2lance: Phase B = %s%s\n",
                     args_.sequential ? "sequential (1 thread)" : "bulk",
                     args_.sequential ? "" : (" (" + std::to_string(pool_threads(args_.threads)) + " threads)").c_str());
        if (args_.no_write) std::fprintf(stderr, "pcapng2lance: --no-write (scan+parse+decode only, no Lance output)\n");

        win.fill();
        while (win.size() > 0) {
            std::vector<pcapblocks::BlockRef> refs;
            std::size_t consumed = 0;
            if (!pcapblocks::scan_window(st_, win.bytes(), refs, consumed, win.eof(), err)) {
                return fail("scan: " + err);
            }
            if (consumed == 0) {
                if (win.eof()) break;           // no complete block remains
                if (win.full()) win.grow();     // a single block larger than the window
                win.fill();
                continue;
            }
            if (const int rc = process_window(win.bytes(), win.base(), refs)) return rc;
            win.consume(consumed);
            // --count: once every requested packet has been seen, stop reading the rest of the capture.
            if (args_.take != UINT64_MAX && global_pid_ >= args_.drop + args_.take) break;
            win.fill();
        }

        ArrowBufferReset(&meta_);
        if (!args_.no_write) {
            nano_lance_writer_close(&writer_);
            ArrowSchemaRelease(&schema_);
        }
        if (!args_.no_write && args_.decode_l2l3 && emitted_ > 0) {
            if (const int rc = write_pdu_tables()) return rc;
        }
        print_summary();
        return 0;
    }

private:
    static constexpr std::size_t kScalarCols = soatins::column_count<PacketRow>;

    // Phase-B execution policy: parallel ex::bulk, or an in-thread loop under --sequential.
    void phase_b(std::size_t num_tasks, std::size_t n, const std::function<void(std::size_t)>& k) {
        phase_b_runner_.run(args_.sequential, num_tasks, n, k);
    }

    bool build_schema(std::string& err) {
        ArrowSchemaInit(&schema_);
        if (ArrowSchemaSetTypeStruct(&schema_, static_cast<int64_t>(kScalarCols + 1)) != NANOARROW_OK) {
            return (err = "alloc combined schema", false);
        }
        if (!soatins::nt_fill_struct_schema<PacketRow>(&schema_, 0, err)) {
            return (err = "scalar schema: " + err, false);
        }
        ArrowSchema blob{};
        if (!nano_lance::build_blob_v2_payload_schema(blob, err)) {
            return (err = "blob schema: " + err, false);
        }
        ArrowSchemaRelease(schema_.children[kScalarCols]);  // swap the placeholder child for the blob struct
        std::memcpy(schema_.children[kScalarCols], &blob, sizeof(ArrowSchema));
        blob.release = nullptr;
        schema_.flags = 0;
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

    void begin_section() {
        if (cur_section_ < 0) {
            sections_.emplace_back();
            cur_section_ = 0;
        }
    }

    // Append the present, known SHB/IDB string options to the dataset KV metadata under `prefix`.
    void absorb_options(pcapblocks::Options opts, const std::string& prefix,
                        std::initializer_list<std::pair<std::uint16_t, const char*>> codes) {
        pcapblocks::Option opt{};
        while (pcapblocks::next_option(opts, opt)) {
            for (const auto& [code, name] : codes) {
                if (opt.code == code) {
                    const std::string key = prefix + name;
                    ArrowStringView k{key.c_str(), static_cast<int64_t>(key.size())};
                    ArrowStringView v{reinterpret_cast<const char*>(opt.value), opt.length};
                    ArrowMetadataBuilderAppend(&meta_, k, v);
                }
            }
        }
    }

    // Route one block into state: SHB opens a section, IDB extends the interface table, EPB/record is a
    // packet, anything else is counted. Returns false only on a fatal IDB parse failure.
    bool classify_block(pcapblocks::Bytes wbytes, const pcapblocks::BlockRef& r,
                        std::vector<pcapblocks::BlockRef>& packets, std::vector<std::size_t>& packet_section) {
        switch (r.kind) {
            case pcapblocks::Kind::Shb: {
                ++shb_count_;
                sections_.emplace_back();
                cur_section_ = static_cast<std::ptrdiff_t>(sections_.size()) - 1;
                pcapblocks::ShbView shb{};
                if (!schema_meta_set_ && pcapblocks::parse_shb(wbytes, r, shb)) {
                    absorb_options(shb.options, "pcapng:shb" + std::to_string(shb_count_ - 1) + ":",
                                   {{2, "hardware"}, {3, "os"}, {4, "userappl"}});
                }
                return true;
            }
            case pcapblocks::Kind::Idb: {
                begin_section();
                pcapblocks::IdbView idb{};
                if (!pcapblocks::parse_idb(wbytes, r, idb)) return false;
                if (!schema_meta_set_) {
                    absorb_options(idb.options, "pcapng:if" + std::to_string(total_idb_) + ":",
                                   {{2, "name"}, {3, "description"}, {12, "os"}});
                }
                sections_[static_cast<std::size_t>(cur_section_)].push_back(idb);
                ++total_idb_;
                return true;
            }
            case pcapblocks::Kind::Epb:
            case pcapblocks::Kind::PcapRecord:
                begin_section();
                packets.push_back(r);
                packet_section.push_back(static_cast<std::size_t>(cur_section_));
                return true;
            default:
                ++other_count_;
                return true;
        }
    }

    // Phase B fast path: parse each EPB and scatter its PacketRow straight into the columns in ONE pass — no
    // intermediate std::vector<EpbView>, no second materialization. soa<PacketRow>::raw() hands the column
    // pointers to soatins::scatter (the same device-view fill the bulk kernels use); each task writes
    // disjoint slots, so it parallelizes cleanly. The interface-table denormalization (link_type/ts_resol)
    // is a host lookup. Byte-identical output whether run serially or on the thread pool.
    PacketBatch parse_and_assemble(pcapblocks::Bytes wbytes, const std::vector<pcapblocks::BlockRef>& packets,
                                   const std::vector<std::size_t>& packet_section, std::uint64_t base_pid) {
        const std::size_t n = packets.size();
        PacketBatch b;
        b.rows.resize(n);
        b.link_type.resize(n);
        b.poff.resize(n);
        b.psize.resize(n);
        const pcapblocks::Bytes wb = wbytes;
        const pcapblocks::BlockRef* pk = packets.data();
        const std::size_t* sect = packet_section.data();
        const soatins::soa_ptrs<PacketRow> cols = b.rows.raw();
        std::uint16_t* lt = b.link_type.data();
        std::uint64_t* po = b.poff.data();
        std::uint32_t* ps = b.psize.data();
        const std::uint64_t pid0 = base_pid;
        auto run = [this](std::size_t nt, std::size_t m, const auto& k) { phase_b(nt, m, k); };
        run(std::min<std::size_t>(n, 64), n, [=](std::size_t i) {
            pcapblocks::EpbView e{};
            pcapblocks::EpbView parsed{};
            if (pcapblocks::parse_epb(wb, pk[i], parsed)) {
                e = parsed;  // keep e default on parse failure (matches parse_packets' out[i] semantics)
            }
            const auto& table = sections_[sect[i]];
            const std::uint16_t link =
                e.interface_id < table.size() ? table[e.interface_id].link_type : std::uint16_t{0};
            const std::uint8_t res =
                e.interface_id < table.size() ? table[e.interface_id].ts_resol : std::uint8_t{6};
            soatins::scatter(cols, i,
                             PacketRow{pid0 + i, e.interface_id, e.ts_raw, e.caplen, e.origlen, link, res,
                                       e.epb_flags});
            lt[i] = link;
            po[i] = e.payload_file_offset;
            ps[i] = e.caplen;
        });
        return b;
    }

    // Freeze the accumulated SHB/IDB KV metadata onto the schema (once, before the first commit).
    int freeze_metadata() {
        if (schema_meta_set_) return 0;
        if (ArrowSchemaSetMetadata(schema_.children[0], reinterpret_cast<const char*>(meta_.data)) !=
            NANOARROW_OK) {
            return fail("set field metadata");
        }
        schema_meta_set_ = true;
        return 0;
    }

    // Build the combined Arrow batch (scalars + external payload_ref) and commit it as its own fragment.
    int write_batch(const PacketBatch& b, std::uint64_t wbase) {
        const std::size_t n = b.size();
        ArrowArray batch{};
        if (ArrowArrayInitFromSchema(&batch, &schema_, nullptr) != NANOARROW_OK ||
            ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
            return fail("alloc combined array");
        }
        ArrowArray* payload = batch.children[kScalarCols];
        for (std::size_t i = 0; i < n; ++i) {
            if (!soatins::nt_append_scalar_row<PacketRow>(&batch, 0, b.rows, i)) {
                return fail("append scalar columns");
            }
            ArrowStringView uri{payload_uri_.data(), static_cast<int64_t>(payload_uri_.size())};
            if (ArrowArrayAppendNull(payload->children[0], 1) != NANOARROW_OK ||
                ArrowArrayAppendString(payload->children[1], uri) != NANOARROW_OK ||
                ArrowArrayAppendUInt(payload->children[2], wbase + b.poff[i]) != NANOARROW_OK ||
                ArrowArrayAppendUInt(payload->children[3], b.psize[i]) != NANOARROW_OK) {
                return fail("append payload_ref");
            }
            if (ArrowArrayFinishElement(payload) != NANOARROW_OK ||
                ArrowArrayFinishElement(&batch) != NANOARROW_OK) {
                return fail("finish element");
            }
        }
        if (ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) return fail("finalize array");
        const int wrote = nano_lance_write_batch(&writer_, &batch, &schema_);
        batch.release(&batch);
        if (wrote != NANO_LANCE_OK) return fail(std::string("write_batch: ") + nano_lance_writer_last_error(&writer_));
        if (nano_lance_writer_commit(&writer_, /*is_append=*/!first_commit_) != NANO_LANCE_OK) {
            return fail(std::string("commit: ") + nano_lance_writer_last_error(&writer_));
        }
        first_commit_ = false;
        return 0;
    }

    // One window: classify its blocks, parse the packets, write the L1 batch, optionally decode L2/L3/L4.
    int process_window(pcapblocks::Bytes wbytes, std::uint64_t wbase,
                       const std::vector<pcapblocks::BlockRef>& refs) {
        std::vector<pcapblocks::BlockRef> packets;
        std::vector<std::size_t> packet_section;
        for (const auto& r : refs) {
            if (!classify_block(wbytes, r, packets, packet_section)) {
                return fail("failed to parse interface description block");
            }
        }
        const std::size_t n_total = packets.size();
        if (n_total == 0) return 0;  // window held only SHB/IDB/other; nothing to write

        // --drop/--count packet slicing. global_pid_ is the running GLOBAL packet index (it counts every
        // packet seen, dropped or kept), so a kept packet keeps the same packet_id it would have in a full
        // run — that is what makes a stitched set of slices a bit-exact replica of the full dataset. Keep
        // only packets whose global index lies in [drop, drop+take); interface (IDB) state was already
        // applied above for every block, so dropping packets never corrupts the section tables.
        const std::uint64_t g0 = global_pid_;
        global_pid_ += n_total;  // advance over ALL packets in this window (kept or dropped)
        std::size_t lo = 0, hi = n_total;
        if (args_.drop > g0) lo = static_cast<std::size_t>(std::min<std::uint64_t>(args_.drop - g0, n_total));
        if (args_.take != UINT64_MAX) {
            const std::uint64_t end = args_.drop + args_.take;  // one past the last kept global index
            hi = end > g0 ? static_cast<std::size_t>(std::min<std::uint64_t>(end - g0, n_total)) : 0;
        }
        if (hi < lo) hi = lo;
        const std::size_t n = hi - lo;  // packets kept from this window
        if (n == 0) return 0;           // this window is entirely outside the slice
        const std::uint64_t base_pid = g0 + lo;
        if (lo != 0 || hi != n_total) {  // narrow to the kept sub-range (contiguous within the window)
            packets.erase(packets.begin() + hi, packets.end());
            packets.erase(packets.begin(), packets.begin() + lo);
            packet_section.erase(packet_section.begin() + hi, packet_section.end());
            packet_section.erase(packet_section.begin(), packet_section.begin() + lo);
        }
        emitted_ += n;

        // One fused parse+scatter pass over the window (no intermediate EpbView vector).
        PacketBatch batch = parse_and_assemble(wbytes, packets, packet_section, base_pid);
        if (!args_.no_write) {  // --no-write isolates Phase B (scan+parse+decode) from the Lance I/O
            if (const int rc = freeze_metadata()) return rc;
            if (const int rc = write_batch(batch, wbase)) return rc;
        }
        if (args_.decode_l2l3) {
            std::vector<protocols::WalkResult> trailers(n);
            auto run = [this](std::size_t nt, std::size_t m, const auto& k) { phase_b(nt, m, k); };
            pcapng2lance::dag_decode_window(run, base_pid, batch.link_type.data(), batch.poff.data(),
                                            batch.psize.data(), wbytes, n, dag_pdus_, trailers.data(),
                                            &ipv6_kids_, &ipv4_kids_);
            collect_remainder(trailers, batch, wbase, base_pid);
        }
        return 0;
    }

    // Build remainder_after_l4 rows from this window's L4 boundaries: the application payload (after L4)
    // as an external blob.v2 ref into the original capture. Only packets that reached L4 and still have
    // bytes left contribute — byte-identical to what the staged --stage l4 path emits.
    void collect_remainder(const std::vector<protocols::WalkResult>& trailers, const PacketBatch& b,
                           std::uint64_t wbase, std::uint64_t base_pid) {
        const std::size_t n = b.size();
        if (!rem_sink_) {  // lazily open the chunked remainder writer on the first contributing window
            const auto stem = (output_.parent_path() / output_.stem()).string();
            rem_appender_ = std::make_unique<staged::RemainderAppender>(stem + "_remainder_after_l4.lance",
                                                                        "next_protocol", args_.compress);
            rem_sink_ = std::make_unique<RemSink>(
                [this](soatins::soa<staged::RemainderRow, kRemainderChunk>& chunk, std::string& e) {
                    if (args_.no_write) return true;  // --no-write isolates Phase B from all Lance I/O
                    return rem_appender_->append_chunk(chunk, payload_uri_, e);
                });
        }
        for (std::size_t i = 0; i < n; ++i) {
            const protocols::WalkResult& w = trailers[i];
            if (w.reached_l4 && w.l4_payload_offset < b.psize[i]) {
                std::string e;
                if (!rem_sink_->push(staged::RemainderRow{base_pid + i, /*next_protocol=*/w.l4_ports,
                                                          wbase + b.poff[i] + w.l4_payload_offset,
                                                          b.psize[i] - w.l4_payload_offset},
                                     e)) {
                    std::fprintf(stderr, "pcapng2lance: remainder flush failed: %s\n", e.c_str());
                    rem_ok_ = false;
                }
                ++rem_count_;
            }
        }
    }

    // Final one-shot L2/L3 output: one Lance table per PDU type (accumulated across windows).
    int write_pdu_tables() {
        const auto stem = (output_.parent_path() / output_.stem()).string();
        std::string err;
        // Each DAG node's table writes to its own Lance table (spec deduced from the table type). The output
        // is byte-identical to the old protocols:: tables (see test_pdu_table_interop / _lance_interop).
        const auto write_one = [&](const char* suffix, const auto& table) -> bool {
            const fs::path p = stem + suffix;
            if (!pdu_io::write_dag_pdu_table(p, table, args_.compress, err)) {
                std::fprintf(stderr, "pcapng2lance: failed to write %s: %s\n", p.string().c_str(), err.c_str());
                return false;
            }
            return true;
        };
        using G = nanotins::L2L3Graph;
        const bool ok =
            write_one("_ethernet.lance", std::get<nanotins::node_id_v<nanotins::EthNode, G>>(dag_pdus_)) &
            write_one("_vlan.lance", std::get<nanotins::node_id_v<nanotins::VlanNode, G>>(dag_pdus_)) &
            write_one("_ipv4.lance", std::get<nanotins::node_id_v<nanotins::Ipv4Node, G>>(dag_pdus_)) &
            write_one("_ipv6.lance", std::get<nanotins::node_id_v<nanotins::Ipv6Node, G>>(dag_pdus_)) &
            write_one("_tcp.lance", std::get<nanotins::node_id_v<nanotins::TcpNode, G>>(dag_pdus_)) &
            write_one("_udp.lance", std::get<nanotins::node_id_v<nanotins::UdpNode, G>>(dag_pdus_)) &
            write_one("_gptp.lance", std::get<nanotins::node_id_v<nanotins::GptpNode, G>>(dag_pdus_)) &
            // gPTP per-message-type bodies (the GptpNode message_type sub-dispatch).
            write_one("_ptp_timestamp.lance", std::get<nanotins::node_id_v<nanotins::PtpTimestampBody, G>>(dag_pdus_)) &
            write_one("_ptp_ts_port.lance", std::get<nanotins::node_id_v<nanotins::PtpTsPortBody, G>>(dag_pdus_)) &
            write_one("_ptp_announce.lance", std::get<nanotins::node_id_v<nanotins::PtpAnnounceBody, G>>(dag_pdus_)) &
            write_one("_ptp_signaling.lance", std::get<nanotins::node_id_v<nanotins::PtpSignalingBody, G>>(dag_pdus_)) &
            // IPv6 extension headers (one fixed-field table per type; their variable parts — SRv6 segments
            // and options — go to the child tables written below).
            write_one("_ipv6_hopbyhop.lance", std::get<nanotins::node_id_v<nanotins::Ipv6HopByHopNode, G>>(dag_pdus_)) &
            write_one("_ipv6_routing.lance", std::get<nanotins::node_id_v<nanotins::Ipv6RoutingNode, G>>(dag_pdus_)) &
            write_one("_ipv6_fragment.lance", std::get<nanotins::node_id_v<nanotins::Ipv6FragmentNode, G>>(dag_pdus_)) &
            write_one("_ipv6_destopt.lance", std::get<nanotins::node_id_v<nanotins::Ipv6DestOptNode, G>>(dag_pdus_)) &
            write_one("_ipv6_ah.lance", std::get<nanotins::node_id_v<nanotins::Ipv6AhNode, G>>(dag_pdus_));
        if (!ok) return 1;
        if (!write_ipv4_child_tables(stem, err)) return 1;
        if (!write_ipv6_child_tables(stem, err)) return 1;
        // The application payload after L4 (for later UDP-internal PDU parsing), as external refs — the
        // same remainder_after_l4 table the staged --stage l4 path emits. Written incrementally through the
        // SoA sink (one shared URI, chunked flush); drain the partial tail and close here.
        if (!rem_ok_) return 1;
        if (rem_sink_ && !rem_sink_->finish(err)) {
            std::fprintf(stderr, "pcapng2lance: failed to flush remainder_after_l4: %s\n", err.c_str());
            return 1;
        }
        if (rem_appender_) {
            rem_appender_->close();
        }
        std::fprintf(
            stderr,
            "pcapng2lance: decoded L2/L3 -> eth %zu, vlan %zu, ipv4 %zu, ipv6 %zu, tcp %zu, udp %zu, gptp %zu, remainder %zu\n",
            std::get<nanotins::node_id_v<nanotins::EthNode, nanotins::L2L3Graph>>(dag_pdus_).size(),
            std::get<nanotins::node_id_v<nanotins::VlanNode, nanotins::L2L3Graph>>(dag_pdus_).size(),
            std::get<nanotins::node_id_v<nanotins::Ipv4Node, nanotins::L2L3Graph>>(dag_pdus_).size(),
            std::get<nanotins::node_id_v<nanotins::Ipv6Node, nanotins::L2L3Graph>>(dag_pdus_).size(),
            std::get<nanotins::node_id_v<nanotins::TcpNode, nanotins::L2L3Graph>>(dag_pdus_).size(),
            std::get<nanotins::node_id_v<nanotins::UdpNode, nanotins::L2L3Graph>>(dag_pdus_).size(),
            std::get<nanotins::node_id_v<nanotins::GptpNode, nanotins::L2L3Graph>>(dag_pdus_).size(), rem_count_);
        return 0;
    }

    // The IPv4 variable-length option table (accumulated across windows in ipv4_kids_): one row per IPv4
    // header option. Written lazily (no empty table).
    bool write_ipv4_child_tables(const std::string& stem, std::string& err) {
        if (!pdu_io::write_ipv4_option_table(stem + "_ipv4_option.lance", ipv4_kids_.opt, args_.compress,
                                             err)) {
            std::fprintf(stderr, "pcapng2lance: failed to write ipv4_option: %s\n", err.c_str());
            return false;
        }
        if (ipv4_kids_.opt.size() != 0) {
            std::fprintf(stderr, "pcapng2lance: IPv4 options -> %zu\n", ipv4_kids_.opt.size());
        }
        return true;
    }

    // The IPv6 variable-length child tables (accumulated across windows in ipv6_kids_): one row per SRv6
    // segment and one row per IPv6 / SRH option. Written lazily (no empty table).
    bool write_ipv6_child_tables(const std::string& stem, std::string& err) {
        if (!pdu_io::write_ipv6_srh_segment_table(stem + "_ipv6_srh_segment.lance", ipv6_kids_.srh_segment,
                                                  args_.compress, err)) {
            std::fprintf(stderr, "pcapng2lance: failed to write ipv6_srh_segment: %s\n", err.c_str());
            return false;
        }
        if (!pdu_io::write_ipv6_option_table(stem + "_ipv6_option.lance", ipv6_kids_.opt, args_.compress,
                                             err)) {
            std::fprintf(stderr, "pcapng2lance: failed to write ipv6_option: %s\n", err.c_str());
            return false;
        }
        if (ipv6_kids_.srh_segment.size() != 0 || ipv6_kids_.opt.size() != 0) {
            std::fprintf(stderr, "pcapng2lance: IPv6 children -> srh_segment %zu, option %zu\n",
                         ipv6_kids_.srh_segment.size(), ipv6_kids_.opt.size());
        }
        return true;
    }

    void print_summary() const {
        // global_pid_ is the count of packets SEEN (it spans dropped packets so packet_id stays global);
        // emitted_ is how many rows were actually written (== global_pid_ unless --drop/--count narrowed it).
        if (emitted_ != global_pid_) {
            std::fprintf(stderr, "pcapng2lance: emitted %llu of %llu packets (--drop %llu --count %s)\n",
                         static_cast<unsigned long long>(emitted_), static_cast<unsigned long long>(global_pid_),
                         static_cast<unsigned long long>(args_.drop),
                         args_.take == UINT64_MAX ? "all" : std::to_string(args_.take).c_str());
        }
        std::fprintf(
            stderr, "pcapng2lance: %llu packets, %zu interface(s) across %zu section(s), %zu skipped block(s) -> %s\n",
            static_cast<unsigned long long>(emitted_), total_idb_, shb_count_, other_count_,
            output_.string().c_str());
    }

    Args args_;
    fs::path output_;
    std::string payload_uri_;
    pcapng2lance::PhaseBRunner phase_b_runner_;

    ArrowSchema schema_{};
    NanoLanceWriter writer_{};
    ArrowBuffer meta_{};

    pcapblocks::ScanState st_{};
    std::vector<std::vector<pcapblocks::IdbView>> sections_;  // per-section interface tables
    std::ptrdiff_t cur_section_ = -1;
    std::uint64_t global_pid_ = 0;  // packets SEEN (global index; spans --drop so packet_id stays global)
    std::uint64_t emitted_ = 0;     // packets actually written (== global_pid_ unless --drop/--count)
    std::size_t shb_count_ = 0, total_idb_ = 0, other_count_ = 0;
    bool schema_meta_set_ = false;
    bool first_commit_ = true;
    nanotins::dag_tables<nanotins::L2L3Graph> dag_pdus_;  // accumulated only when --decode-l2l3 (spec/DAG)
    nanotins::ipv6_child_tables ipv6_kids_;  // SRv6 segments + IPv6/SRH options (variable child records)
    nanotins::ipv4_child_tables ipv4_kids_;  // IPv4 header options (variable child records)

    // remainder_after_l4: filled into a fixed-N SoA and flushed in chunks through the shared-URI writer
    // (no per-row uri string, bounded memory). The sink's flush is bound to rem_appender_->append_chunk.
    static constexpr std::size_t kRemainderChunk = 16384;
    using RemSink = soatins::column_sink<
        staged::RemainderRow, kRemainderChunk,
        std::function<bool(soatins::soa<staged::RemainderRow, kRemainderChunk>&, std::string&)>>;
    std::unique_ptr<staged::RemainderAppender> rem_appender_;
    std::unique_ptr<RemSink> rem_sink_;
    std::size_t rem_count_ = 0;
    bool rem_ok_ = true;
};

}  // namespace

int main(int argc, char** argv) {
    Args args;
    std::string err;
    if (!parse_args(argc, argv, args, err)) return fail(err);



    // Enrichment stages run entirely off the previously-written tables in <datadir>.
    if (args.stage == "l2" || args.stage == "l3" || args.stage == "l4") {
        if (args.pos.empty()) {
            std::fprintf(stderr, "usage: %s --stage l2|l3|l4 [--mem-bytes N] [--read-tile-bytes N] <datadir>\n",
                         argv[0]);
            return 2;
        }
        return run_enrich_stage(args.pos[0], args);
    }

    if (args.pos.size() < 2) {
        std::fprintf(
            stderr,
            "usage: %s [--no-compress] [--decode-l2l3] [--sequential|--threads N] [--no-write]\n"
            "          [-d|--drop N] [-c|--count N] <input.pcap|pcapng> <output.lance> [payload_uri]\n"
            "       (--gpu is a planned future feature; CPU bulk/sequential only for now)\n"
            "       (-d/-c select a packet slice: skip the first N, then emit at most N; packet_id stays\n"
            "        global so slices stitch into a bit-exact replica of the full dataset)\n"
            "       %s --stage l1 <input.pcap|pcapng> <datadir>   (then --stage l2|l3|l4 <datadir>)\n",
            argv[0], argv[0]);
        return 2;
    }

    const fs::path input = args.pos[0];
    fs::path output = args.pos[1];
    if (args.stage == "l1") {
        std::error_code ec;
        fs::create_directories(output, ec);  // <datadir>
        output = output / "packets.lance";
        args.decode_l2l3 = false;  // staged mode decodes each layer in its own run
    }
    std::string payload_uri = (args.pos.size() >= 3) ? args.pos[2] : to_file_uri(input);

    return L1Converter(std::move(args), std::move(output), std::move(payload_uri)).run(input);
}
