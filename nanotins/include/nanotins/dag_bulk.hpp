#pragma once

// The bulk/GPU shape of the DAG decode: count -> exclusive prefix-sum -> scatter, the same data-parallel
// pattern protocol_decode_bulk.hpp uses for the fixed six PDU types, but generic over a spec_dag graph.
//
//   count_packet_dag(p)      -> per-node PDU counts (node_counts, a POD with operator+ for the scan)
//   scatter_packet_dag(...)  -> re-walk and write each emitted PDU into its node's prefix-summed slot
//
// Both are NANOTINS_HD and capture only POD (node_counts, dag_sink = arrays of raw pointers — trivially
// copyable, so a GPU bulk kernel can memcpy them, unlike a std::tuple pack). Every write index is derived
// solely from this packet's base + its own emit order, so no two packets touch the same slot: identical
// output whether run sequentially, on a CPU pool, or on the GPU (CPU sequential==bulk proven here; the GPU
// wiring reuses parse_spec_gpu's H2D/D2H around these same kernels).
//
// dag_decode_bulk drives the whole thing on the host into dag_tables<Graph> — the SAME type the sequential
// dag_decode_packet fills — so the two are compared row-for-row.

#include "nanotins/bulk.hpp"
#include "nanotins/dag_decode.hpp"
#include "nanotins/spec_dag.hpp"
#include "nanotins/wire_spec.hpp"
#include "nanotins/wire_spec_soa.hpp"  // scatter helpers shape

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace nanotins {

// ---- per-node counts: a POD vector of node counts, addable for the prefix-sum ------------------------
template <std::size_t NumNodes>
struct node_counts {
    std::uint32_t n[NumNodes];

    NANOTINS_HD node_counts operator+(const node_counts& o) const {
        node_counts r{};
        for (std::size_t i = 0; i < NumNodes; ++i) {
            r.n[i] = n[i] + o.n[i];
        }
        return r;
    }
};

// Count how many PDUs of each node a packet emits — no writes, device-safe (mirrors count_packet).
template <class Graph>
NANOTINS_HD inline node_counts<Graph::size> count_packet_dag(int root, const std::uint8_t* p,
                                                             std::size_t size) noexcept {
    node_counts<Graph::size> c{};
    walk<Graph>(root, p, size, [&](auto Ic, std::size_t) { ++c.n[decltype(Ic)::value]; });
    return c;
}

// ---- the POD scatter sink: raw column + packet_id pointers per node ----------------------------------
namespace detail_dag {
template <class Graph>
struct max_cols_h;
template <class... Nodes>
struct max_cols_h<graph<Nodes...>> {
    static constexpr std::size_t value = std::max({std::size_t{1}, Nodes::spec::field_count...});
};
}  // namespace detail_dag

template <class Graph>
struct dag_sink {
    static constexpr std::size_t NumNodes = Graph::size;
    static constexpr std::size_t MaxCols = detail_dag::max_cols_h<Graph>::value;
    std::uint64_t* pid[NumNodes];     // packet_id column per node
    void* col[NumNodes][MaxCols];     // one elem* per spec field per node (unused slots null)
};

// Write one PDU's fields into column row i (the per-node sibling of scatter_spec_pod, over a void* row).
template <class Spec>
NANOTINS_HD inline void scatter_spec_pod_row(void* const* cols, std::size_t i,
                                             const std::uint8_t* pdu) noexcept {
    using fields = spec_fields_t<Spec>;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((static_cast<typename std::tuple_element_t<I, fields>::value_type*>(cols[I])[i] =
              read_field<std::tuple_element_t<I, fields>>(pdu)),
         ...);
    }(std::make_index_sequence<Spec::field_count>{});
}

// Re-walk a packet, writing each emitted PDU to base[node] + its own per-node emit order. Device-safe: the
// local cursor array starts at this packet's bases (from the exclusive scan), so writes never collide.
template <class Graph>
NANOTINS_HD inline void scatter_packet_dag(std::uint64_t packet_id, int root, const std::uint8_t* p,
                                           std::size_t size, const node_counts<Graph::size>& base,
                                           const dag_sink<Graph>& sink) noexcept {
    std::uint32_t cur[Graph::size];
    for (std::size_t i = 0; i < Graph::size; ++i) {
        cur[i] = base.n[i];
    }
    walk<Graph>(root, p, size, [&](auto Ic, std::size_t off) {
        constexpr std::size_t I = decltype(Ic)::value;
        using N = std::tuple_element_t<I, typename Graph::nodes>;
        const std::uint32_t slot = cur[I];
        sink.pid[I][slot] = packet_id;
        scatter_spec_pod_row<typename N::spec>(sink.col[I], slot, p + off);
        ++cur[I];
    });
}

// ---- host harness: count + scan + scatter into dag_tables<Graph> -------------------------------------
namespace detail_dag {
// Point node I's sink slots (packet_id + each column) at its already-resized table vectors.
template <class Graph, std::size_t I>
inline void bind_one(dag_sink<Graph>& s, dag_tables<Graph>& tabs) {
    auto& t = std::get<I>(tabs);
    s.pid[I] = t.packet_id.data();
    [&]<std::size_t... C>(std::index_sequence<C...>) {
        ((s.col[I][C] = std::get<C>(t.columns).data()), ...);
    }(std::make_index_sequence<std::decay_t<decltype(t)>::ncols>{});
}

// Point a dag_sink at the (already-resized) columns of dag_tables<Graph>.
template <class Graph>
inline dag_sink<Graph> bind_sink(dag_tables<Graph>& tabs) {
    dag_sink<Graph> s{};
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (bind_one<Graph, I>(s, tabs), ...);
    }(std::make_index_sequence<Graph::size>{});
    return s;
}

// Resize node I's table (packet_id + every column vector) to `n`.
template <class Table>
inline void resize_table(Table& t, std::size_t n) {
    t.packet_id.resize(n);
    [&]<std::size_t... C>(std::index_sequence<C...>) {
        ((std::get<C>(t.columns).resize(n)), ...);
    }(std::make_index_sequence<Table::ncols>{});
}
}  // namespace detail_dag

// A packet to decode: its bytes + the packet_id to stamp on its rows.
struct dag_packet {
    const std::uint8_t* data;
    std::size_t size;
    std::uint64_t packet_id;
};

// Bulk-decode packets into per-node tables via count -> exclusive scan -> scatter. Appends to whatever the
// tables already hold (seeded from their current sizes), so it composes across windows. `executor` is
// bulk_for_each (CPU pool / GPU stream) or serial_for_each; the scatter is identical either way.
template <class Graph, class Executor>
void dag_decode_bulk(const std::vector<dag_packet>& pkts, dag_tables<Graph>& tabs, int root,
                     Executor&& executor) {
    constexpr std::size_t NN = Graph::size;
    const std::size_t np = pkts.size();

    // 1) count pass + 2) exclusive prefix-sum, seeded from current table sizes (cross-batch append).
    std::vector<node_counts<NN>> base(np);
    node_counts<NN> run{};
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((run.n[I] = static_cast<std::uint32_t>(std::get<I>(tabs).size())), ...);
    }(std::make_index_sequence<NN>{});
    for (std::size_t k = 0; k < np; ++k) {
        base[k] = run;
        run = run + count_packet_dag<Graph>(root, pkts[k].data, pkts[k].size);
    }

    // 3) size every node table to its post-scan total (no over-allocation, no push_back).
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((detail_dag::resize_table(std::get<I>(tabs), run.n[I])), ...);
    }(std::make_index_sequence<NN>{});

    // 4) bind the POD sink and scatter every packet into its prefix-summed slots.
    const dag_sink<Graph> sink = detail_dag::bind_sink<Graph>(tabs);
    const dag_packet* pp = pkts.data();
    const node_counts<NN>* bp = base.data();
    const std::size_t num_tasks = std::min<std::size_t>(np, 64);  // match decode_window's partitioning
    executor(num_tasks, np, [=](std::size_t k) {
        scatter_packet_dag<Graph>(pp[k].packet_id, root, pp[k].data, pp[k].size, bp[k], sink);
    });
}

}  // namespace nanotins
