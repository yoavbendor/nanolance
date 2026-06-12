// M5c: the DAG's bulk (count -> prefix-sum -> scatter) path must produce byte-identical per-node tables to
// the sequential append decode — and identically whether the scatter runs serially or on a thread pool.
// This is the CPU proof of the determinism the GPU path needs: every write index is a pure function of the
// packet's prefix-summed base + its emit order, so no executor can perturb the result.

#include "nanotins/dag_bulk.hpp"
#include "nanotins/dag_decode.hpp"
#include "nanotins/spec_dag.hpp"

#include <exec/static_thread_pool.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <utility>
#include <vector>

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

std::vector<std::uint8_t> eth_ipv4_udp(std::uint16_t sp) {
    std::vector<std::uint8_t> b(42, 0);
    put16(b, 12, 0x0800);
    b[14] = 0x45;
    b[14 + 9] = 17;
    put16(b, 34, sp);
    return b;
}
std::vector<std::uint8_t> eth_vlan_ipv6_tcp(std::uint16_t sp) {
    std::vector<std::uint8_t> b(78, 0);
    put16(b, 12, 0x8100);
    put16(b, 16, 0x86DD);
    b[18] = 0x60;
    b[18 + 6] = 6;
    b[58 + 12] = 0x50;
    put16(b, 58, sp);
    return b;
}
std::vector<std::uint8_t> eth_qinq_ipv4_udp(std::uint16_t sp) {
    std::vector<std::uint8_t> b(50, 0);
    put16(b, 12, 0x88A8);
    put16(b, 16, 0x8100);
    put16(b, 20, 0x0800);
    b[22] = 0x45;
    b[22 + 9] = 17;
    put16(b, 42, sp);
    return b;
}
std::vector<std::uint8_t> eth_gptp() {
    std::vector<std::uint8_t> b(48, 0);
    put16(b, 12, 0x88F7);
    b[14] = 0x0B;
    return b;
}

// std::tuple<std::vector<...>...> and std::vector both have operator==, so a node table compares with its
// packet_id vector + its columns tuple directly. Fold that over every node.
template <class Tables>
bool tables_equal(const Tables& a, const Tables& b) {
    bool ok = true;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((ok = ok && std::get<I>(a).packet_id == std::get<I>(b).packet_id &&
                std::get<I>(a).columns == std::get<I>(b).columns),
         ...);
    }(std::make_index_sequence<std::tuple_size_v<Tables>>{});
    return ok;
}

}  // namespace

int main() {
    const int root = nanotins::kEthRoot;

    // A mixed batch (multiple PDUs per node across packets; QinQ gives two VLANs in one packet).
    std::vector<std::vector<std::uint8_t>> bufs;
    bufs.push_back(eth_ipv4_udp(0x1111));
    bufs.push_back(eth_vlan_ipv6_tcp(0x2222));
    bufs.push_back(eth_qinq_ipv4_udp(0x3333));
    bufs.push_back(eth_gptp());
    bufs.push_back(eth_ipv4_udp(0x4444));
    bufs.push_back(eth_vlan_ipv6_tcp(0x5555));

    std::vector<nanotins::dag_packet> pkts;
    for (std::size_t i = 0; i < bufs.size(); ++i) {
        pkts.push_back({bufs[i].data(), bufs[i].size(), 100 + i});
    }

    // Reference: sequential append decode.
    nanotins::dag_tables<G> ref;
    for (const auto& p : pkts) {
        nanotins::dag_decode_packet<G>(p.packet_id, p.data, p.size, ref, root);
    }

    // Bulk, serial executor.
    nanotins::dag_tables<G> bulk_serial;
    nanotins::dag_decode_bulk<G>(pkts, bulk_serial, root,
                                 [](std::size_t nt, std::size_t n, auto k) {
                                     nanotins::serial_for_each(nt, n, k);
                                 });

    // Bulk, thread-pool executor (the real parallel scatter).
    nanotins::dag_tables<G> bulk_pool;
    exec::static_thread_pool pool{4};
    auto sched = pool.get_scheduler();
    nanotins::dag_decode_bulk<G>(pkts, bulk_pool, root,
                                 [&](std::size_t nt, std::size_t n, auto k) {
                                     nanotins::bulk_for_each(sched, nt, n, k);
                                 });

    CHECK(tables_equal(ref, bulk_serial));
    CHECK(tables_equal(ref, bulk_pool));

    // sanity: the batch actually exercised multi-row nodes (eth x5 non-gptp-root... eth x6, vlan x4, udp x2)
    constexpr std::size_t kEth = nanotins::node_id_v<nanotins::EthNode, G>;
    constexpr std::size_t kVlan = nanotins::node_id_v<nanotins::VlanNode, G>;
    constexpr std::size_t kUdp = nanotins::node_id_v<nanotins::UdpNode, G>;
    CHECK(std::get<kEth>(ref).size() == 6);   // every packet has Ethernet
    CHECK(std::get<kVlan>(ref).size() == 4);  // 1 + 2(QinQ) + 1
    CHECK(std::get<kUdp>(ref).size() == 3);   // packets 0, 2, 4

    std::printf("dag_bulk: ok (count->scan->scatter == sequential, serial == pool; eth=%zu vlan=%zu udp=%zu)\n",
                std::get<kEth>(ref).size(), std::get<kVlan>(ref).size(), std::get<kUdp>(ref).size());
    return 0;
}
