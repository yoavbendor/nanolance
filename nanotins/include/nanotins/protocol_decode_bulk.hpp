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

template <class Scheduler>
void decode_window_bulk(Scheduler sched, std::uint64_t pid_base, const std::uint16_t* link_type,
                        const std::uint64_t* poff, const std::uint32_t* psize, Bytes window, std::size_t n,
                        DecodedPdus& out) {
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
        nanotins::bulk_for_each(sched, num_tasks, n, [=](std::size_t i) {
            Bytes pkt{};
            if (off[i] + sz[i] <= wsize) {
                pkt = Bytes(wbase + off[i], sz[i]);
            }
            pc[i] = count_packet(lt[i], pkt);
        });
    }

    // Exclusive prefix-sum per PDU type, seeded at the columns' current sizes (append across windows).
    PduCounts run{static_cast<std::uint32_t>(out.ethernet.size()), static_cast<std::uint32_t>(out.vlan.size()),
                  static_cast<std::uint32_t>(out.ipv4.size()),     static_cast<std::uint32_t>(out.ipv6.size()),
                  static_cast<std::uint32_t>(out.tcp.size()),      static_cast<std::uint32_t>(out.udp.size())};
    std::vector<PduCounts> base(n);
    for (std::size_t i = 0; i < n; ++i) {
        base[i] = run;
        run.eth += per[i].eth;
        run.vlan += per[i].vlan;
        run.ipv4 += per[i].ipv4;
        run.ipv6 += per[i].ipv6;
        run.tcp += per[i].tcp;
        run.udp += per[i].udp;
    }

    // Size each output column exactly to its new total (the running sums after the scan).
    out.ethernet.packet_id.resize(run.eth);
    out.ethernet.rows.resize(run.eth);
    out.vlan.packet_id.resize(run.vlan);
    out.vlan.rows.resize(run.vlan);
    out.ipv4.packet_id.resize(run.ipv4);
    out.ipv4.rows.resize(run.ipv4);
    out.ipv6.packet_id.resize(run.ipv6);
    out.ipv6.rows.resize(run.ipv6);
    out.tcp.packet_id.resize(run.tcp);
    out.tcp.rows.resize(run.tcp);
    out.udp.packet_id.resize(run.udp);
    out.udp.rows.resize(run.udp);

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
        nanotins::bulk_for_each(sched, num_tasks, n, [=](std::size_t i) {
            Bytes pkt{};
            if (off[i] + sz[i] <= wsize) {
                pkt = Bytes(wbase + off[i], sz[i]);
            }
            scatter_packet(pid_base + i, lt[i], pkt, b[i], sink);
        });
    }
}

}  // namespace protocols
