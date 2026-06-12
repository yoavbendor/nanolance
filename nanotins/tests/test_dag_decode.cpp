// M5b: the DAG-driven columnar decode. Walking the graph must scatter each PDU's fields into its node's
// SoA columns with byte-identical values to struct_view, link each row to the right packet_id, accumulate
// across packets, and produce per-node row counts equal to the existing count_packet (== walk_packet). So
// the spec-driven tables carry exactly the PDUs the hand-written decoder would, column for column.

#include "nanotins/dag_decode.hpp"
#include "nanotins/protocol_decode.hpp"
#include "nanotins/protocols.hpp"
#include "nanotins/spec_dag.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <vector>

using G = nanotins::L2L3Graph;
using namespace nanotins::literals;

namespace {

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

constexpr std::size_t kEth = nanotins::node_id_v<nanotins::EthNode, G>;
constexpr std::size_t kVlan = nanotins::node_id_v<nanotins::VlanNode, G>;
constexpr std::size_t kIpv4 = nanotins::node_id_v<nanotins::Ipv4Node, G>;
constexpr std::size_t kIpv6 = nanotins::node_id_v<nanotins::Ipv6Node, G>;
constexpr std::size_t kTcp = nanotins::node_id_v<nanotins::TcpNode, G>;
constexpr std::size_t kUdp = nanotins::node_id_v<nanotins::UdpNode, G>;

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v >> 8);
    b[off + 1] = static_cast<std::uint8_t>(v & 0xFF);
}

std::vector<std::uint8_t> eth_ipv4_udp() {
    std::vector<std::uint8_t> b(42, 0);
    put16(b, 12, 0x0800);  // ethertype IPv4
    b[14] = 0x45;          // version 4, ihl 5
    b[14 + 9] = 17;        // protocol UDP
    put16(b, 34, 0x1234);  // UDP src_port
    put16(b, 36, 0x5678);  // UDP dst_port
    return b;
}

std::vector<std::uint8_t> eth_vlan_ipv6_tcp() {
    std::vector<std::uint8_t> b(78, 0);
    put16(b, 12, 0x8100);  // VLAN
    put16(b, 16, 0x86DD);  // inner IPv6
    b[18] = 0x60;          // IPv6 version
    b[18 + 6] = 6;         // next_header TCP
    b[58 + 12] = 0x50;     // TCP data_offset 5
    put16(b, 58, 0xABCD);  // TCP src_port
    return b;
}

}  // namespace

int main() {
    nanotins::dag_tables<G> tabs;

    // ---- packet 7: eth / ipv4 / udp ----
    {
        const auto b = eth_ipv4_udp();
        nanotins::dag_decode_packet<G>(7, b.data(), b.size(), tabs, nanotins::kEthRoot);

        CHECK(std::get<kEth>(tabs).size() == 1);
        CHECK(std::get<kIpv4>(tabs).size() == 1);
        CHECK(std::get<kUdp>(tabs).size() == 1);
        CHECK(std::get<kVlan>(tabs).size() == 0);
        CHECK(std::get<kIpv6>(tabs).size() == 0);
        CHECK(std::get<kTcp>(tabs).size() == 0);

        // packet_id links every row back to packet 7
        CHECK(std::get<kEth>(tabs).packet_id[0] == 7);
        CHECK(std::get<kIpv4>(tabs).packet_id[0] == 7);
        CHECK(std::get<kUdp>(tabs).packet_id[0] == 7);

        // column values == struct_view reads at the known offsets
        nanotins::struct_view<nanotins::EthernetSpec> ev(b.data() + 0);
        nanotins::struct_view<nanotins::Ipv4Spec> i4v(b.data() + 14);
        nanotins::struct_view<nanotins::UdpSpec> uv(b.data() + 34);
        CHECK(std::get<kEth>(tabs).column<2>()[0] == ev("ethertype"_fld));      // ethertype col
        CHECK(std::get<kIpv4>(tabs).column<9>()[0] == i4v("protocol"_fld));     // protocol col
        CHECK(std::get<kUdp>(tabs).column<0>()[0] == uv("src_port"_fld));       // src_port col
        CHECK(std::get<kUdp>(tabs).column<1>()[0] == uv("dst_port"_fld));       // dst_port col
        CHECK(std::get<kUdp>(tabs).column<0>()[0] == 0x1234);
        CHECK(std::get<kUdp>(tabs).column<1>()[0] == 0x5678);

        // per-node row counts == the existing count_packet (walk_packet)
        const auto c = protocols::count_packet(protocols::kLinkTypeEthernet,
                                               protocols::Bytes(b.data(), b.size()));
        CHECK(std::get<kEth>(tabs).size() == c.eth);
        CHECK(std::get<kIpv4>(tabs).size() == c.ipv4);
        CHECK(std::get<kUdp>(tabs).size() == c.udp);
    }

    // ---- packet 8: eth / vlan / ipv6 / tcp (accumulates onto the same tables) ----
    {
        const auto b = eth_vlan_ipv6_tcp();
        nanotins::dag_decode_packet<G>(8, b.data(), b.size(), tabs, nanotins::kEthRoot);

        CHECK(std::get<kEth>(tabs).size() == 2);   // both packets have an Ethernet header
        CHECK(std::get<kVlan>(tabs).size() == 1);
        CHECK(std::get<kIpv6>(tabs).size() == 1);
        CHECK(std::get<kTcp>(tabs).size() == 1);
        CHECK(std::get<kIpv4>(tabs).size() == 1);  // unchanged from packet 7
        CHECK(std::get<kUdp>(tabs).size() == 1);

        // second Ethernet row belongs to packet 8; the VLAN/IPv6/TCP rows too
        CHECK(std::get<kEth>(tabs).packet_id[0] == 7);
        CHECK(std::get<kEth>(tabs).packet_id[1] == 8);
        CHECK(std::get<kVlan>(tabs).packet_id[0] == 8);
        CHECK(std::get<kTcp>(tabs).packet_id[0] == 8);

        nanotins::struct_view<nanotins::TcpSpec> tv(b.data() + 58);
        CHECK(std::get<kTcp>(tabs).column<0>()[0] == tv("src_port"_fld));  // TCP src_port col
        CHECK(std::get<kTcp>(tabs).column<0>()[0] == 0xABCD);
    }

    std::printf("dag_decode: ok (DAG scatter == struct_view; packet_id + accumulation + counts == "
                "walk_packet)\n");
    return 0;
}
