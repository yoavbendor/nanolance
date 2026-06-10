// pcapng -> Lance converter (step 1). Pipeline:
//   read file -> scan_blocks (seam) -> interface table -> parse_epbs_bulk (seam) -> fill nanotins
//   SoA scalar columns + a lance.blob.v2 payload_ref (external uri+off+size) -> nano_lance write.
// Packet payloads are never copied: each row stores where its bytes live in the source file.

#include "mem_budget.hpp"
#include "nanotins/arrow_glue.hpp"
#include "nanotins/bulk.hpp"
#include "pcap_blocks.hpp"
#include "pdu_table_writer.hpp"
#include "protocol_decode.hpp"
#include "staged_pipeline.hpp"
#include "streaming_reader.hpp"

#include "nanolance/blob_builder.hpp"
#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"

#include <boost/describe.hpp>
#include <exec/static_thread_pool.hpp>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

// The all-scalar packet row that flows through the nanotins reflection core. payload_uri/off/size are
// NOT here — they ride in the lance.blob.v2 `payload_ref` struct appended alongside.
struct PacketRow {
    std::uint64_t packet_id;  // stable row id; join key for the per-PDU / staged tables
    std::uint32_t interface_id;
    std::uint64_t ts_raw;
    std::uint32_t caplen;
    std::uint32_t origlen;
    std::uint16_t link_type;  // denormalized from the interface (ConstantLayout when single-iface)
    std::uint8_t ts_resol;    // denormalized; lets a row self-describe its time unit
    std::uint32_t epb_flags;
};
BOOST_DESCRIBE_STRUCT(PacketRow, (),
                      (packet_id, interface_id, ts_raw, caplen, origlen, link_type, ts_resol, epb_flags))

std::string to_file_uri(const std::filesystem::path& path) {
    const auto abs = std::filesystem::absolute(path).generic_string();  // forward slashes
    return abs.size() > 1 && abs[1] == ':' ? ("file:///" + abs) : ("file://" + abs);
}

// Append one string option to the schema metadata builder if present (best-effort dataset KV).
void add_string_option(ArrowBuffer& meta, const std::string& key, const pcapblocks::Option& opt) {
    ArrowStringView k{key.c_str(), static_cast<int64_t>(key.size())};
    ArrowStringView v{reinterpret_cast<const char*>(opt.value), opt.length};
    ArrowMetadataBuilderAppend(&meta, k, v);
}

int fail(const std::string& msg) {
    std::fprintf(stderr, "pcapng2lance: %s\n", msg.c_str());
    return 1;
}

// One enrichment stage (l2/l3/l4): read the previous stage's payload table, fetch each packet's
// still-unparsed bytes via its external reference, decode exactly one more layer, write that layer's
// PDU tables, and write the advanced remainder for the next stage. Nothing is recomputed; the data
// folder simply gains tables. Payload bytes are never copied (the remainder points back into the
// original capture).
int run_enrich_stage(const std::filesystem::path& datadir, const std::string& stage, bool compress,
                     std::uint64_t mem_bytes, std::size_t read_tile_bytes, std::size_t prefix_cap) {
    std::filesystem::path input;
    if (stage == "l2") {
        input = datadir / "packets.lance";  // discriminator column = link_type
    } else if (stage == "l3") {
        input = datadir / "remainder_after_l2.lance";  // discriminator = next_protocol (ethertype)
    } else if (stage == "l4") {
        input = datadir / "remainder_after_l3.lance";  // discriminator = next_protocol (ip_proto)
    } else {
        return fail("unknown --stage '" + stage + "' (expected l1/l2/l3/l4)");
    }
    const char* disc_col = (stage == "l2") ? "link_type" : "next_protocol";

    std::vector<staged::PayloadRow> in_rows;
    std::string error;
    if (!staged::read_payload_table(input, disc_col, in_rows, error)) {
        return fail("read " + input.string() + ": " + error);
    }

    // Size the per-chunk bulk from THIS host's memory budget (the enrich host may differ from the L1
    // chunker). Per-row working set ~ carved header + decoded structs + a remainder row.
    const std::size_t per_row_cost = prefix_cap + sizeof(protocols::Ipv6) + sizeof(staged::PayloadRow) + 64;
    const std::uint64_t budget = membudget::resolve_budget(mem_bytes);
    const std::size_t chunk_rows = membudget::rows_per_chunk(budget, per_row_cost);

    const char* remainder_name = (stage == "l2") ? "remainder_after_l2.lance"
                                 : (stage == "l3") ? "remainder_after_l3.lance"
                                                   : "remainder_after_l4.lance";
    std::size_t total_forward = 0;
    std::vector<std::uint8_t> tile;  // resident big-read buffer, carved per row
    char ferr[512]{};

    // One open writer per output table; each appends a fragment per chunk (lazily created on first rows).
    // All six are declared; the ones the current stage never fills simply never open.
    pdu_io::PduAppender<protocols::Ethernet> eth_app(datadir / "ethernet.lance", compress);
    pdu_io::PduAppender<protocols::VlanTag> vlan_app(datadir / "vlan.lance", compress);
    pdu_io::PduAppender<protocols::Ipv4> ipv4_app(datadir / "ipv4.lance", compress);
    pdu_io::PduAppender<protocols::Ipv6> ipv6_app(datadir / "ipv6.lance", compress);
    pdu_io::PduAppender<protocols::Tcp> tcp_app(datadir / "tcp.lance", compress);
    pdu_io::PduAppender<protocols::Udp> udp_app(datadir / "udp.lance", compress);
    staged::RemainderAppender rem_app(datadir / remainder_name, "next_protocol", compress);
    const auto close_all = [&]() {
        eth_app.close();
        vlan_app.close();
        ipv4_app.close();
        ipv6_app.close();
        tcp_app.close();
        udp_app.close();
        rem_app.close();
    };

    // Process the input rows in N-row chunks (N from the budget). Each chunk appends its own fragment(s)
    // to the per-PDU + remainder tables, so writer memory stays bounded by one chunk.
    for (std::size_t start = 0; start < in_rows.size(); start += chunk_rows) {
        const std::size_t end = std::min(start + chunk_rows, in_rows.size());
        protocols::DecodedPdus pdus;
        std::vector<staged::PayloadRow> remainder;

        // Within the chunk, fetch in big contiguous tiles (S3-throughput friendly) and carve each row's
        // header prefix out of the resident tile.
        std::size_t i = start;
        while (i < end) {
            // Grow a fetch group of consecutive rows whose byte span stays within read_tile_bytes (and
            // that share the same uri — distinct input files would live at distinct objects).
            const std::uint64_t group_base = in_rows[i].off;
            const std::string& group_uri = in_rows[i].uri;
            std::size_t j = i;
            std::uint64_t span_end = group_base;
            while (j < end && in_rows[j].uri == group_uri) {
                const std::uint64_t need = in_rows[j].off + std::min<std::uint64_t>(in_rows[j].size, prefix_cap);
                if (j > i && need - group_base > read_tile_bytes) {
                    break;
                }
                span_end = std::max(span_end, need);
                ++j;
            }
            const std::size_t span_len = static_cast<std::size_t>(span_end - group_base);
            tile.resize(span_len);
            std::size_t got = 0;
            if (span_len > 0 &&
                nano_lance_fetch_external_blob(group_uri.c_str(), group_base, span_len, tile.data(), tile.size(),
                                               &got, ferr, sizeof ferr) != NANO_LANCE_READER_OK) {
                return fail(std::string("fetch_external_blob: ") + ferr);
            }
            for (std::size_t k = i; k < j; ++k) {
                const staged::PayloadRow& r = in_rows[k];
                const std::size_t local = static_cast<std::size_t>(r.off - group_base);
                const std::size_t hdr = static_cast<std::size_t>(std::min<std::uint64_t>(r.size, prefix_cap));
                if (local + hdr > got) {
                    continue;  // header prefix not available (truncated fetch) -> skip this row's layer
                }
                protocols::Bytes bytes(tile.data() + local, hdr);
                std::size_t consumed = 0;
                std::uint64_t next_disc = 0;
                bool ok = false;
                if (stage == "l2") {
                    std::uint16_t et = 0;
                    ok = protocols::decode_l2(r.packet_id, static_cast<std::uint32_t>(r.discriminator), bytes,
                                              pdus, consumed, et);
                    next_disc = et;
                } else if (stage == "l3") {
                    std::uint8_t proto = 0;
                    ok = protocols::decode_l3(r.packet_id, static_cast<std::uint16_t>(r.discriminator), bytes,
                                              pdus, consumed, proto);
                    next_disc = proto;
                } else {
                    ok = protocols::decode_l4(r.packet_id, static_cast<std::uint8_t>(r.discriminator), bytes,
                                              pdus, consumed);
                }
                // Carry forward only packets with bytes left; remainder size is arithmetic (no re-read).
                if (ok && consumed < r.size) {
                    remainder.push_back(staged::PayloadRow{r.packet_id, next_disc, r.uri, r.off + consumed,
                                                           r.size - consumed});
                }
            }
            i = j;
        }

        // Append this chunk's fragment to each output table (empty columns are no-ops).
        std::string perr;
        const bool ok = eth_app.append(pdus.ethernet, perr) && vlan_app.append(pdus.vlan, perr) &&
                        ipv4_app.append(pdus.ipv4, perr) && ipv6_app.append(pdus.ipv6, perr) &&
                        tcp_app.append(pdus.tcp, perr) && udp_app.append(pdus.udp, perr) &&
                        rem_app.append(remainder, perr);
        if (!ok) {
            close_all();
            return fail("enrich write: " + perr);
        }
        total_forward += remainder.size();
    }
    close_all();

    std::fprintf(stderr,
                 "pcapng2lance: stage %s -> %zu input rows, %zu decoded forward (chunk=%zu rows, tile=%zu B)\n",
                 stage.c_str(), in_rows.size(), total_forward, chunk_rows, read_tile_bytes);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Positional: <input> <output> [payload_uri]. Flags: --no-compress, --decode-l2l3.
    bool compress = true;
    bool decode_l2l3 = false;
    std::string stage;  // empty = one-shot; l1 writes <datadir>/packets.lance; l2/l3/l4 enrich <datadir>
    std::size_t window_bytes = std::size_t{512} * 1024 * 1024;  // L1 RAM budget per chunk; small files = 1 chunk
    std::uint64_t mem_bytes = 0;                                // enrich budget; 0 = auto-detect free RAM
    std::size_t read_tile_bytes = std::size_t{32} * 1024 * 1024;  // enrich big-read tile (S3 throughput)
    std::size_t prefix_cap = 256;                              // header bytes carved per row for the bulk
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-compress") {
            compress = false;
        } else if (a == "--decode-l2l3") {
            decode_l2l3 = true;
        } else if (a == "--stage") {
            if (i + 1 >= argc) {
                return fail("--stage requires a value (l1/l2/l3/l4)");
            }
            stage = argv[++i];
        } else if (a == "--window-bytes") {
            if (i + 1 >= argc) {
                return fail("--window-bytes requires a value");
            }
            window_bytes = static_cast<std::size_t>(std::stoull(argv[++i]));
            if (window_bytes == 0) {
                return fail("--window-bytes must be > 0");
            }
        } else if (a == "--mem-bytes") {
            if (i + 1 >= argc) {
                return fail("--mem-bytes requires a value");
            }
            mem_bytes = std::stoull(argv[++i]);
        } else if (a == "--read-tile-bytes") {
            if (i + 1 >= argc) {
                return fail("--read-tile-bytes requires a value");
            }
            read_tile_bytes = static_cast<std::size_t>(std::stoull(argv[++i]));
            if (read_tile_bytes == 0) {
                return fail("--read-tile-bytes must be > 0");
            }
        } else {
            pos.push_back(a);
        }
    }

    // Enrichment stages take just <datadir> and run entirely off the previously-written tables.
    if (stage == "l2" || stage == "l3" || stage == "l4") {
        if (pos.empty()) {
            std::fprintf(stderr,
                         "usage: %s --stage l2|l3|l4 [--mem-bytes N] [--read-tile-bytes N] <datadir>\n", argv[0]);
            return 2;
        }
        return run_enrich_stage(pos[0], stage, compress, mem_bytes, read_tile_bytes, prefix_cap);
    }

    if (pos.size() < 2) {
        std::fprintf(stderr,
                     "usage: %s [--no-compress] [--decode-l2l3] <input.pcap|pcapng> <output.lance> [payload_uri]\n"
                     "       %s --stage l1 <input.pcap|pcapng> <datadir>   (then --stage l2|l3|l4 <datadir>)\n",
                     argv[0], argv[0]);
        return 2;
    }
    const std::filesystem::path input = pos[0];
    std::filesystem::path output = pos[1];
    if (stage == "l1") {
        std::error_code ec;
        std::filesystem::create_directories(output, ec);  // <datadir>
        output = output / "packets.lance";
        decode_l2l3 = false;  // in staged mode each layer is its own run
    }
    const std::string payload_uri = (pos.size() >= 3) ? pos[2] : to_file_uri(input);

    std::string error;

    // Stream the capture in bounded windows: scan complete blocks in the window, bulk-parse them from
    // the SAME resident bytes (no re-read), write a Lance batch, commit it as a fragment, slide. The only
    // viable shape for endless / S3-backed captures, and exactly the per-window batch a CUDA ex::bulk path
    // will run. `--window-bytes` is the RAM/VRAM budget; a small file is simply one window/fragment.
    streaming::FileSource source(input);
    if (!source.ok()) {
        return fail("cannot open input file: " + input.string());
    }
    streaming::Window<streaming::FileSource> win(source, window_bytes);

    // Combined record-batch schema (scalar columns + lance.blob.v2 payload_ref), built once and reused
    // for every chunk's write_batch (the writer requires an identical schema each time).
    constexpr std::size_t kScalarCols = nanotins::column_count<PacketRow>;
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, static_cast<int64_t>(kScalarCols + 1)) != NANOARROW_OK) {
        return fail("alloc combined schema");
    }
    if (!nanotins::nt_fill_struct_schema<PacketRow>(&schema, 0, error)) {
        return fail("scalar schema: " + error);
    }
    {
        ArrowSchema blob{};
        if (!nano_lance::build_blob_v2_payload_schema(blob, error)) {
            return fail("blob schema: " + error);
        }
        ArrowSchemaRelease(schema.children[kScalarCols]);
        std::memcpy(schema.children[kScalarCols], &blob, sizeof(ArrowSchema));
        blob.release = nullptr;
    }
    schema.flags = 0;

    NanoLanceWriter writer{};
    if (nano_lance_writer_init(&writer, output.string().c_str(), 3) != NANO_LANCE_OK) {
        return fail(std::string("writer init: ") + nano_lance_writer_last_error(&writer));
    }
    nano_lance_writer_set_ignore_nullability(&writer, true);  // blob.v2 data child is null for external rows
    nano_lance_writer_set_compression(&writer, compress);

    // State carried across windows: per-section interface tables (a pcapng may concatenate sections, each
    // resetting the table; interface_id is section-relative), the current section, a global monotonic
    // packet id, and the SHB/IDB option metadata frozen onto the schema before the first commit.
    pcapblocks::ScanState st;
    std::vector<std::vector<pcapblocks::IdbView>> sections;
    std::ptrdiff_t cur_section = -1;
    std::uint64_t global_pid = 0;
    std::size_t shb_count = 0, total_idb = 0, other_count = 0;
    ArrowBuffer meta;
    ArrowMetadataBuilderInit(&meta, nullptr);
    bool schema_meta_set = false;
    bool first_commit = true;
    protocols::DecodedPdus pdus;  // accumulated only when --decode-l2l3

    const auto ensure_section = [&]() {
        if (cur_section < 0) {
            sections.emplace_back();
            cur_section = 0;
        }
    };

    // Phase-B runs as a scheduler-agnostic ex::bulk over each window's BlockRefs. On this host the
    // scheduler is a CPU thread pool; a CUDA build swaps it for nvexec and the kernel is unchanged.
    const unsigned hc = std::thread::hardware_concurrency();
    exec::static_thread_pool bulk_pool(hc == 0 ? 4 : hc);
    auto bulk_sched = bulk_pool.get_scheduler();

    win.fill();
    while (win.size() > 0) {
        std::vector<pcapblocks::BlockRef> refs;
        std::size_t consumed = 0;
        if (!pcapblocks::scan_window(st, win.bytes(), refs, consumed, win.eof(), error)) {
            return fail("scan: " + error);
        }
        if (consumed == 0) {
            if (win.eof()) {
                break;  // no complete block remains
            }
            if (win.full()) {
                win.grow();  // a single block larger than the window
            }
            win.fill();
            continue;
        }
        const pcapblocks::Bytes wbytes = win.bytes();
        const std::uint64_t wbase = win.base();

        std::vector<pcapblocks::BlockRef> packets;
        std::vector<std::size_t> packet_section;
        for (const auto& r : refs) {
            if (r.kind == pcapblocks::Kind::Shb) {
                ++shb_count;
                sections.emplace_back();
                cur_section = static_cast<std::ptrdiff_t>(sections.size()) - 1;
                pcapblocks::ShbView shb{};
                if (!schema_meta_set && pcapblocks::parse_shb(wbytes, r, shb)) {
                    pcapblocks::Options opts = shb.options;
                    pcapblocks::Option opt{};
                    const std::string prefix = "pcapng:shb" + std::to_string(shb_count - 1) + ":";
                    while (pcapblocks::next_option(opts, opt)) {
                        if (opt.code == 2) add_string_option(meta, prefix + "hardware", opt);
                        else if (opt.code == 3) add_string_option(meta, prefix + "os", opt);
                        else if (opt.code == 4) add_string_option(meta, prefix + "userappl", opt);
                    }
                }
            } else if (r.kind == pcapblocks::Kind::Idb) {
                ensure_section();
                pcapblocks::IdbView idb{};
                if (!pcapblocks::parse_idb(wbytes, r, idb)) {
                    return fail("failed to parse interface description block");
                }
                if (!schema_meta_set) {
                    pcapblocks::Options opts = idb.options;
                    pcapblocks::Option opt{};
                    const std::string prefix = "pcapng:if" + std::to_string(total_idb) + ":";
                    while (pcapblocks::next_option(opts, opt)) {
                        if (opt.code == 2) add_string_option(meta, prefix + "name", opt);
                        else if (opt.code == 3) add_string_option(meta, prefix + "description", opt);
                        else if (opt.code == 12) add_string_option(meta, prefix + "os", opt);
                    }
                }
                sections[static_cast<std::size_t>(cur_section)].push_back(idb);
                ++total_idb;
            } else if (r.kind == pcapblocks::Kind::Epb || r.kind == pcapblocks::Kind::PcapRecord) {
                ensure_section();
                packets.push_back(r);
                packet_section.push_back(static_cast<std::size_t>(cur_section));
            } else {
                ++other_count;
            }
        }

        const std::size_t n = packets.size();
        if (n == 0) {
            win.consume(consumed);
            win.fill();
            continue;  // window held only SHB/IDB/other; no packet batch to write
        }

        // Phase B as a scheduler-agnostic ex::bulk over the window's BlockRefs: one task range per
        // partition, each calling the pure per-block parse_epb and scattering into the SoA columns. The
        // kernel captures only POD pointers + the (ptr+size) window span — the device-safe shape a CUDA
        // scheduler runs unchanged.
        std::vector<std::uint32_t> iface(n), caplen(n), origlen(n), psize(n), flags(n);
        std::vector<std::uint64_t> ts(n), poff(n);
        {
            const pcapblocks::Bytes wb = wbytes;
            const pcapblocks::BlockRef* pk = packets.data();
            std::uint32_t* p_iface = iface.data();
            std::uint32_t* p_caplen = caplen.data();
            std::uint32_t* p_origlen = origlen.data();
            std::uint32_t* p_psize = psize.data();
            std::uint32_t* p_flags = flags.data();
            std::uint64_t* p_ts = ts.data();
            std::uint64_t* p_poff = poff.data();
            const std::size_t num_tasks = std::min<std::size_t>(n, 64);
            nanotins::bulk_for_each(bulk_sched, num_tasks, n, [=](std::size_t i) {
                pcapblocks::EpbView v{};
                if (pcapblocks::parse_epb(wb, pk[i], v)) {
                    p_iface[i] = v.interface_id;
                    p_ts[i] = v.ts_raw;
                    p_caplen[i] = v.caplen;
                    p_origlen[i] = v.origlen;
                    p_poff[i] = v.payload_file_offset;
                    p_psize[i] = v.caplen;
                    p_flags[i] = v.epb_flags;
                }
            });
        }

        nanotins::soa<PacketRow> rows;
        rows.resize(n);
        std::vector<std::uint16_t> pkt_link_type(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto& table = sections[packet_section[i]];
            const std::uint32_t id = iface[i];
            const std::uint16_t link_type =
                id < table.size() ? table[id].link_type : static_cast<std::uint16_t>(0);
            const std::uint8_t ts_resol =
                id < table.size() ? table[id].ts_resol : static_cast<std::uint8_t>(6);
            pkt_link_type[i] = link_type;
            rows.store(i, PacketRow{global_pid + i, iface[i], ts[i], caplen[i], origlen[i], link_type, ts_resol,
                                    flags[i]});
        }

        // Freeze the SHB/IDB KV metadata onto the schema before the first commit (it can't change once
        // the writer has mapped the schema). Captures every option seen before the first packet.
        if (!schema_meta_set) {
            if (ArrowSchemaSetMetadata(schema.children[0], reinterpret_cast<const char*>(meta.data)) !=
                NANOARROW_OK) {
                return fail("set field metadata");
            }
            schema_meta_set = true;
        }

        ArrowArray batch{};
        if (ArrowArrayInitFromSchema(&batch, &schema, nullptr) != NANOARROW_OK ||
            ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
            return fail("alloc combined array");
        }
        ArrowArray* payload = batch.children[kScalarCols];
        for (std::size_t i = 0; i < n; ++i) {
            if (!nanotins::nt_append_scalar_row<PacketRow>(&batch, 0, rows, i)) {
                return fail("append scalar columns");
            }
            ArrowStringView uri_view{payload_uri.data(), static_cast<int64_t>(payload_uri.size())};
            if (ArrowArrayAppendNull(payload->children[0], 1) != NANOARROW_OK ||
                ArrowArrayAppendString(payload->children[1], uri_view) != NANOARROW_OK ||
                ArrowArrayAppendUInt(payload->children[2], wbase + poff[i]) != NANOARROW_OK ||  // absolute offset
                ArrowArrayAppendUInt(payload->children[3], psize[i]) != NANOARROW_OK) {
                return fail("append payload_ref");
            }
            if (ArrowArrayFinishElement(payload) != NANOARROW_OK ||
                ArrowArrayFinishElement(&batch) != NANOARROW_OK) {
                return fail("finish element");
            }
        }
        if (ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
            return fail("finalize array");
        }
        if (nano_lance_write_batch(&writer, &batch, &schema) != NANO_LANCE_OK) {
            batch.release(&batch);
            return fail(std::string("write_batch: ") + nano_lance_writer_last_error(&writer));
        }
        batch.release(&batch);
        // Commit this chunk as its own fragment (first = create, rest = append) so writer memory stays
        // bounded by one window.
        if (nano_lance_writer_commit(&writer, /*is_append=*/!first_commit) != NANO_LANCE_OK) {
            return fail(std::string("commit: ") + nano_lance_writer_last_error(&writer));
        }
        first_commit = false;

        if (decode_l2l3) {
            for (std::size_t i = 0; i < n; ++i) {
                if (poff[i] + psize[i] <= wbytes.size()) {
                    protocols::decode_packet(global_pid + i, pkt_link_type[i], wbytes.subspan(poff[i], psize[i]),
                                             pdus);
                }
            }
        }

        global_pid += n;
        win.consume(consumed);
        win.fill();
    }
    ArrowBufferReset(&meta);
    nano_lance_writer_close(&writer);
    schema.release(&schema);

    // Optional one-shot L2/L3 decode -> one Lance table per PDU type (accumulated across windows; for
    // truly endless captures use the staged --stage path, which is itself bounded).
    if (decode_l2l3 && global_pid > 0) {
        const auto stem = (output.parent_path() / output.stem()).string();
        std::string perr;
        const auto write_one = [&](const char* suffix, auto& column) -> bool {
            const std::filesystem::path p = stem + suffix;
            if (!pdu_io::write_pdu_table(p, column, compress, perr)) {
                std::fprintf(stderr, "pcapng2lance: failed to write %s: %s\n", p.string().c_str(), perr.c_str());
                return false;
            }
            return true;
        };
        const bool pdus_ok = write_one("_ethernet.lance", pdus.ethernet) & write_one("_vlan.lance", pdus.vlan) &
                             write_one("_ipv4.lance", pdus.ipv4) & write_one("_ipv6.lance", pdus.ipv6) &
                             write_one("_tcp.lance", pdus.tcp) & write_one("_udp.lance", pdus.udp);
        if (!pdus_ok) {
            return 1;
        }
        std::fprintf(stderr,
                     "pcapng2lance: decoded L2/L3 -> eth %zu, vlan %zu, ipv4 %zu, ipv6 %zu, tcp %zu, udp %zu\n",
                     pdus.ethernet.size(), pdus.vlan.size(), pdus.ipv4.size(), pdus.ipv6.size(), pdus.tcp.size(),
                     pdus.udp.size());
    }

    std::fprintf(stderr,
                 "pcapng2lance: %llu packets, %zu interface(s) across %zu section(s), %zu skipped block(s) -> %s\n",
                 static_cast<unsigned long long>(global_pid), total_idb, shb_count, other_count,
                 output.string().c_str());
    return 0;
}
