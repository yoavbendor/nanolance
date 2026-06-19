// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// lldp2lance — apply the LLDP example parser to a capture via DPAR rules and write the resulting per-TLV
// table to a Lance dataset. The columnar counterpart to nanotins' NDJSON `lldp` example (examples/lldp):
// a CLI rule says "when a frame's header fields match, run the LLDP parser over this region and tabulate
// every TLV", and here that table lands in Lance instead of stdout.
//
//   lldp2lance [--compress] <rules.txt> <input.pcap|pcapng> <outdir>
//
// Example rules (see the vendored examples/lldp/rules.example.txt):
//   eth.ethertype == 0x88CC        => lldp eth_payload  "lldp"   # untagged
//   vlan.inner_ethertype == 0x88CC => lldp vlan_payload "lldp"   # 802.1Q-tagged (trunk port)
//
// Output: one dataset at <outdir>/lldp.lance, one row per LLDP TLV. Each row carries the rule_id
// discriminator (which rule produced it) and the LLDP example's decoded, type-specific columns
// (ttl_seconds, caps_*, mgmt_*) plus a 32-byte value_head fixed-binary snapshot of the TLV value.
//
// This is "free" reuse: lldp2lance is the same shape as dpar2lance, only the palette (the user-defined
// LldpPalette from the vendored example, NOT the built-in DefaultPalette) and the table name differ. The
// DPAR table -> Arrow -> Lance sink (dpar_table_writer.hpp) is entirely generic over the Row type, so the
// LLDP rows — including their fixed-size value_head array — go through unchanged.

#include "dpar_table_writer.hpp"

#include "lldp.hpp"  // the user-written LLDP parser + LldpPalette (vendored example, outside the include tree)

#include "nanotins/dpar_palette.hpp"
#include "nanotins/pcap_blocks.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    std::uint8_t chunk[65536];
    std::size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        out.insert(out.end(), chunk, chunk + n);
    }
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    bool compress = false;
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--compress") {
            compress = true;
        } else {
            pos.push_back(argv[i]);
        }
    }
    if (pos.size() < 3) {
        std::fprintf(stderr, "usage: %s [--compress] <rules.txt> <input.pcap|pcapng> <outdir>\n", argv[0]);
        std::fprintf(stderr,
                     "  rules: <node>.<field> <op> <value> [&& ...] => lldp <region> \"<label>\"\n"
                     "  example: eth.ethertype == 0x88CC => lldp eth_payload \"lldp\"\n"
                     "           vlan.inner_ethertype == 0x88CC => lldp vlan_payload \"lldp\"\n"
                     "  writes one Lance dataset of LLDP TLV rows to <outdir>/lldp.lance\n");
        return 2;
    }
    const char* rules_path = pos[0];
    const char* cap_path = pos[1];
    const std::filesystem::path outdir = pos[2];

    std::vector<std::uint8_t> rules_bytes;
    if (!read_file(rules_path, rules_bytes)) {
        std::fprintf(stderr, "lldp2lance: cannot open rules file %s\n", rules_path);
        return 1;
    }
    std::vector<std::uint8_t> cap;
    if (!read_file(cap_path, cap)) {
        std::fprintf(stderr, "lldp2lance: cannot open capture %s\n", cap_path);
        return 1;
    }

    // The only difference from dpar2lance: the engine carries the example's LldpPalette (LldpKind), not
    // the built-in DefaultPalette. Everything downstream (scan, run, for_each_table, write) is identical.
    nanotins::dpar::dpar_engine<nanotins::L2L3Graph, lldp_example::LldpPalette> engine;
    nanotins::dpar::CompileResult cr =
        engine.load_rules(std::string(rules_bytes.begin(), rules_bytes.end()));
    if (!cr.ok) {
        std::fprintf(stderr, "lldp2lance: %zu rule error(s):\n", cr.errors.size());
        for (const std::string& e : cr.errors) {
            std::fprintf(stderr, "  %s\n", e.c_str());
        }
        return 1;
    }

    std::string err;
    std::vector<pcapblocks::BlockRef> refs;
    if (!pcapblocks::scan_blocks(pcapblocks::Bytes(cap.data(), cap.size()), refs, err)) {
        std::fprintf(stderr, "lldp2lance: %s\n", err.c_str());
        return 1;
    }
    std::uint64_t packet_id = 0;
    for (const pcapblocks::BlockRef& ref : refs) {
        if (ref.kind != pcapblocks::Kind::Epb && ref.kind != pcapblocks::Kind::PcapRecord) {
            continue;
        }
        pcapblocks::EpbView e{};
        if (!pcapblocks::parse_epb(pcapblocks::Bytes(cap.data(), cap.size()), ref, e)) {
            continue;
        }
        engine.run(cap.data() + e.payload_file_offset, e.caplen, packet_id);
        ++packet_id;
    }

    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);

    // One Lance dataset per non-empty table, named by the parser Kind (here just "lldp").
    int rc = 0;
    engine.palette.for_each_table([&](const char* kind, auto& rows) {
        using Row = typename std::decay_t<decltype(rows)>::value_type;
        if (rows.empty()) {
            return;
        }
        const std::filesystem::path ds = outdir / (std::string(kind) + ".lance");
        std::string werr;
        if (!dpar_io::write_dpar_table<Row>(ds, rows, compress, werr)) {
            std::fprintf(stderr, "lldp2lance: failed writing %s: %s\n", ds.string().c_str(), werr.c_str());
            rc = 1;
            return;
        }
        std::fprintf(stderr, "lldp2lance: wrote %zu rows -> %s\n", rows.size(), ds.string().c_str());
    });

    const nanotins::dpar::EngineStats& s = engine.stats;
    std::fprintf(stderr, "lldp2lance: packets=%llu matched_any=%llu rows=%llu\n",
                 static_cast<unsigned long long>(s.packets_seen),
                 static_cast<unsigned long long>(s.packets_matched_any),
                 static_cast<unsigned long long>(s.rows_emitted));
    return rc;
}
