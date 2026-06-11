// pcapng -> Lance converter (step 1). Pipeline, top to bottom:
//   parse args -> (staged enrich l2/l3/l4) OR (L1Converter: stream the capture in bounded windows ->
//   scan blocks -> classify -> bulk parse_epb -> SoA scalar columns + a lance.blob.v2 payload_ref
//   (external uri+off+size) -> write a Lance fragment -> optional L2/L3/L4 decode -> per-PDU tables).
// Packet payloads are never copied: each row stores where its bytes live in the source object.

#include "mem_budget.hpp"
#include "packet_row.hpp"
#include "nanotins/arrow_glue.hpp"
#include "nanotins/bulk.hpp"
#include "nanotins/pcap_blocks.hpp"
#include "pdu_table_writer.hpp"
#include "nanotins/protocol_decode.hpp"
#include "nanotins/protocol_decode_bulk.hpp"
#include "staged_pipeline.hpp"
#include "streaming_reader.hpp"

#include "nanolance/blob_builder.hpp"
#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"

#include <exec/static_thread_pool.hpp>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <thread>
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
    std::string stage;           // "" = one-shot; l1 writes packets.lance; l2/l3/l4 enrich a data dir
    std::size_t window_bytes = std::size_t{512} * 1024 * 1024;    // L1 RAM/VRAM budget per window
    std::uint64_t mem_bytes = 0;                                  // enrich budget; 0 = auto-detect free RAM
    std::size_t read_tile_bytes = std::size_t{32} * 1024 * 1024;  // enrich big-read tile (S3 throughput)
    std::size_t prefix_cap = 256;                                 // header bytes carved per row for enrich
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
    next_disc = 0;
    return protocols::decode_l4(r.packet_id, static_cast<std::uint8_t>(r.discriminator), bytes, pdus, consumed);
}

// The six per-PDU appenders + the remainder appender, opened once; each appends a fragment per chunk.
struct EnrichTables {
    pdu_io::PduAppender<protocols::Ethernet> eth;
    pdu_io::PduAppender<protocols::VlanTag> vlan;
    pdu_io::PduAppender<protocols::Ipv4> ipv4;
    pdu_io::PduAppender<protocols::Ipv6> ipv6;
    pdu_io::PduAppender<protocols::Tcp> tcp;
    pdu_io::PduAppender<protocols::Udp> udp;
    staged::RemainderAppender remainder;

    EnrichTables(const fs::path& d, const char* rem, bool compress)
        : eth(d / "ethernet.lance", compress), vlan(d / "vlan.lance", compress),
          ipv4(d / "ipv4.lance", compress), ipv6(d / "ipv6.lance", compress), tcp(d / "tcp.lance", compress),
          udp(d / "udp.lance", compress), remainder(d / rem, "next_protocol", compress) {}

    bool append(protocols::DecodedPdus& p, std::vector<staged::PayloadRow>& rem, std::string& err) {
        return eth.append(p.ethernet, err) && vlan.append(p.vlan, err) && ipv4.append(p.ipv4, err) &&
               ipv6.append(p.ipv6, err) && tcp.append(p.tcp, err) && udp.append(p.udp, err) &&
               remainder.append(rem, err);
    }
    void close() {
        eth.close();
        vlan.close();
        ipv4.close();
        ipv6.close();
        tcp.close();
        udp.close();
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

// SoA column buffers for one window's packets (filled by the bulk parse, then by row building).
struct PacketColumns {
    std::vector<std::uint32_t> iface, caplen, origlen, psize, flags;
    std::vector<std::uint64_t> ts, poff;
    std::vector<std::uint16_t> link_type;  // resolved per packet from its section's interface table
    explicit PacketColumns(std::size_t n)
        : iface(n), caplen(n), origlen(n), psize(n), flags(n), ts(n), poff(n), link_type(n) {}
    std::size_t size() const { return iface.size(); }
};

unsigned pool_threads() {
    const unsigned hc = std::thread::hardware_concurrency();
    return hc == 0 ? 4U : hc;
}

// Streams a capture into a Lance dataset. Owns the cross-window state and the writer; one method per phase
// so the window loop reads as a short sequence of named steps.
class L1Converter {
public:
    L1Converter(Args args, fs::path output, std::string payload_uri)
        : args_(std::move(args)), output_(std::move(output)), payload_uri_(std::move(payload_uri)),
          pool_(pool_threads()) {}

    int run(const fs::path& input) {
        streaming::FileSource source(input);
        if (!source.ok()) return fail("cannot open input file: " + input.string());
        streaming::Window<streaming::FileSource> win(source, args_.window_bytes);

        std::string err;
        if (!build_schema(err)) return fail(err);
        if (const int rc = open_writer()) return rc;
        ArrowMetadataBuilderInit(&meta_, nullptr);

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
            win.fill();
        }

        ArrowBufferReset(&meta_);
        nano_lance_writer_close(&writer_);
        ArrowSchemaRelease(&schema_);
        if (args_.decode_l2l3 && global_pid_ > 0) {
            if (const int rc = write_pdu_tables()) return rc;
        }
        print_summary();
        return 0;
    }

private:
    static constexpr std::size_t kScalarCols = nanotins::column_count<PacketRow>;

    // Phase-B execution policy: parallel ex::bulk, or an in-thread loop under --sequential.
    template <class Kernel>
    void phase_b(std::size_t num_tasks, std::size_t n, const Kernel& k) {
        if (args_.sequential) {
            nanotins::serial_for_each(num_tasks, n, k);
        } else {
            nanotins::bulk_for_each(pool_.get_scheduler(), num_tasks, n, k);
        }
    }

    bool build_schema(std::string& err) {
        ArrowSchemaInit(&schema_);
        if (ArrowSchemaSetTypeStruct(&schema_, static_cast<int64_t>(kScalarCols + 1)) != NANOARROW_OK) {
            return (err = "alloc combined schema", false);
        }
        if (!nanotins::nt_fill_struct_schema<PacketRow>(&schema_, 0, err)) {
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

    // Phase B: parse each EPB BlockRef into the SoA columns via the chosen policy (the device-safe shape a
    // CUDA scheduler runs unchanged — POD captures + the window span only).
    void parse_packets(pcapblocks::Bytes wbytes, const std::vector<pcapblocks::BlockRef>& packets,
                       PacketColumns& cols) {
        const std::size_t n = packets.size();
        const pcapblocks::Bytes wb = wbytes;
        const pcapblocks::BlockRef* pk = packets.data();
        std::uint32_t* iface = cols.iface.data();
        std::uint32_t* caplen = cols.caplen.data();
        std::uint32_t* origlen = cols.origlen.data();
        std::uint32_t* psize = cols.psize.data();
        std::uint32_t* flags = cols.flags.data();
        std::uint64_t* ts = cols.ts.data();
        std::uint64_t* poff = cols.poff.data();
        const std::size_t num_tasks = std::min<std::size_t>(n, 64);
        auto run = [this](std::size_t nt, std::size_t m, const auto& k) { phase_b(nt, m, k); };
        run(num_tasks, n, [=](std::size_t i) {
            pcapblocks::EpbView v{};
            if (pcapblocks::parse_epb(wb, pk[i], v)) {
                iface[i] = v.interface_id;
                ts[i] = v.ts_raw;
                caplen[i] = v.caplen;
                origlen[i] = v.origlen;
                poff[i] = v.payload_file_offset;
                psize[i] = v.caplen;
                flags[i] = v.epb_flags;
            }
        });
    }

    // Build the PacketRow SoA, denormalizing link_type/ts_resol from each packet's section interface table.
    nanotins::soa<PacketRow> build_rows(const std::vector<std::size_t>& packet_section, PacketColumns& cols) {
        const std::size_t n = cols.size();
        nanotins::soa<PacketRow> rows;
        rows.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto& table = sections_[packet_section[i]];
            const std::uint32_t id = cols.iface[i];
            const std::uint16_t link = id < table.size() ? table[id].link_type : std::uint16_t{0};
            const std::uint8_t res = id < table.size() ? table[id].ts_resol : std::uint8_t{6};
            cols.link_type[i] = link;
            rows.store(i, PacketRow{global_pid_ + i, cols.iface[i], cols.ts[i], cols.caplen[i], cols.origlen[i],
                                    link, res, cols.flags[i]});
        }
        return rows;
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
    int write_batch(const nanotins::soa<PacketRow>& rows, const PacketColumns& cols, std::uint64_t wbase) {
        const std::size_t n = cols.size();
        ArrowArray batch{};
        if (ArrowArrayInitFromSchema(&batch, &schema_, nullptr) != NANOARROW_OK ||
            ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
            return fail("alloc combined array");
        }
        ArrowArray* payload = batch.children[kScalarCols];
        for (std::size_t i = 0; i < n; ++i) {
            if (!nanotins::nt_append_scalar_row<PacketRow>(&batch, 0, rows, i)) {
                return fail("append scalar columns");
            }
            ArrowStringView uri{payload_uri_.data(), static_cast<int64_t>(payload_uri_.size())};
            if (ArrowArrayAppendNull(payload->children[0], 1) != NANOARROW_OK ||
                ArrowArrayAppendString(payload->children[1], uri) != NANOARROW_OK ||
                ArrowArrayAppendUInt(payload->children[2], wbase + cols.poff[i]) != NANOARROW_OK ||
                ArrowArrayAppendUInt(payload->children[3], cols.psize[i]) != NANOARROW_OK) {
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
        const std::size_t n = packets.size();
        if (n == 0) return 0;  // window held only SHB/IDB/other; nothing to write

        PacketColumns cols(n);
        parse_packets(wbytes, packets, cols);
        const nanotins::soa<PacketRow> rows = build_rows(packet_section, cols);
        if (const int rc = freeze_metadata()) return rc;
        if (const int rc = write_batch(rows, cols, wbase)) return rc;
        if (args_.decode_l2l3) {
            auto run = [this](std::size_t nt, std::size_t m, const auto& k) { phase_b(nt, m, k); };
            protocols::decode_window(run, global_pid_, cols.link_type.data(), cols.poff.data(),
                                     cols.psize.data(), wbytes, n, pdus_);
        }
        global_pid_ += n;
        return 0;
    }

    // Final one-shot L2/L3 output: one Lance table per PDU type (accumulated across windows).
    int write_pdu_tables() {
        const auto stem = (output_.parent_path() / output_.stem()).string();
        std::string err;
        const auto write_one = [&](const char* suffix, auto& column) -> bool {
            const fs::path p = stem + suffix;
            if (!pdu_io::write_pdu_table(p, column, args_.compress, err)) {
                std::fprintf(stderr, "pcapng2lance: failed to write %s: %s\n", p.string().c_str(), err.c_str());
                return false;
            }
            return true;
        };
        const bool ok = write_one("_ethernet.lance", pdus_.ethernet) & write_one("_vlan.lance", pdus_.vlan) &
                        write_one("_ipv4.lance", pdus_.ipv4) & write_one("_ipv6.lance", pdus_.ipv6) &
                        write_one("_tcp.lance", pdus_.tcp) & write_one("_udp.lance", pdus_.udp);
        if (!ok) return 1;
        std::fprintf(stderr,
                     "pcapng2lance: decoded L2/L3 -> eth %zu, vlan %zu, ipv4 %zu, ipv6 %zu, tcp %zu, udp %zu\n",
                     pdus_.ethernet.size(), pdus_.vlan.size(), pdus_.ipv4.size(), pdus_.ipv6.size(),
                     pdus_.tcp.size(), pdus_.udp.size());
        return 0;
    }

    void print_summary() const {
        std::fprintf(
            stderr, "pcapng2lance: %llu packets, %zu interface(s) across %zu section(s), %zu skipped block(s) -> %s\n",
            static_cast<unsigned long long>(global_pid_), total_idb_, shb_count_, other_count_,
            output_.string().c_str());
    }

    Args args_;
    fs::path output_;
    std::string payload_uri_;
    exec::static_thread_pool pool_;

    ArrowSchema schema_{};
    NanoLanceWriter writer_{};
    ArrowBuffer meta_{};

    pcapblocks::ScanState st_{};
    std::vector<std::vector<pcapblocks::IdbView>> sections_;  // per-section interface tables
    std::ptrdiff_t cur_section_ = -1;
    std::uint64_t global_pid_ = 0;
    std::size_t shb_count_ = 0, total_idb_ = 0, other_count_ = 0;
    bool schema_meta_set_ = false;
    bool first_commit_ = true;
    protocols::DecodedPdus pdus_;  // accumulated only when --decode-l2l3
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
            "usage: %s [--no-compress] [--decode-l2l3] [--sequential] <input.pcap|pcapng> <output.lance> [payload_uri]\n"
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
