// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// The DAG twin of protocols::decode_window — decode one window of packets into the per-node DAG tables and
// (optionally) the per-packet L4 boundary used to build remainder_after_l4. This is the driver glue that
// bridges the generic spec_dag engine to the L2/L3/L4 specifics the driver still needs from the old path
// (protocols::WalkResult / pack_ports for the remainder), so the one-shot --decode-l2l3 output is produced
// by the spec/DAG instead of the hand-written walk. Byte-identical to the old per-PDU tables (proven in
// test_pdu_table_interop / test_pdu_table_lance_interop).
//
// Same shape as decode_window: count -> exclusive prefix-sum -> scatter (via dag_decode_bulk), appends to
// `out` across windows, runs on the caller's executor (CPU pool / serial; the GPU path is the bridge's
// dag_decode_window_gpu). The trailer is a second read-only walk that reports each packet's L4 boundary.

#include "nanotins/dag_bulk.hpp"
#include "nanotins/dag_decode.hpp"
#include "nanotins/ipv6_children_bulk.hpp"  // ipv6_child_tables, ipv6_children_bulk
#include "nanotins/protocol_decode.hpp"  // protocols::WalkResult, pack_ports, kLinkTypeEthernet
#include "nanotins/protocol_specs.hpp"
#include "nanotins/spec_dag.hpp"

#include "nanotins/pcap_blocks.hpp"  // pcapblocks::Bytes

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pcapng2lance {

// One packet's L4 boundary, reconstructed from the DAG walk so the remainder rows match the old walk:
// reached_l4 == the walk ended on TCP/UDP; l4_payload_offset == total header bytes (the payload start);
// l4_ports == the packed src+dst ports (the L5 dispatch key). A read-only walk (no scatter), device-safe.
inline NANOTINS_HD protocols::WalkResult dag_l4_trailer(int root, const std::uint8_t* p,
                                                        std::size_t size) noexcept {
    using namespace nanotins::literals;
    protocols::WalkResult res;
    if (p == nullptr || size == 0) {
        return res;
    }
    constexpr int kTcp = nanotins::node_id_v<nanotins::TcpNode, nanotins::L2L3Graph>;
    constexpr int kUdp = nanotins::node_id_v<nanotins::UdpNode, nanotins::L2L3Graph>;
    const nanotins::walk_result wr = nanotins::walk<nanotins::L2L3Graph>(
        root, p, size, [&](auto Ic, std::size_t off) {
            constexpr int I = static_cast<int>(decltype(Ic)::value);
            if constexpr (I == kUdp) {
                nanotins::struct_view<nanotins::UdpSpec> v(p + off);
                res.l4_ports = protocols::pack_ports(v("src_port"_fld), v("dst_port"_fld));
                res.reached_l4 = true;  // TCP/UDP are leaves, so this is the last visit -> stays set
            } else if constexpr (I == kTcp) {
                nanotins::struct_view<nanotins::TcpSpec> v(p + off);
                res.l4_ports = protocols::pack_ports(v("src_port"_fld), v("dst_port"_fld));
                res.reached_l4 = true;
            }
        });
    res.l4_payload_offset = wr.consumed;  // first un-parsed byte == payload start when reached_l4
    return res;
}

// `run_each(num_tasks, n, kernel)` is the execution policy (a bulk runner bound to a scheduler, or
// serial_for_each) — identical output either way. Appends to `out` across windows. `trailers` (optional,
// pre-sized to n) receives each packet's L4 boundary.
template <class Runner>
void dag_decode_window(Runner run_each, std::uint64_t pid_base, const std::uint16_t* link_type,
                       const std::uint64_t* poff, const std::uint32_t* psize, pcapblocks::Bytes window,
                       std::size_t n, nanotins::dag_tables<nanotins::L2L3Graph>& out,
                       protocols::WalkResult* trailers = nullptr,
                       nanotins::ipv6_child_tables* ipv6_kids = nullptr) {
    using G = nanotins::L2L3Graph;
    if (n == 0) {
        return;
    }
    const std::uint8_t* wbase = window.data();
    const std::size_t wsize = window.size();
    const int root = nanotins::kEthRoot;

    // Per-packet spans, gated to empty for non-Ethernet / truncated packets (the DAG root is Ethernet, so an
    // empty span emits nothing — exactly count_packet's link_type gate).
    std::vector<nanotins::dag_packet> pkts(n);
    for (std::size_t i = 0; i < n; ++i) {
        const bool ok = link_type[i] == protocols::kLinkTypeEthernet &&
                        static_cast<std::size_t>(poff[i]) + psize[i] <= wsize;
        pkts[i] = nanotins::dag_packet{ok ? wbase + poff[i] : nullptr,
                                       ok ? static_cast<std::size_t>(psize[i]) : std::size_t{0},
                                       pid_base + i};
    }

    // Rows: the count -> scan -> scatter pipeline (appends at the columns' current sizes).
    nanotins::dag_decode_bulk<G>(pkts, out, root,
                                 [&](std::size_t nt, std::size_t m, auto k) { run_each(nt, m, k); });

    // IPv6 variable-length child records (SRv6 segments + IPv6/SRH options), same count->scan->scatter over
    // the same packet spans — appended across windows like the node tables.
    if (ipv6_kids != nullptr) {
        nanotins::ipv6_children_bulk<G>(pkts, *ipv6_kids, root,
                                        [&](std::size_t nt, std::size_t m, auto k) { run_each(nt, m, k); });
    }

    // Trailers: the L4 boundary per packet (read-only re-walk), for the remainder rows.
    if (trailers != nullptr) {
        const nanotins::dag_packet* pp = pkts.data();
        protocols::WalkResult* tr = trailers;
        run_each(std::min<std::size_t>(n, 64), n,
                 [=](std::size_t i) { tr[i] = dag_l4_trailer(root, pp[i].data, pp[i].size); });
    }
}

}  // namespace pcapng2lance
