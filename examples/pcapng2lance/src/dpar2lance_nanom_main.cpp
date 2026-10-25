// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
// dpar2lance (and lldp2lance, the same source built with NANOM_LLDP_ONLY) — apply DPAR (Dynamic Parser Application Rules) to a capture and write each matched rule table to its
// own Lance dataset, on the nanom parser (include/nanom_dpar.hpp):
//
//   dpar2lance [--compress] <rules.txt> <input.pcap|pcapng> <outdir>
//   lldp2lance [--compress] <rules.txt> <input.pcap|pcapng> <outdir>   (rules with the lldp parser only)
//
// Output: one dataset per non-empty parser table at <outdir>/<kind>.lance (someip_tlv, raw_tlv, oddtlv, lldp), each
// row carrying the rule_id that produced it. The someip_tlv and raw_tlv tables have the columns of the nanotins
// dpar2lance tables; the rules grammar is nanom's, which also accepts the oddtlv and lldp parsers.
#include "nanom_dpar.hpp"
#include "nanom_soa_lance_writer.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

#ifdef NANOM_LLDP_ONLY
constexpr const char* kTool = "lldp2lance";
#else
constexpr const char* kTool = "dpar2lance";
#endif

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::uint8_t chunk[65536];
    std::size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) out.insert(out.end(), chunk, chunk + n);
    std::fclose(f);
    return true;
}

// One table: write its rows as a Lance dataset named after the parser (a table with no rows writes nothing).
template <class Row>
bool write_table(const std::filesystem::path& outdir, const char* kind, const std::vector<Row>& rows, bool compress) {
    if (rows.empty()) return true;
    nanom::soa<Row> table(rows.size());
    for (const Row& r : rows) table.push(r);
    std::string err;
    const std::string path = (outdir / (std::string(kind) + ".lance")).string();
    if (!p2l_nanom::write_soa_table(path, table, compress, err)) {
        std::fprintf(stderr, "%s: failed writing %s: %s\n", kTool, path.c_str(), err.c_str());
        return false;
    }
    std::fprintf(stderr, "%s: wrote %zu rows -> %s\n", kTool, rows.size(), path.c_str());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    namespace nm = nanom;
    bool compress = false;
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--compress") {
            compress = true;
        } else {
            pos.push_back(argv[i]);
        }
    }
    if (pos.size() < 3) {
        std::fprintf(stderr, "usage: %s [--compress] <rules.txt> <input.pcap|pcapng> <outdir>\n", argv[0]);
        std::fprintf(stderr,
                     "  rules: <node>.<field> <op> <value> [&& ...] => <parser> <region> \"<label>\"\n"
                     "  parsers: someip_tlv | raw_tlv | oddtlv | lldp   regions: eth_payload | vlan_payload |\n"
                     "           udp_payload | tcp_payload | someip_payload\n"
                     "  writes one Lance dataset per non-empty parser table to <outdir>/<kind>.lance\n");
        return 2;
    }
    std::vector<std::uint8_t> rules_bytes, cap;
    if (!read_file(pos[0], rules_bytes)) {
        std::fprintf(stderr, "%s: cannot open rules file %s\n", kTool, pos[0]);
        return 1;
    }
    if (!read_file(pos[1], cap)) {
        std::fprintf(stderr, "%s: cannot open capture %s\n", kTool, pos[1]);
        return 1;
    }
    const std::filesystem::path outdir = pos[2];

    std::vector<Rule> rules;
    std::string err;
    if (!load_rules(std::string(rules_bytes.begin(), rules_bytes.end()), rules, err)) {
        std::fprintf(stderr, "%s: %s\n", kTool, err.c_str());
        return 1;
    }
#ifdef NANOM_LLDP_ONLY
    for (const Rule& r : rules) {  // lldp2lance: the LLDP palette only, as the nanotins tool
        if (r.parser != "lldp") {
            std::fprintf(stderr, "%s: rule %u: unknown parser '%s' (available: lldp)\n", kTool, r.rule_id,
                         r.parser.c_str());
            return 1;
        }
    }
#endif
    const nm::bytes file(reinterpret_cast<const std::byte*>(cap.data()), cap.size());
    std::vector<nmpcap::BlockRef> refs;
    if (!nmpcap::scan_blocks(file, refs, err)) {
        std::fprintf(stderr, "%s: %s\n", kTool, err.c_str());
        return 1;
    }

    Tables tb;
    std::vector<u16> iface_link;
    u64 pid = 0, matched_any = 0, rows_total = 0;
    for (const auto& ref : refs) {
        if (ref.kind == nmpcap::Kind::Shb) {
            iface_link.clear();
            continue;
        }
        if (ref.kind == nmpcap::Kind::Idb) {
            nmpcap::IdbView idb{};
            if (nmpcap::parse_idb(file, ref, idb)) iface_link.push_back(idb.link_type);
            continue;
        }
        if (ref.kind != nmpcap::Kind::Epb && ref.kind != nmpcap::Kind::PcapRecord) continue;
        nmpcap::EpbView e{};
        if (!nmpcap::parse_epb(file, ref, e)) continue;
        const u16 link = e.interface_id < iface_link.size() ? iface_link[e.interface_id] : u16{0};
        const nm::bytes pkt = file.subspan(std::size_t(e.payload_file_offset), e.caplen);

        PacketState st;
        const auto wr = nmproto::walk_packet(
            link, pkt, [&](const nmproto::Ethernet& x) { st.eth = x; },
            [&](const nmproto::VlanTag& x) {
                if (!st.vlan) {
                    st.vlan = x;
                    st.vlan_end = nm::wire_size_v<nmproto::Ethernet> + nm::wire_size_v<nmproto::VlanTag>;
                }
            },
            [&](const nmproto::Ipv4& x) { st.ipv4 = x; }, [&](const nmproto::Ipv6& x) { st.ipv6 = x; },
            [&](const nmproto::Tcp& x) { st.tcp = x; }, [&](const nmproto::Udp& x) { st.udp = x; });
        st.l4_payload = wr.l4_payload_offset;
        st.reached_l4 = wr.reached_l4;

        bool any = false;
        for (const auto& rule : rules) {
            bool ok = true;
            for (const auto& p : rule.preds) {
                const auto v = node_field(st, p);
                if (!v || !eval(p.op, *v, p.value)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            std::size_t b = 0, en = 0;
            if (!region_of(rule.region, st, pkt, b, en)) continue;
            any = true;
            rows_total += run_kind(rule.parser, pid, rule.rule_id, pkt.subspan(b, en - b), tb);
        }
        matched_any += any;
        ++pid;
    }

    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);
    bool ok = write_table(outdir, "someip_tlv", tb.someip_tlv, compress) &&
              write_table(outdir, "raw_tlv", tb.raw_tlv, compress) &&
              write_table(outdir, "oddtlv", tb.oddtlv, compress) && write_table(outdir, "lldp", tb.lldp, compress);
    std::fprintf(stderr, "%s: packets=%llu matched_any=%llu rows=%llu\n", kTool,
                 static_cast<unsigned long long>(pid), static_cast<unsigned long long>(matched_any),
                 static_cast<unsigned long long>(rows_total));
    return ok ? 0 : 1;
}
