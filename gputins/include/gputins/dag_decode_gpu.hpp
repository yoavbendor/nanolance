#pragma once

// GPU spec/DAG decode — the on-device twin of nanotins::dag_decode_window (the CPU path in
// examples/pcapng2lance/include/dag_decode_window.hpp) and the generic successor to protocol_decode_gpu.hpp.
// Same count -> exclusive-scan -> scatter pattern, but generic over a spec_dag graph, reusing the EXACT
// device-safe POD kernels the CPU bulk path uses (count_packet_dag / scatter_packet_dag / dag_sink /
// node_counts in nanotins/dag_bulk.hpp). Only the scheduler (nvexec), the pointers (device), and the scan
// (thrust::exclusive_scan) differ.
//
// ============================ FOR THE GPU-HOST BUILD (read this) ============================
// This file is WRITTEN-NOT-COMPILED (the author host has no CUDA), exactly like protocol_decode_gpu.hpp.
// Treat it as a near-complete scaffold: structure, device buffers, thrust calls, and D2H append are all
// here. On the GPU host:
//   1. Build with -DNANOTINS_ENABLE_CUDA=ON; fix any toolchain-specific syntax.
//   2. VERIFY byte-identity: run the driver --decode-l2l3 with --gpu and without (CPU dag_decode_window),
//      dump each *_ethernet/_vlan/_ipv4/_ipv6/_tcp/_udp.lance with nlance2table, and diff. They MUST match
//      (the CPU path is already proven == the old protocols:: tables by test_pdu_table_lance_interop).
// Design choices that should make that easy:
//   * The scatter reuses scatter_packet_dag + dag_sink unchanged — the same code the CPU bulk path runs, so
//     correctness reduces to "did the device pointers + scan line up", not "is the parse right".
//   * Every column (and packet_id) is a uniform device_buffer<uint8_t> of total*sizeof(elem) bytes, pointed
//     at by the POD dag_sink (col[] is void*). So there is NO per-spec-type device buffer zoo to maintain;
//     adding a protocol/column needs no change here. D2H is a raw byte copy into the host column vectors
//     (which are contiguous POD), appended at each column's current size (cross-window accumulation).
//   * Likely fix points: thrust::plus<node_counts<NN>> over a POD-with-operator+ (should be fine), the
//     reinterpret_cast<uint64_t*> for packet_id, and device_buffer move/lifetime in the std::vector.
// ===========================================================================================

#include "nanotins/dag_bulk.hpp"
#include "nanotins/dag_decode.hpp"
#include "nanotins/spec_dag.hpp"
#include "nanotins/struct_spec_soa.hpp"  // columns_of_spec
#include "gputins/gpu.hpp"

#ifdef NANOTINS_ENABLE_CUDA

#include <thrust/device_ptr.h>
#include <thrust/execution_policy.h>
#include <thrust/functional.h>
#include <thrust/reduce.h>
#include <thrust/scan.h>

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace nanotins::gpu {

// Byte width of column C of spec S (sizeof(elem); for fixed-size-binary std::array<u8,N> this is N).
template <class Spec, std::size_t C>
constexpr std::size_t spec_col_bytes() {
    using cols = columns_of_spec<Spec>;
    return sizeof(typename std::tuple_element_t<C, cols>::elem);
}

// Decode one window's packets on the GPU into the per-node DAG tables. `link_type/poff/psize` are host
// arrays (per packet); `window` is the host window bytes. Results are APPENDED to `out` (so windows
// accumulate, like the CPU path). Trailers (the L4 boundary) are computed on the host by the bridge.
template <class Graph, class Scheduler>
void dag_decode_window_gpu(Scheduler sch, std::size_t num_tasks, std::uint64_t pid_base,
                           const std::uint16_t* link_type, const std::uint64_t* poff,
                           const std::uint32_t* psize, ::pcapblocks::Bytes window, std::size_t n,
                           dag_tables<Graph>& out) {
    constexpr std::size_t NN = Graph::size;
    if (n == 0) {
        return;
    }
    constexpr int root = node_id_v<EthNode, Graph>;  // L2L3Graph root; generic on the graph's first node id

    // --- H2D inputs ---
    device_buffer<std::uint8_t> d_win(window.size());
    d_win.to_device(window.data(), window.size());
    device_buffer<std::uint16_t> d_link(n);
    d_link.to_device(link_type, n);
    device_buffer<std::uint64_t> d_off(n);
    d_off.to_device(poff, n);
    device_buffer<std::uint32_t> d_sz(n);
    d_sz.to_device(psize, n);
    const std::uint8_t* win = d_win.get();
    const std::size_t wsize = window.size();
    const std::uint16_t* lt = d_link.get();
    const std::uint64_t* off = d_off.get();
    const std::uint32_t* sz = d_sz.get();

    // Per-packet device span, gated to empty for non-Ethernet / truncated (DAG root is Ethernet).
    auto span_of = [=] NANOTINS_HD(std::size_t i, const std::uint8_t*& p, std::size_t& s) {
        const bool ok = lt[i] == 1u /*LINKTYPE_ETHERNET*/ && off[i] + sz[i] <= wsize;
        p = ok ? win + off[i] : nullptr;
        s = ok ? sz[i] : std::size_t{0};
    };

    // --- Pass 1: count PDUs per packet (device) ---
    device_buffer<node_counts<NN>> d_counts(n);
    {
        node_counts<NN>* counts = d_counts.get();
        nanotins::bulk_for_each(sch, num_tasks, n, [=](std::size_t i) {
            const std::uint8_t* p = nullptr;
            std::size_t s = 0;
            span_of(i, p, s);
            counts[i] = count_packet_dag<Graph>(root, p, s);
        });
    }

    // --- Exclusive prefix-sum + total over node_counts (all nodes at once via thrust::plus) ---
    device_buffer<node_counts<NN>> d_bases(n);
    thrust::device_ptr<node_counts<NN>> counts_b(d_counts.get());
    thrust::device_ptr<node_counts<NN>> bases_b(d_bases.get());
    thrust::exclusive_scan(thrust::device, counts_b, counts_b + n, bases_b, node_counts<NN>{},
                           thrust::plus<node_counts<NN>>{});
    const node_counts<NN> total =
        thrust::reduce(thrust::device, counts_b, counts_b + n, node_counts<NN>{}, thrust::plus<node_counts<NN>>{});

    // --- Allocate uniform byte buffers: per node, one packet_id buffer + one buffer per column ---
    // Pushed in a fixed order (node 0 pid, node 0 cols..., node 1 pid, ...) so the sink fold below reads
    // them back by the same running index.
    std::vector<device_buffer<std::uint8_t>> bufs;
    auto alloc_node_buffers = [&]<std::size_t I>() {
        using NodeI = std::tuple_element_t<I, typename Graph::nodes>;
        using SpecI = typename NodeI::spec;
        const std::size_t cnt = static_cast<std::size_t>(total.n[I]);
        // node I: pid (cnt*8 bytes) then each column (cnt*sizeof(elem) bytes)
        bufs.emplace_back(cnt * sizeof(std::uint64_t));
        [&]<std::size_t... C>(std::index_sequence<C...>) {
            (bufs.emplace_back(cnt * spec_col_bytes<SpecI, C>()), ...);
        }(std::make_index_sequence<SpecI::field_count>{});
    };
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (alloc_node_buffers.template operator()<I>(), ...);
    }(std::make_index_sequence<NN>{});

    // --- Build the POD device sink from the byte buffers (same push order, running index) ---
    dag_sink<Graph> sink{};
    std::size_t bi = 0;
    auto bind_node_sink = [&]<std::size_t I>() {
        using NodeI = std::tuple_element_t<I, typename Graph::nodes>;
        using SpecI = typename NodeI::spec;
        sink.pid[I] = reinterpret_cast<std::uint64_t*>(bufs[bi++].get());
        [&]<std::size_t... C>(std::index_sequence<C...>) {
            ((sink.col[I][C] = static_cast<void*>(bufs[bi++].get())), ...);
        }(std::make_index_sequence<SpecI::field_count>{});
    };
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (bind_node_sink.template operator()<I>(), ...);
    }(std::make_index_sequence<NN>{});

    // --- Pass 2: scatter each PDU into its prefix-summed slot (device) ---
    {
        const node_counts<NN>* bases = d_bases.get();
        nanotins::bulk_for_each(sch, num_tasks, n, [=](std::size_t i) {
            const std::uint8_t* p = nullptr;
            std::size_t s = 0;
            span_of(i, p, s);
            scatter_packet_dag<Graph>(pid_base + i, root, p, s, bases[i], sink);
        });
    }

    // --- D2H: append each node's columns to the host tables at their current size (cross-window accum) ---
    bi = 0;
    auto copy_node_to_host = [&]<std::size_t I>() {
        using NodeI = std::tuple_element_t<I, typename Graph::nodes>;
        using SpecI = typename NodeI::spec;
        auto& table = std::get<I>(out);
        const std::size_t cnt = static_cast<std::size_t>(total.n[I]);
        const std::size_t base = table.packet_id.size();
        table.packet_id.resize(base + cnt);
        [&]<std::size_t... C>(std::index_sequence<C...>) {
            ((std::get<C>(table.columns).resize(base + cnt)), ...);
        }(std::make_index_sequence<SpecI::field_count>{});

        if (cnt > 0) {
            bufs[bi].to_host(reinterpret_cast<std::uint8_t*>(table.packet_id.data() + base),
                             cnt * sizeof(std::uint64_t));
            ++bi;
            [&]<std::size_t... C>(std::index_sequence<C...>) {
                ((bufs[bi++].to_host(reinterpret_cast<std::uint8_t*>(std::get<C>(table.columns).data() + base),
                                     cnt * sizeof(typename std::tuple_element_t<C, columns_of_spec<SpecI>>::elem))),
                 ...);
            }(std::make_index_sequence<SpecI::field_count>{});
        } else {
            // skip this node's buffers (pid + ncols) in the running index
            bi += 1 + SpecI::field_count;
        }
    };
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (copy_node_to_host.template operator()<I>(), ...);
    }(std::make_index_sequence<NN>{});
}

}  // namespace nanotins::gpu

#endif  // NANOTINS_ENABLE_CUDA
