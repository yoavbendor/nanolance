#pragma once

// DAG-driven columnar decode: walk a packet through a spec_dag graph and scatter each emitted PDU's fields
// into that node's columns. This is the host accumulator (one growable column set per node, plus a
// packet_id column linking each row to its packet) — the spec-driven analogue of protocols::DecodedPdus,
// but generated from the graph instead of hand-listing the six PDU types. The columns are already SoA
// (one vector per field), so each node's table is Lance-ready via to_arrow over the same spec.
//
// The fill is the spec's own read_field, so the column values are byte-identical to struct_view / the
// described-struct overlay (verified in test_dag_decode). The device bulk path (fixed-N spec_soa + the POD
// scatter) is a separate, later step; this is the sequential reference + the parity oracle for it.

#include "nanotins/spec_dag.hpp"
#include "nanotins/struct_spec.hpp"

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace nanotins {

namespace detail_dag {
template <class FieldsTuple>
struct vecs_of;
template <class... F>
struct vecs_of<std::tuple<F...>> {
    using type = std::tuple<std::vector<typename F::value_type>...>;
};
}  // namespace detail_dag

// One node's columnar table: a packet_id column + one std::vector per spec field (SoA), filled by read_field.
template <class Spec>
struct dag_pdu_table {
    using fields = spec_fields_t<Spec>;
    static constexpr std::size_t ncols = std::tuple_size_v<fields>;

    std::vector<std::uint64_t> packet_id;
    typename detail_dag::vecs_of<fields>::type columns;

    std::size_t size() const { return packet_id.size(); }

    void append(std::uint64_t pkt, const std::uint8_t* pdu) {
        packet_id.push_back(pkt);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((std::get<I>(columns).push_back(read_field<std::tuple_element_t<I, fields>>(pdu))), ...);
        }(std::make_index_sequence<ncols>{});
    }

    // Column I's data (in spec field order); pairs with std::tuple_element_t<I, fields>::name().
    template <std::size_t I>
    const auto& column() const {
        return std::get<I>(columns);
    }
};

// One table per graph node, in node-id order.
namespace detail_dag {
template <class Graph>
struct tables_of;
template <class... Nodes>
struct tables_of<graph<Nodes...>> {
    using type = std::tuple<dag_pdu_table<typename Nodes::spec>...>;
};
}  // namespace detail_dag
template <class Graph>
using dag_tables = typename detail_dag::tables_of<Graph>::type;

// Decode one packet into the per-node tables by walking the graph from `root`. The visitor recovers each
// emitted node's compile-time id and appends the PDU at its offset to that node's table. Stays in lockstep
// with walk<Graph>, so the rows produced are exactly the PDUs the DAG (== walk_packet) traverses.
template <class Graph>
void dag_decode_packet(std::uint64_t packet_id, const std::uint8_t* pkt, std::size_t size,
                       dag_tables<Graph>& tabs, int root = 0) {
    walk<Graph>(root, pkt, size, [&](auto Ic, std::size_t off) {
        constexpr std::size_t I = decltype(Ic)::value;
        std::get<I>(tabs).append(packet_id, pkt + off);
    });
}

}  // namespace nanotins
