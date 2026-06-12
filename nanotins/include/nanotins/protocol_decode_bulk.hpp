#pragma once

// Scheduler-agnostic bulk L2/L3/L4 decode of one window of packets, the canonical GPU pattern for a
// variable-number-of-outputs-per-input problem: two device-safe bulk passes bracketing a prefix-sum.
//
//   Pass 1 (bulk)   count_packet per packet            -> per-packet PDU counts
//   Scan            exclusive prefix-sum per PDU type   -> per-packet write bases + exact totals
//   resize          size each output column to its total (no over-allocation, no push_back)
//   Pass 2 (bulk)   scatter_packet per packet           -> each PDU written to its own prefix-summed slot
//
// Both passes run through nanotins::bulk_for_each on the SAME scheduler the L1 parse uses; on a CUDA host
// it swaps to an nvexec scheduler and the identical kernels run on the GPU (the scan becomes a
// thrust::exclusive_scan). Row order is packet order (the scan sums counts in index order), so the output
// tables are byte-identical to the serial decode_packet path. Appends to `out` across windows by seeding
// the scan at each column's current size.

#include "nanotins/bulk.hpp"
#include "nanotins/protocol_decode.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace protocols {

// `run` is the execution policy: a callable `run(num_tasks, n, kernel)` that runs kernel(i) for i in
// [0,n). Pass a bulk runner (nanotins::bulk_for_each bound to a scheduler) for the parallel path, or
// nanotins::serial_for_each for the sequential reference/debug path — the output is identical either way.
// `trailers` (optional, pre-sized to n) receives each packet's WalkResult — the L4-payload boundary —
// from the scatter pass, so the caller can build the remainder_after_l4 rows from the same traversal.
template <class Runner>
void decode_window(Runner run_each, std::uint64_t pid_base, const std::uint16_t* link_type,
                   const std::uint64_t* poff, const std::uint32_t* psize, Bytes window, std::size_t n,
                   DecodedPdus& out, WalkResult* trailers = nullptr) {
    if (n == 0) {
        return;
    }
    const std::uint8_t* wbase = window.data();
    const std::size_t wsize = window.size();
    const std::size_t num_tasks = std::min<std::size_t>(n, 64);

    // Pass 1: count PDUs per packet (device-safe; no writes to shared output).
    std::vector<PduCounts> per(n);
    {
        PduCounts* pc = per.data();
        const std::uint64_t* off = poff;
        const std::uint32_t* sz = psize;
        const std::uint16_t* lt = link_type;
        run_each(num_tasks, n, [=](std::size_t i) {
            Bytes pkt{};
            if (off[i] + sz[i] <= wsize) {
                pkt = Bytes(wbase + off[i], sz[i]);
            }
            pc[i] = count_packet(lt[i], pkt);
        });
    }

    // Exclusive prefix-sum per PDU type, seeded at the columns' current sizes (append across windows). The
    // per-type running sum is one `run = run + per[i]` (PduCounts::operator+ is component-wise); seeding
    // and the final resize fold over the columns (seed_counts / resize_columns) — no hand-unrolled types.
    PduCounts run = seed_counts(out);
    std::vector<PduCounts> base(n);
    for (std::size_t i = 0; i < n; ++i) {
        base[i] = run;
        run = run + per[i];
    }
    resize_columns(out, run);  // size each output column exactly to its new total

    // Pass 2: re-walk and scatter each PDU into its prefix-summed slot (device-safe; disjoint writes).
    {
        PduSink sink{out.ethernet.packet_id.data(), out.ethernet.rows.data(), out.vlan.packet_id.data(),
                     out.vlan.rows.data(),          out.ipv4.packet_id.data(), out.ipv4.rows.data(),
                     out.ipv6.packet_id.data(),     out.ipv6.rows.data(),      out.tcp.packet_id.data(),
                     out.tcp.rows.data(),           out.udp.packet_id.data(),  out.udp.rows.data()};
        const PduCounts* b = base.data();
        const std::uint64_t* off = poff;
        const std::uint32_t* sz = psize;
        const std::uint16_t* lt = link_type;
        WalkResult* tr = trailers;
        run_each(num_tasks, n, [=](std::size_t i) {
            Bytes pkt{};
            if (off[i] + sz[i] <= wsize) {
                pkt = Bytes(wbase + off[i], sz[i]);
            }
            const WalkResult w = scatter_packet(pid_base + i, lt[i], pkt, b[i], sink);
            if (tr != nullptr) {
                tr[i] = w;
            }
        });
    }
}

}  // namespace protocols
