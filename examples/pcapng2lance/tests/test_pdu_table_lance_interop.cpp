// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// M6 (full-pipeline golden, no Python): the DAG PDU tables, written to real Lance and read back, dump
// identically to the protocols:: tables. For the same packets we write each PDU table both ways (old
// PduColumn path and new dag_pdu_table path) to actual .lance datasets, then dump each with the
// nlance2table tool (the same reader the parity tests use) and diff the CSV. This validates the WHOLE
// round-trip — Arrow -> Lance write -> Lance read -> text — locally, so the driver's decode-path cutover
// to the spec/DAG can be verified without the Python/tshark goldens.
//
// argv[1] = path to the nlance2table executable (passed by CTest).

#include "dag_table_writer.hpp"
#include "pdu_table_writer.hpp"

#include "nanotins/dag_decode.hpp"
#include "nanotins/protocol_decode.hpp"
#include "nanotins/protocol_specs.hpp"
#include "nanotins/spec_dag.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using G = nanotins::L2L3Graph;

namespace {

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v >> 8);
    b[off + 1] = static_cast<std::uint8_t>(v & 0xFF);
}

std::vector<std::uint8_t> pkt_eth_ipv4_udp() {
    std::vector<std::uint8_t> b(42, 0);
    for (std::size_t i = 0; i < 12; ++i) b[i] = static_cast<std::uint8_t>(0x10 + i);
    put16(b, 12, 0x0800);
    b[14] = 0x45;
    b[15] = 0xB8;
    put16(b, 16, 1500);
    put16(b, 18, 0xABCD);
    b[22] = 64;
    b[23] = 17;
    for (std::size_t i = 0; i < 4; ++i) b[26 + i] = static_cast<std::uint8_t>(192 + i);
    for (std::size_t i = 0; i < 4; ++i) b[30 + i] = static_cast<std::uint8_t>(10 + i);
    put16(b, 34, 0x1234);
    put16(b, 36, 0x5678);
    put16(b, 38, 8);
    return b;
}
std::vector<std::uint8_t> pkt_eth_vlan_ipv6_tcp() {
    std::vector<std::uint8_t> b(78, 0);
    put16(b, 12, 0x8100);
    put16(b, 14, 0x6005);
    put16(b, 16, 0x86DD);
    b[18] = 0x60;
    b[24] = 6;
    b[25] = 64;
    for (std::size_t i = 0; i < 16; ++i) b[26 + i] = static_cast<std::uint8_t>(0x20 + i);
    for (std::size_t i = 0; i < 16; ++i) b[42 + i] = static_cast<std::uint8_t>(0x40 + i);
    put16(b, 58, 0xABCD);
    put16(b, 60, 0xDCBA);
    b[70] = 0x50;
    return b;
}

std::string g_tool;

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Dump a Lance dataset to CSV via nlance2table; return the CSV text (empty + fail the test on error).
std::string dump_csv(const fs::path& dataset, const fs::path& out_csv) {
    const std::string inner =
        "\"" + g_tool + "\" \"" + dataset.string() + "\" -f csv -o \"" + out_csv.string() + "\"";
#ifdef _WIN32
    const std::string cmd = "\"" + inner + "\"";
#else
    const std::string cmd = inner;
#endif
    CHECK(std::system(cmd.c_str()) == 0);
    return read_file(out_csv);
}

template <class T, class Spec>
void check_type(const char* name, const protocols::PduColumn<T>& col,
                const nanotins::dag_pdu_table<Spec>& tab, const fs::path& tmp) {
    CHECK(col.size() == tab.size());
    std::string err;
    const fs::path old_ds = tmp / (std::string(name) + "_old.lance");
    const fs::path new_ds = tmp / (std::string(name) + "_new.lance");
    CHECK(pdu_io::write_pdu_table<T>(old_ds, col, /*compress=*/false, err));
    CHECK(pdu_io::write_dag_pdu_table<Spec>(new_ds, tab, /*compress=*/false, err));

    const std::string old_csv = dump_csv(old_ds, tmp / (std::string(name) + "_old.csv"));
    const std::string new_csv = dump_csv(new_ds, tmp / (std::string(name) + "_new.csv"));
    if (old_csv != new_csv) {
        std::fprintf(stderr, "FAIL %s: CSV dumps differ\n--- old ---\n%s\n--- new ---\n%s\n", name,
                     old_csv.c_str(), new_csv.c_str());
        std::exit(1);
    }
    CHECK(!old_csv.empty());
    std::printf("  %-9s ok (%zu rows, identical nlance2table CSV)\n", name, tab.size());
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc >= 2);
    g_tool = argv[1];

    const fs::path tmp = fs::temp_directory_path() / "pdu_table_lance_interop";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    const auto p0 = pkt_eth_ipv4_udp();
    const auto p1 = pkt_eth_vlan_ipv6_tcp();

    protocols::DecodedPdus pdus;
    protocols::decode_packet(7, protocols::kLinkTypeEthernet, protocols::Bytes(p0.data(), p0.size()), pdus);
    protocols::decode_packet(8, protocols::kLinkTypeEthernet, protocols::Bytes(p1.data(), p1.size()), pdus);

    nanotins::dag_tables<G> tabs;
    nanotins::dag_decode_packet<G>(7, p0.data(), p0.size(), tabs, nanotins::kEthRoot);
    nanotins::dag_decode_packet<G>(8, p1.data(), p1.size(), tabs, nanotins::kEthRoot);

    constexpr std::size_t kEth = nanotins::node_id_v<nanotins::EthNode, G>;
    constexpr std::size_t kVlan = nanotins::node_id_v<nanotins::VlanNode, G>;
    constexpr std::size_t kIpv4 = nanotins::node_id_v<nanotins::Ipv4Node, G>;
    constexpr std::size_t kIpv6 = nanotins::node_id_v<nanotins::Ipv6Node, G>;
    constexpr std::size_t kTcp = nanotins::node_id_v<nanotins::TcpNode, G>;
    constexpr std::size_t kUdp = nanotins::node_id_v<nanotins::UdpNode, G>;

    check_type<protocols::Ethernet, nanotins::EthernetSpec>("ethernet", pdus.ethernet, std::get<kEth>(tabs), tmp);
    check_type<protocols::VlanTag, nanotins::VlanTagSpec>("vlan", pdus.vlan, std::get<kVlan>(tabs), tmp);
    check_type<protocols::Ipv4, nanotins::Ipv4Spec>("ipv4", pdus.ipv4, std::get<kIpv4>(tabs), tmp);
    check_type<protocols::Ipv6, nanotins::Ipv6Spec>("ipv6", pdus.ipv6, std::get<kIpv6>(tabs), tmp);
    check_type<protocols::Tcp, nanotins::TcpSpec>("tcp", pdus.tcp, std::get<kTcp>(tabs), tmp);
    check_type<protocols::Udp, nanotins::UdpSpec>("udp", pdus.udp, std::get<kUdp>(tabs), tmp);

    fs::remove_all(tmp, ec);
    std::printf("pdu_table_lance_interop: ok (DAG tables == protocols:: tables through Lance write/read/dump)\n");
    return 0;
}
