// M5a: the spec-driven DAG must reproduce walk_packet's traversal exactly — same PDU sequence, same byte
// offsets — over well-formed packets, with the same L4 gating (IPv4 fragment => no L4). This is the
// determinism/parity oracle: if the graph and the hand-written switch ever disagree, the Lance tables
// would differ. gPTP (which walk_packet doesn't handle) is checked against the DAG directly.

#include "nanotins/protocol_decode.hpp"
#include "nanotins/protocols.hpp"
#include "nanotins/spec_dag.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace nanotins::literals;
using G = nanotins::L2L3Graph;

namespace {

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

constexpr int kEth = nanotins::node_id_v<nanotins::EthNode, G>;
constexpr int kVlan = nanotins::node_id_v<nanotins::VlanNode, G>;
constexpr int kIpv4 = nanotins::node_id_v<nanotins::Ipv4Node, G>;
constexpr int kIpv6 = nanotins::node_id_v<nanotins::Ipv6Node, G>;
constexpr int kTcp = nanotins::node_id_v<nanotins::TcpNode, G>;
constexpr int kUdp = nanotins::node_id_v<nanotins::UdpNode, G>;
constexpr int kGptp = nanotins::node_id_v<nanotins::GptpNode, G>;

using Trace = std::vector<std::pair<int, std::size_t>>;  // (node id, offset)

Trace dag_trace(const std::vector<std::uint8_t>& b) {
    Trace t;
    nanotins::walk<G>(nanotins::kEthRoot, b.data(), b.size(),
                      [&](auto Ic, std::size_t off) { t.emplace_back(static_cast<int>(Ic), off); });
    return t;
}

// The node-id sequence walk_packet visits (ignoring offsets), via its per-PDU callbacks.
std::vector<int> walk_packet_ids(const std::vector<std::uint8_t>& b) {
    std::vector<int> ids;
    protocols::walk_packet(
        protocols::kLinkTypeEthernet, protocols::Bytes(b.data(), b.size()),
        [&](const protocols::Ethernet&) { ids.push_back(kEth); },
        [&](const protocols::VlanTag&) { ids.push_back(kVlan); },
        [&](const protocols::Ipv4&) { ids.push_back(kIpv4); },
        [&](const protocols::Ipv6&) { ids.push_back(kIpv6); },
        [&](const protocols::Tcp&) { ids.push_back(kTcp); },
        [&](const protocols::Udp&) { ids.push_back(kUdp); });
    return ids;
}

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v >> 8);
    b[off + 1] = static_cast<std::uint8_t>(v & 0xFF);
}

// Compare the DAG trace's id sequence to walk_packet, and assert the DAG offsets match `expected`.
void check_parity(const char* name, const std::vector<std::uint8_t>& b, const Trace& expected) {
    const Trace got = dag_trace(b);
    CHECK(got.size() == expected.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].first == expected[i].first);
        CHECK(got[i].second == expected[i].second);
    }
    const std::vector<int> wp = walk_packet_ids(b);
    CHECK(wp.size() == got.size());
    for (std::size_t i = 0; i < wp.size(); ++i) {
        CHECK(wp[i] == got[i].first);
    }
    std::printf("  %-22s ok (%zu PDUs, DAG == walk_packet)\n", name, got.size());
}

}  // namespace

int main() {
    // A: Ethernet / IPv4 / UDP (42 bytes)
    {
        std::vector<std::uint8_t> b(42, 0);
        put16(b, 12, 0x0800);  // ethertype IPv4
        b[14] = 0x45;          // version 4, ihl 5
        b[14 + 9] = 17;        // protocol UDP
        // flags_frag (b[20..21]) = 0 -> first fragment, L4 present
        check_parity("eth/ipv4/udp", b, {{kEth, 0}, {kIpv4, 14}, {kUdp, 34}});
    }

    // B: Ethernet / VLAN / IPv6 / TCP (78 bytes)
    {
        std::vector<std::uint8_t> b(78, 0);
        put16(b, 12, 0x8100);  // ethertype VLAN
        put16(b, 16, 0x86DD);  // VLAN inner_ethertype IPv6
        b[18] = 0x60;          // IPv6 version nibble
        b[18 + 6] = 6;         // next_header TCP
        b[58 + 12] = 0x50;     // TCP data_offset 5 (20-byte header)
        check_parity("eth/vlan/ipv6/tcp", b, {{kEth, 0}, {kVlan, 14}, {kIpv6, 18}, {kTcp, 58}});
    }

    // C: Ethernet / IPv4 fragment (frag_offset != 0) — no L4 (34 bytes)
    {
        std::vector<std::uint8_t> b(60, 0);
        put16(b, 12, 0x0800);
        b[14] = 0x45;
        b[14 + 9] = 17;        // protocol UDP, but...
        put16(b, 14 + 6, 0x0025);  // flags_frag: non-zero fragment offset => fragment data, no L4
        check_parity("eth/ipv4 fragment", b, {{kEth, 0}, {kIpv4, 14}});
    }

    // D: Ethernet / gPTP (walk_packet has no gPTP branch — check the DAG directly) (48 bytes)
    {
        std::vector<std::uint8_t> b(48, 0);
        put16(b, 12, 0x88F7);  // ethertype PTP
        b[14] = 0x0B;          // message_type Announce (low nibble) — leaf for now
        const Trace got = dag_trace(b);
        CHECK(got.size() == 2);
        CHECK(got[0].first == kEth && got[0].second == 0);
        CHECK(got[1].first == kGptp && got[1].second == 14);
        std::printf("  %-22s ok (%zu PDUs, DAG emits eth->gptp)\n", "eth/gptp", got.size());
    }

    // E: QinQ — two stacked VLAN tags then IPv4/UDP, exercising the VlanNode self-loop (50 bytes)
    {
        std::vector<std::uint8_t> b(50, 0);
        put16(b, 12, 0x88A8);  // outer S-VLAN
        put16(b, 16, 0x8100);  // inner C-VLAN
        put16(b, 20, 0x0800);  // inner_ethertype IPv4
        b[22] = 0x45;
        b[22 + 9] = 17;        // UDP
        check_parity("eth/qinq/ipv4/udp", b,
                     {{kEth, 0}, {kVlan, 14}, {kVlan, 18}, {kIpv4, 22}, {kUdp, 42}});
    }

    std::printf("spec_dag: ok (DAG traversal == walk_packet over L2/L3/L4 + QinQ; gPTP via DAG)\n");
    return 0;
}
