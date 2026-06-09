// pcapng -> Lance converter (step 1). Pipeline:
//   read file -> scan_blocks (seam) -> interface table -> parse_epbs_bulk (seam) -> fill nanotins
//   SoA scalar columns + a lance.blob.v2 payload_ref (external uri+off+size) -> nano_lance write.
// Packet payloads are never copied: each row stores where its bytes live in the source file.

#include "nanotins/arrow_glue.hpp"
#include "pcap_blocks.hpp"
#include "pdu_table_writer.hpp"
#include "protocol_decode.hpp"

#include "nanolance/blob_builder.hpp"
#include "nanolance/nano_lance_writer.h"

#include <boost/describe.hpp>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// The all-scalar packet row that flows through the nanotins reflection core. payload_uri/off/size are
// NOT here — they ride in the lance.blob.v2 `payload_ref` struct appended alongside.
struct PacketRow {
    std::uint32_t interface_id;
    std::uint64_t ts_raw;
    std::uint32_t caplen;
    std::uint32_t origlen;
    std::uint16_t link_type;  // denormalized from the interface (ConstantLayout when single-iface)
    std::uint8_t ts_resol;    // denormalized; lets a row self-describe its time unit
    std::uint32_t epb_flags;
};
BOOST_DESCRIBE_STRUCT(PacketRow, (), (interface_id, ts_raw, caplen, origlen, link_type, ts_resol, epb_flags))

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "cannot open input file: " + path.string();
        return false;
    }
    const auto size = in.tellg();
    if (size < 0) {
        error = "cannot size input file";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size)) {
        error = "failed to read input file";
        return false;
    }
    return true;
}

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

}  // namespace

int main(int argc, char** argv) {
    // Positional: <input> <output> [payload_uri]. Flags: --no-compress, --decode-l2l3.
    bool compress = true;
    bool decode_l2l3 = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-compress") {
            compress = false;
        } else if (a == "--decode-l2l3") {
            decode_l2l3 = true;
        } else {
            pos.push_back(a);
        }
    }
    if (pos.size() < 2) {
        std::fprintf(stderr,
                     "usage: %s [--no-compress] [--decode-l2l3] <input.pcap|pcapng> <output.lance> [payload_uri]\n",
                     argv[0]);
        return 2;
    }
    const std::filesystem::path input = pos[0];
    const std::filesystem::path output = pos[1];
    const std::string payload_uri = (pos.size() >= 3) ? pos[2] : to_file_uri(input);

    std::string error;
    std::vector<std::uint8_t> file_bytes;
    if (!read_file(input, file_bytes, error)) {
        return fail(error);
    }
    pcapblocks::Bytes file(file_bytes.data(), file_bytes.size());

    // Phase A: scan.
    std::vector<pcapblocks::BlockRef> refs;
    if (!pcapblocks::scan_blocks(file, refs, error)) {
        return fail("scan: " + error);
    }

    // Walk blocks in order. A pcapng file may concatenate several sections (one SHB each); the
    // interface table RESETS per SHB and `interface_id` is section-relative (DESIGN section 6). So we
    // keep a per-section interface table and remember which section each packet belongs to, rather than
    // accumulating IDBs globally.
    std::vector<std::vector<pcapblocks::IdbView>> sections;  // one interface table per section
    std::vector<pcapblocks::BlockRef> packets;
    std::vector<std::size_t> packet_section;  // section index for each packet
    std::size_t shb_count = 0, other_count = 0, total_idb = 0;
    std::ptrdiff_t cur_section = -1;
    ArrowBuffer meta;
    ArrowMetadataBuilderInit(&meta, nullptr);

    const auto ensure_section = [&]() {
        if (cur_section < 0) {
            sections.emplace_back();
            cur_section = 0;
        }
    };

    for (const auto& r : refs) {
        if (r.kind == pcapblocks::Kind::Shb) {
            ++shb_count;
            sections.emplace_back();  // reset interface table for the new section
            cur_section = static_cast<std::ptrdiff_t>(sections.size()) - 1;
            pcapblocks::ShbView shb{};
            if (pcapblocks::parse_shb(file, r, shb)) {
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
            if (!pcapblocks::parse_idb(file, r, idb)) {
                return fail("failed to parse interface description block");
            }
            pcapblocks::Options opts = idb.options;
            pcapblocks::Option opt{};
            const std::string prefix = "pcapng:if" + std::to_string(total_idb) + ":";
            while (pcapblocks::next_option(opts, opt)) {
                if (opt.code == 2) add_string_option(meta, prefix + "name", opt);
                else if (opt.code == 3) add_string_option(meta, prefix + "description", opt);
                else if (opt.code == 12) add_string_option(meta, prefix + "os", opt);
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

    // Phase B (bulk): parse packet blocks into raw column arrays.
    std::vector<std::uint32_t> iface(n), caplen(n), origlen(n), psize(n), flags(n);
    std::vector<std::uint64_t> ts(n), poff(n);
    pcapblocks::EpbColumns cols{iface.data(), ts.data(),   caplen.data(), origlen.data(),
                                poff.data(),  psize.data(), flags.data(),  n};
    if (n > 0 && !pcapblocks::parse_epbs_bulk(file, packets.data(), n, cols, error)) {
        return fail("bulk parse: " + error);
    }

    // Fill the nanotins SoA (scalar columns, with link_type/ts_resol denormalized per row).
    nanotins::soa<PacketRow> rows;
    rows.resize(n);
    std::vector<std::uint16_t> pkt_link_type(n);  // kept for the optional L2/L3 decode pass
    for (std::size_t i = 0; i < n; ++i) {
        const auto& table = sections[packet_section[i]];  // this packet's section interface table
        const std::uint32_t id = iface[i];
        const std::uint16_t link_type =
            id < table.size() ? table[id].link_type : static_cast<std::uint16_t>(0);
        const std::uint8_t ts_resol = id < table.size() ? table[id].ts_resol : static_cast<std::uint8_t>(6);
        pkt_link_type[i] = link_type;
        rows.store(i, PacketRow{iface[i], ts[i], caplen[i], origlen[i], link_type, ts_resol, flags[i]});
    }

    // Build the combined record-batch schema: scalar columns + lance.blob.v2 payload_ref.
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
        ArrowSchemaRelease(schema.children[kScalarCols]);  // free placeholder
        std::memcpy(schema.children[kScalarCols], &blob, sizeof(ArrowSchema));
        blob.release = nullptr;  // ownership transferred into the struct's child slot
    }
    // Dataset KV metadata (SHB/IDB options) ride as field metadata on the first scalar column.
    // (Root-level metadata can't be used: the mapper treats any root metadata as an extension marker,
    // breaking record-batch detection.) Field metadata round-trips to the manifest and the reader.
    if (ArrowSchemaSetMetadata(schema.children[0], reinterpret_cast<const char*>(meta.data)) != NANOARROW_OK) {
        return fail("set field metadata");
    }
    ArrowBufferReset(&meta);
    schema.flags = 0;

    // Build the combined array and fill it row by row.
    ArrowArray batch{};
    if (ArrowArrayInitFromSchema(&batch, &schema, nullptr) != NANOARROW_OK) {
        return fail("alloc combined array");
    }
    if (ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
        return fail("start appending");
    }
    ArrowArray* payload = batch.children[kScalarCols];
    ArrowArray* p_data = payload->children[0];
    ArrowArray* p_uri = payload->children[1];
    ArrowArray* p_pos = payload->children[2];
    ArrowArray* p_size = payload->children[3];
    for (std::size_t i = 0; i < n; ++i) {
        if (!nanotins::nt_append_scalar_row<PacketRow>(&batch, 0, rows, i)) {
            return fail("append scalar columns");
        }
        ArrowStringView uri_view{payload_uri.data(), static_cast<int64_t>(payload_uri.size())};
        if (ArrowArrayAppendNull(p_data, 1) != NANOARROW_OK ||
            ArrowArrayAppendString(p_uri, uri_view) != NANOARROW_OK ||
            ArrowArrayAppendUInt(p_pos, poff[i]) != NANOARROW_OK ||
            ArrowArrayAppendUInt(p_size, psize[i]) != NANOARROW_OK) {
            return fail("append payload_ref");
        }
        if (ArrowArrayFinishElement(payload) != NANOARROW_OK || ArrowArrayFinishElement(&batch) != NANOARROW_OK) {
            return fail("finish element");
        }
    }
    if (ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
        return fail("finalize array");
    }

    // Write the Lance dataset (compression on: lights up bitpacking / RLE / ConstantLayout).
    NanoLanceWriter writer{};
    if (nano_lance_writer_init(&writer, output.string().c_str(), 3) != NANO_LANCE_OK) {
        return fail(std::string("writer init: ") + nano_lance_writer_last_error(&writer));
    }
    // The blob.v2 payload_ref's `data` child is null for external rows (uri-only), so nullability must
    // be ignored — the same mode every other lance.blob.v2 writer path uses.
    nano_lance_writer_set_ignore_nullability(&writer, true);
    nano_lance_writer_set_compression(&writer, compress);
    if (n > 0 && nano_lance_write_batch(&writer, &batch, &schema) != NANO_LANCE_OK) {
        return fail(std::string("write_batch: ") + nano_lance_writer_last_error(&writer));
    }
    if (n > 0 && nano_lance_writer_commit(&writer, false) != NANO_LANCE_OK) {
        return fail(std::string("commit: ") + nano_lance_writer_last_error(&writer));
    }
    nano_lance_writer_close(&writer);

    batch.release(&batch);
    schema.release(&schema);

    // Optional L2/L3 decode: walk each packet's bytes and write one Lance table per PDU type, keyed by
    // packet row id. Tables land next to the packets dataset: <stem>_<pdu>.lance.
    if (decode_l2l3 && n > 0) {
        protocols::DecodedPdus pdus;
        for (std::size_t i = 0; i < n; ++i) {
            if (poff[i] + psize[i] <= file_bytes.size()) {
                protocols::Bytes pkt(file_bytes.data() + poff[i], psize[i]);
                protocols::decode_packet(i, pkt_link_type[i], pkt, pdus);
            }
        }
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
                 "pcapng2lance: %zu packets, %zu interface(s) across %zu section(s), %zu skipped block(s) -> %s\n",
                 n, total_idb, shb_count, other_count, output.string().c_str());
    return 0;
}
