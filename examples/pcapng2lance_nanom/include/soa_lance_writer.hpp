// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// A generic nanom-soa -> Lance table writer. Given any nanom `soa<Row>` (already filled), it builds the
// Arrow schema straight from nanom's own per-column Arrow C-Data format strings (`column_info::arrow`,
// e.g. "C"/"S"/"I"/"L" for u8/u16/u32/u64 and "w:4"/"w:16" for fixed-size-binary address columns) and
// copies each row's values out of nanom's contiguous per-column chunk buffers into a nanoarrow record
// batch, then writes it as one Lance fragment via nanolance. This is the "schemas for free -> Lance" path
// from nanom's README, realized: nothing about the row type is hand-coded here — the columns, names,
// types, and byte widths all come from the single NANOM_DESCRIBE on the row struct.
//
// Used by the --decode-l2l3 path to emit one Lance table per PDU type (ethernet/vlan/ipv4/ipv6/tcp/udp)
// with zero per-type writer code.

#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>
#include <string>

#include <nanom/nanom.hpp>

namespace p2l_nanom {

namespace nm = nanom;

static_assert(std::endian::native == std::endian::little,
              "soa_lance_writer copies nanom's host-order column bytes assuming little-endian native "
              "(matches Arrow's on-wire layout); a big-endian host would need a byte swap here.");

// Append one column value at `off` bytes into a nanom column buffer to the matching Arrow child.
inline bool append_cell(ArrowArray* child, nm::dkind kind, const std::byte* p, std::size_t elem_bytes) {
    switch (kind) {
        case nm::dkind::fixed_bin: {
            ArrowBufferView v{{reinterpret_cast<const uint8_t*>(p)}, static_cast<int64_t>(elem_bytes)};
            return ArrowArrayAppendBytes(child, v) == NANOARROW_OK;
        }
        case nm::dkind::f32: {
            float f = 0;
            std::memcpy(&f, p, 4);
            return ArrowArrayAppendDouble(child, f) == NANOARROW_OK;
        }
        case nm::dkind::f64: {
            double d = 0;
            std::memcpy(&d, p, 8);
            return ArrowArrayAppendDouble(child, d) == NANOARROW_OK;
        }
        case nm::dkind::i8:
        case nm::dkind::i16:
        case nm::dkind::i32:
        case nm::dkind::i64: {
            std::int64_t s = 0;
            std::memcpy(&s, p, elem_bytes);  // sign-extend from the top bit of the stored width
            const unsigned bits = static_cast<unsigned>(elem_bytes) * 8U;
            if (bits < 64 && (s & (std::int64_t{1} << (bits - 1)))) s |= -(std::int64_t{1} << bits);
            return ArrowArrayAppendInt(child, s) == NANOARROW_OK;
        }
        default: {  // u8/u16/u32/u64
            std::uint64_t u = 0;
            std::memcpy(&u, p, elem_bytes);  // zero-extend (little-endian native)
            return ArrowArrayAppendUInt(child, u) == NANOARROW_OK;
        }
    }
}

// Build the struct schema (one child per nanom column) from the soa's column_info list.
template <class Row>
bool build_soa_schema(const nm::soa<Row>& table, ArrowSchema& schema, std::string& err) {
    const auto& cols = table.columns();
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, static_cast<int64_t>(cols.size())) != NANOARROW_OK) {
        return (err = "alloc struct schema", false);
    }
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (ArrowSchemaSetFormat(schema.children[i], cols[i].arrow.c_str()) != NANOARROW_OK ||
            ArrowSchemaSetName(schema.children[i], cols[i].name.c_str()) != NANOARROW_OK) {
            return (err = "schema column: " + cols[i].name, false);
        }
    }
    schema.flags = 0;
    return true;
}

// Write one filled soa<Row> to `path` as a single-fragment Lance dataset. An empty table is skipped
// (no file), matching the nanotins example's lazy per-PDU tables.
template <class Row>
bool write_soa_table(const std::string& path, const nm::soa<Row>& table, bool compress, std::string& err) {
    if (table.rows() == 0) return true;
    const auto& cols = table.columns();

    ArrowSchema schema{};
    if (!build_soa_schema(table, schema, err)) return false;

    ArrowArray batch{};
    if (ArrowArrayInitFromSchema(&batch, &schema, nullptr) != NANOARROW_OK ||
        ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
        ArrowSchemaRelease(&schema);
        return (err = "alloc array", false);
    }

    bool ok = true;
    table.for_each_chunk([&](const auto& ch) {
        for (std::size_t r = 0; ok && r < ch.rows; ++r) {
            for (std::size_t c = 0; c < cols.size(); ++c) {
                const std::byte* base = ch.cols[c].data() + r * cols[c].elem_bytes;
                if (!append_cell(batch.children[c], cols[c].kind, base, cols[c].elem_bytes)) {
                    ok = false;
                    break;
                }
            }
            if (ok && ArrowArrayFinishElement(&batch) != NANOARROW_OK) ok = false;
        }
    });
    if (!ok) {
        batch.release(&batch);
        ArrowSchemaRelease(&schema);
        return (err = "append rows to " + path, false);
    }
    if (ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
        batch.release(&batch);
        ArrowSchemaRelease(&schema);
        return (err = "finalize array for " + path, false);
    }

    NanoLanceWriter writer{};
    if (nano_lance_writer_init(&writer, path.c_str(), 3) != NANO_LANCE_OK) {
        err = std::string("writer init: ") + nano_lance_writer_last_error(&writer);
        batch.release(&batch);
        ArrowSchemaRelease(&schema);
        return false;
    }
    // nanoarrow marks every field nullable by default; the columns carry no nulls, so tell the writer to
    // accept the nullable flag rather than rejecting it (same knob the L1 blob path uses).
    nano_lance_writer_set_ignore_nullability(&writer, true);
    nano_lance_writer_set_compression(&writer, compress);
    const int wrote = nano_lance_write_batch(&writer, &batch, &schema);
    if (wrote == NANO_LANCE_OK) {
        nano_lance_writer_commit(&writer, /*is_append=*/false);
    } else {
        err = std::string("write_batch: ") + nano_lance_writer_last_error(&writer);
    }
    nano_lance_writer_close(&writer);
    batch.release(&batch);
    ArrowSchemaRelease(&schema);
    return wrote == NANO_LANCE_OK;
}

}  // namespace p2l_nanom
