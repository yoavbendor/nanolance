// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// A generic nanom-soa -> Lance table writer built on nanolance's compile-time typed facade
// (nanolance/typed_writer.hpp). The Lance schema is derived at COMPILE TIME from the row struct's
// single NANOM_DESCRIBE registration: each described field becomes one typed column -- scalars as
// their decoded host type (be<u16> -> uint16_t, ubits<4> -> uint8_t), byte arrays as Arrow
// fixed_size_binary (std::array<uint8_t, 4/6/16> for IPv4/MAC/IPv6 addresses). Column names, types,
// and Arrow formats are all checked when this header is instantiated, not when the file is written.
//
// The write itself is zero-copy: soa<Row> already stores each column as a contiguous host-order
// buffer per chunk, which is exactly the span shape writer::write_batch takes, and the writer's
// default borrow-buffers mode encodes straight out of those chunk buffers (they outlive commit()
// because the caller's soa does). No nanoarrow builder, no per-cell appends.
//
// Used by the --decode-l2l3 path to emit one Lance table per PDU type (ethernet/vlan/ipv4/ipv6/
// tcp/udp/...) with zero per-type writer code.

#include "nanolance/typed_writer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include <nanom/nanom.hpp>

namespace p2l_nanom {

namespace nm = nanom;

static_assert(std::endian::native == std::endian::little,
              "soa_lance_writer hands nanom's host-order column bytes to Lance assuming little-endian "
              "native (matches Arrow's on-wire layout); a big-endian host would need a byte swap here.");

namespace detail {

// One described field of Row, resolved at compile time: its typed-facade column value type (the
// nanom wire-decoded type -- also exactly how soa<Row> lays the column out in memory) and its name
// converted from nanom's fixed_string to the facade's NTTP fixed_string.
template <class Row, std::size_t I>
struct field_at {
    using fld = std::remove_cvref_t<decltype(std::get<I>(nm::describe<Row>::fields()))>;
    using member = nm::detail::member_t<fld::mem_ptr>;
    static_assert(!nm::Described<member>,
                  "typed soa bridge supports flat row structs only (nested described structs would "
                  "need dotted-name flattening; keep PDU rows flat)");
    using value = typename nm::detail::wire<member>::decoded;
    static constexpr auto name = nano_lance::typed::fixed_string(fld::name.data);
};

template <class Row, class Seq>
struct schema_for_impl;
template <class Row, std::size_t... I>
struct schema_for_impl<Row, std::index_sequence<I...>> {
    using type = nano_lance::typed::schema<
        nano_lance::typed::column<typename field_at<Row, I>::value, field_at<Row, I>::name>...>;
};

template <class Row>
constexpr std::size_t field_count = nm::detail::field_count_v<Row>;

/// The typed-facade schema for a described row struct, one column per registered field.
template <class Row>
using schema_for = typename schema_for_impl<Row, std::make_index_sequence<field_count<Row>>>::type;

template <class Row, std::size_t... I>
bool write_chunk(nano_lance::typed::writer<schema_for<Row>>& w, const typename nm::soa<Row>::chunk& ch,
                 std::index_sequence<I...>) {
    return w.write_batch(ch.template as<typename field_at<Row, I>::value>(I)...);
}

}  // namespace detail

// Write one filled soa<Row> to `path` as a single-fragment Lance dataset. An empty table is skipped
// (no file), matching the nanotins example's lazy per-PDU tables.
template <class Row>
bool write_soa_table(const std::string& path, const nm::soa<Row>& table, bool compress, std::string& err) {
    if (table.rows() == 0) return true;

    nano_lance::typed::writer<detail::schema_for<Row>> w(path.c_str(),
                                                         {.compression_level = 3, .compress = compress});
    if (!w.ok()) {
        err = std::string("writer init: ") + w.last_error();
        return false;
    }
    bool ok = true;
    table.for_each_chunk([&](const auto& ch) {
        ok = ok && detail::write_chunk<Row>(w, ch, std::make_index_sequence<detail::field_count<Row>>{});
    });
    ok = ok && w.commit();
    if (!ok) err = std::string("write ") + path + ": " + w.last_error();
    w.close();
    return ok;
}

}  // namespace p2l_nanom
