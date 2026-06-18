// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Lance writers for the two IPv6 variable-length child tables that don't fit the one-row-per-DAG-node
// shape (so they can't use dag_table_writer.hpp's spec-driven path):
//   ipv6_srh_segment : struct[ packet_id u64, srh_order u8, segment_index u8, address fixed_size_binary(16) ]
//   ipv6_option      : struct[ packet_id u64, container_type u8, opt_type u8, opt_len u8 ]
// Both are plain SoA columns (one bulk ArrowBufferAppend per column, no per-row append), matching the
// dag_pdu writer's batch shape. The 16-byte segment addresses map to Arrow fixed_size_binary(16) (the same
// kind the IPv6 src/dst columns already use), so the output is standard Arrow / Lance.

#include "soatins/arrow_glue.hpp"  // soatins::arrow_kind, nt_set_column_schema

#include "nanotins/ipv6_children.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace pdu_io {

namespace detail_ipv6 {

// Write a fully-built struct batch as a single-fragment Lance table.
inline bool write_one_batch(const std::filesystem::path& path, ArrowSchema& schema, ArrowArray& batch,
                            bool compress, std::string& error) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    NanoLanceWriter writer{};
    if (nano_lance_writer_init(&writer, path.string().c_str(), 3) != NANO_LANCE_OK) {
        error = nano_lance_writer_last_error(&writer);
        return false;
    }
    nano_lance_writer_set_ignore_nullability(&writer, true);
    nano_lance_writer_set_compression(&writer, compress);
    bool ok = nano_lance_write_batch(&writer, &batch, &schema) == NANO_LANCE_OK &&
              nano_lance_writer_commit(&writer, /*is_append=*/false) == NANO_LANCE_OK;
    if (!ok) {
        error = nano_lance_writer_last_error(&writer);
    }
    nano_lance_writer_close(&writer);
    return ok;
}

// Bulk-append a contiguous fixed-width column into child `c` (data buffer is buffer index 1).
inline bool append_fixed_column(ArrowArray* c, const void* data, std::int64_t n, std::size_t elem_size) {
    const bool ok = ArrowBufferAppend(ArrowArrayBuffer(c, 1), data,
                                      n * static_cast<std::int64_t>(elem_size)) == NANOARROW_OK;
    c->length = n;
    c->null_count = 0;
    return ok;
}

}  // namespace detail_ipv6

// ipv6_srh_segment table.
inline bool write_ipv6_srh_segment_table(const std::filesystem::path& path,
                                         const nanotins::ipv6_srh_segment_table& t, bool compress,
                                         std::string& error) {
    if (t.size() == 0) {
        return true;  // nothing to write (lazy: no empty table)
    }
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, 4) != NANOARROW_OK) {
        error = "ipv6_srh_segment schema alloc failed";
        return false;
    }
    using soatins::arrow_kind;
    bool ok = soatins::nt_set_column_schema(schema.children[0], arrow_kind::u64, 0, "packet_id", error) &&
              soatins::nt_set_column_schema(schema.children[1], arrow_kind::u8, 0, "srh_order", error) &&
              soatins::nt_set_column_schema(schema.children[2], arrow_kind::u8, 0, "segment_index", error) &&
              soatins::nt_set_column_schema(schema.children[3], arrow_kind::fixed_binary, 16, "address", error);
    if (!ok) {
        ArrowSchemaRelease(&schema);
        return false;
    }
    schema.flags = 0;

    ArrowArray batch{};
    if (ArrowArrayInitFromSchema(&batch, &schema, nullptr) != NANOARROW_OK) {
        error = "ipv6_srh_segment array init failed";
        ArrowSchemaRelease(&schema);
        return false;
    }
    const std::int64_t n = static_cast<std::int64_t>(t.size());
    ok = detail_ipv6::append_fixed_column(batch.children[0], t.packet_id.data(), n, sizeof(std::uint64_t)) &&
         detail_ipv6::append_fixed_column(batch.children[1], t.srh_order.data(), n, sizeof(std::uint8_t)) &&
         detail_ipv6::append_fixed_column(batch.children[2], t.segment_index.data(), n, sizeof(std::uint8_t)) &&
         detail_ipv6::append_fixed_column(batch.children[3], t.address.data(), n, 16);
    batch.length = n;
    batch.null_count = 0;
    if (!ok || ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
        error = error.empty() ? "ipv6_srh_segment fill failed" : error;
        ArrowArrayRelease(&batch);
        ArrowSchemaRelease(&schema);
        return false;
    }
    ok = detail_ipv6::write_one_batch(path, schema, batch, compress, error);
    ArrowArrayRelease(&batch);
    ArrowSchemaRelease(&schema);
    return ok;
}

// ipv6_option table.
inline bool write_ipv6_option_table(const std::filesystem::path& path, const nanotins::ipv6_opt_table& t,
                                    bool compress, std::string& error) {
    if (t.size() == 0) {
        return true;
    }
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, 4) != NANOARROW_OK) {
        error = "ipv6_option schema alloc failed";
        return false;
    }
    using soatins::arrow_kind;
    bool ok = soatins::nt_set_column_schema(schema.children[0], arrow_kind::u64, 0, "packet_id", error) &&
              soatins::nt_set_column_schema(schema.children[1], arrow_kind::u8, 0, "container_type", error) &&
              soatins::nt_set_column_schema(schema.children[2], arrow_kind::u8, 0, "opt_type", error) &&
              soatins::nt_set_column_schema(schema.children[3], arrow_kind::u8, 0, "opt_len", error);
    if (!ok) {
        ArrowSchemaRelease(&schema);
        return false;
    }
    schema.flags = 0;

    ArrowArray batch{};
    if (ArrowArrayInitFromSchema(&batch, &schema, nullptr) != NANOARROW_OK) {
        error = "ipv6_option array init failed";
        ArrowSchemaRelease(&schema);
        return false;
    }
    const std::int64_t n = static_cast<std::int64_t>(t.size());
    ok = detail_ipv6::append_fixed_column(batch.children[0], t.packet_id.data(), n, sizeof(std::uint64_t)) &&
         detail_ipv6::append_fixed_column(batch.children[1], t.container_type.data(), n, sizeof(std::uint8_t)) &&
         detail_ipv6::append_fixed_column(batch.children[2], t.opt_type.data(), n, sizeof(std::uint8_t)) &&
         detail_ipv6::append_fixed_column(batch.children[3], t.opt_len.data(), n, sizeof(std::uint8_t));
    batch.length = n;
    batch.null_count = 0;
    if (!ok || ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
        error = error.empty() ? "ipv6_option fill failed" : error;
        ArrowArrayRelease(&batch);
        ArrowSchemaRelease(&schema);
        return false;
    }
    ok = detail_ipv6::write_one_batch(path, schema, batch, compress, error);
    ArrowArrayRelease(&batch);
    ArrowSchemaRelease(&schema);
    return ok;
}

}  // namespace pdu_io
