// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Lance writer for the IPv4 options child table — the IPv4 analogue of ipv6_child_table_writer.hpp. IPv4
// options carry 0..N records per packet (between the 20-byte fixed header and IHL*4), so they don't fit the
// one-row-per-DAG-node shape and can't use dag_table_writer.hpp's spec-driven path:
//   ipv4_option : struct[ packet_id u64, opt_type u8, opt_len u8 ]
// Plain SoA columns (one bulk ArrowBufferAppend per column, no per-row append), matching the dag_pdu and
// IPv6-child writers' batch shape, so the output is standard Arrow / Lance.

#include "soatins/arrow_glue.hpp"  // soatins::arrow_kind, nt_set_column_schema

#include "nanotins/ipv4_children.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace pdu_io {

namespace detail_ipv4 {

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

}  // namespace detail_ipv4

// ipv4_option table.
inline bool write_ipv4_option_table(const std::filesystem::path& path, const nanotins::ipv4_opt_table& t,
                                    bool compress, std::string& error) {
    if (t.size() == 0) {
        return true;  // nothing to write (lazy: no empty table)
    }
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, 3) != NANOARROW_OK) {
        error = "ipv4_option schema alloc failed";
        return false;
    }
    using soatins::arrow_kind;
    bool ok = soatins::nt_set_column_schema(schema.children[0], arrow_kind::u64, 0, "packet_id", error) &&
              soatins::nt_set_column_schema(schema.children[1], arrow_kind::u8, 0, "opt_type", error) &&
              soatins::nt_set_column_schema(schema.children[2], arrow_kind::u8, 0, "opt_len", error);
    if (!ok) {
        ArrowSchemaRelease(&schema);
        return false;
    }
    schema.flags = 0;

    ArrowArray batch{};
    if (ArrowArrayInitFromSchema(&batch, &schema, nullptr) != NANOARROW_OK) {
        error = "ipv4_option array init failed";
        ArrowSchemaRelease(&schema);
        return false;
    }
    const std::int64_t n = static_cast<std::int64_t>(t.size());
    ok = detail_ipv4::append_fixed_column(batch.children[0], t.packet_id.data(), n, sizeof(std::uint64_t)) &&
         detail_ipv4::append_fixed_column(batch.children[1], t.opt_type.data(), n, sizeof(std::uint8_t)) &&
         detail_ipv4::append_fixed_column(batch.children[2], t.opt_len.data(), n, sizeof(std::uint8_t));
    batch.length = n;
    batch.null_count = 0;
    if (!ok || ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
        error = error.empty() ? "ipv4_option fill failed" : error;
        ArrowArrayRelease(&batch);
        ArrowSchemaRelease(&schema);
        return false;
    }
    ok = detail_ipv4::write_one_batch(path, schema, batch, compress, error);
    ArrowArrayRelease(&batch);
    ArrowSchemaRelease(&schema);
    return ok;
}

}  // namespace pdu_io
