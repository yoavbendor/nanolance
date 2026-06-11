#pragma once

// GPU L2/L3/L4 decode — the on-device twin of protocol_decode_bulk.hpp's decode_window, the canonical
// variable-outputs-per-input pattern: count -> exclusive-scan -> scatter, with the scan done by thrust on
// the device. Everything is behind NANOTINS_ENABLE_CUDA so the CPU build is untouched. The kernels are the
// SAME NANOTINS_HD primitives the CPU path uses (count_packet / scatter_packet / walk_packet); only the
// scheduler (nvexec), the pointers (device), and the scan (thrust::exclusive_scan instead of a host loop)
// differ. See nanotins/docs/GPU_BULK_INTEGRATION.md for build/verify and the remaining caveats.
//
// This is written-not-compiled (the author host has no CUDA). Treat it as a near-complete scaffold: the
// structure, device buffers, thrust calls, and D2H append are all here; compile it on the GPU host, fix
// any toolchain-specific syntax, and VERIFY the output is byte-identical to decode_window (CPU).

#include "nanotins/bulk.hpp"
#include "gputins/gpu.hpp"
#include "nanotins/protocol_decode.hpp"

#ifdef NANOTINS_ENABLE_CUDA

#include <thrust/device_ptr.h>
#include <thrust/execution_policy.h>
#include <thrust/functional.h>
#include <thrust/reduce.h>
#include <thrust/scan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace protocols::gpu {

// Append `cnt` device rows (packet_id + T) to a host PduColumn<T>, at its current tail (so windows
// accumulate, exactly like the CPU path's resize-at-current-size).
template <class T>
void append_column(PduColumn<T>& col, const nanotins::gpu::device_buffer<std::uint64_t>& d_pid,
                   const nanotins::gpu::device_buffer<T>& d_rows, std::uint32_t cnt) {
    if (cnt == 0) return;
    const std::size_t base = col.rows.size();
    col.packet_id.resize(base + cnt);
    col.rows.resize(base + cnt);
    d_pid.to_host(col.packet_id.data() + base, cnt);
    d_rows.to_host(col.rows.data() + base, cnt);
}

// Decode one window's packets on the GPU. `link_type/poff/psize` are host arrays (per packet); `window` is
// the host window bytes. Results are appended to `out`; `trailers` (host, size n, optional) receives each
// packet's WalkResult (L4 boundary) for the remainder, mirroring decode_window.
template <class Scheduler>
void decode_window_gpu(Scheduler gpu_sch, std::size_t num_tasks, std::uint64_t pid_base,
                       const std::uint16_t* link_type, const std::uint64_t* poff, const std::uint32_t* psize,
                       Bytes window, std::size_t n, DecodedPdus& out, WalkResult* trailers = nullptr) {
    if (n == 0) return;

    // --- H2D inputs ---
    nanotins::gpu::device_buffer<std::uint8_t> d_win(window.size());
    d_win.to_device(window.data(), window.size());
    nanotins::gpu::device_buffer<std::uint16_t> d_link(n);
    d_link.to_device(link_type, n);
    nanotins::gpu::device_buffer<std::uint64_t> d_off(n);
    d_off.to_device(poff, n);
    nanotins::gpu::device_buffer<std::uint32_t> d_sz(n);
    d_sz.to_device(psize, n);
    const std::uint8_t* win = d_win.get();
    const std::size_t wsize = window.size();
    const std::uint16_t* lt = d_link.get();
    const std::uint64_t* off = d_off.get();
    const std::uint32_t* sz = d_sz.get();

    // --- Pass 1: count PDUs per packet (device) ---
    nanotins::gpu::device_buffer<PduCounts> d_counts(n);
    {
        PduCounts* counts = d_counts.get();
        nanotins::bulk_for_each(gpu_sch, num_tasks, n, [=](std::size_t i) {
            Bytes pkt{};
            if (off[i] + sz[i] <= wsize) pkt = Bytes(win + off[i], sz[i]);
            counts[i] = count_packet(lt[i], pkt);
        });
    }

    // --- Exclusive prefix-sum per PDU type, all six at once via thrust::plus<PduCounts> ---
    nanotins::gpu::device_buffer<PduCounts> d_bases(n);
    thrust::device_ptr<PduCounts> counts_b(d_counts.get());
    thrust::device_ptr<PduCounts> bases_b(d_bases.get());
    thrust::exclusive_scan(thrust::device, counts_b, counts_b + n, bases_b, PduCounts{},
                           thrust::plus<PduCounts>{});
    const PduCounts total =
        thrust::reduce(thrust::device, counts_b, counts_b + n, PduCounts{}, thrust::plus<PduCounts>{});

    // --- Size the device output columns to this window's totals ---
    nanotins::gpu::device_buffer<std::uint64_t> eth_pid(total.eth);
    nanotins::gpu::device_buffer<Ethernet> eth_rows(total.eth);
    nanotins::gpu::device_buffer<std::uint64_t> vlan_pid(total.vlan);
    nanotins::gpu::device_buffer<VlanTag> vlan_rows(total.vlan);
    nanotins::gpu::device_buffer<std::uint64_t> ipv4_pid(total.ipv4);
    nanotins::gpu::device_buffer<Ipv4> ipv4_rows(total.ipv4);
    nanotins::gpu::device_buffer<std::uint64_t> ipv6_pid(total.ipv6);
    nanotins::gpu::device_buffer<Ipv6> ipv6_rows(total.ipv6);
    nanotins::gpu::device_buffer<std::uint64_t> tcp_pid(total.tcp);
    nanotins::gpu::device_buffer<Tcp> tcp_rows(total.tcp);
    nanotins::gpu::device_buffer<std::uint64_t> udp_pid(total.udp);
    nanotins::gpu::device_buffer<Udp> udp_rows(total.udp);

    // --- Pass 2: scatter each PDU into its prefix-summed slot (device); capture the L4 boundary ---
    nanotins::gpu::device_buffer<WalkResult> d_tr(n);
    {
        PduSink sink{eth_pid.get(), eth_rows.get(), vlan_pid.get(), vlan_rows.get(),
                     ipv4_pid.get(), ipv4_rows.get(), ipv6_pid.get(), ipv6_rows.get(),
                     tcp_pid.get(),  tcp_rows.get(),  udp_pid.get(),  udp_rows.get()};
        const PduCounts* bases = d_bases.get();
        WalkResult* tr = d_tr.get();
        nanotins::bulk_for_each(gpu_sch, num_tasks, n, [=](std::size_t i) {
            Bytes pkt{};
            if (off[i] + sz[i] <= wsize) pkt = Bytes(win + off[i], sz[i]);
            const WalkResult w = scatter_packet(pid_base + i, lt[i], pkt, bases[i], sink);
            if (tr != nullptr) tr[i] = w;
        });
    }

    // --- D2H: append each column to the host DecodedPdus, and the trailers ---
    append_column(out.ethernet, eth_pid, eth_rows, total.eth);
    append_column(out.vlan, vlan_pid, vlan_rows, total.vlan);
    append_column(out.ipv4, ipv4_pid, ipv4_rows, total.ipv4);
    append_column(out.ipv6, ipv6_pid, ipv6_rows, total.ipv6);
    append_column(out.tcp, tcp_pid, tcp_rows, total.tcp);
    append_column(out.udp, udp_pid, udp_rows, total.udp);
    if (trailers != nullptr) d_tr.to_host(trailers, n);
}

}  // namespace protocols::gpu

#endif  // NANOTINS_ENABLE_CUDA
