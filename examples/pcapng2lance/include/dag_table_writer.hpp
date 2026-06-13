#pragma once

// Write one DAG node's columnar table (dag_pdu_table<Spec>) as its own Lance table: a `packet_id` column
// followed by the spec's field columns. The spec-driven sibling of pdu_table_writer.hpp — and it produces
// BYTE-IDENTICAL Arrow record batches to the protocols::PduColumn path (same column names/types/order,
// same buffers), because the spec field columns map to the same arrow_kind as the described struct's
// columns_of and carry the same host-order values (verified in test_pdu_table_interop).
//
// dag_pdu_table already stores each column contiguously (SoA), so the batch is one bulk ArrowBufferAppend
// per column (no per-row append) — the same shape as wire_spec_soa::to_arrow_spec, with packet_id
// prepended.

#include "soatins/arrow_glue.hpp"          // nt_set_column_schema
#include "nanotins/dag_decode.hpp"         // dag_pdu_table
#include "nanotins/wire_spec.hpp"
#include "nanotins/wire_spec_soa.hpp"    // spec_col (kind / fixed_width / name)

#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>
#include <utility>

namespace pdu_io {

// Schema for a DAG PDU table: struct[ packet_id u64, <spec field columns...> ].
template <class Spec>
bool build_dag_pdu_schema(ArrowSchema& schema, std::string& error) {
    using cols = nanotins::columns_of_spec<Spec>;
    constexpr std::size_t kCols = std::tuple_size_v<cols>;
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, static_cast<int64_t>(kCols + 1)) != NANOARROW_OK) {
        error = "dag pdu schema alloc failed";
        return false;
    }
    if (!soatins::nt_set_column_schema(schema.children[0], soatins::arrow_kind::u64, 0, "packet_id", error)) {
        ArrowSchemaRelease(&schema);
        return false;
    }
    bool ok = true;
    std::string err;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((ok = ok && soatins::nt_set_column_schema(schema.children[I + 1], std::tuple_element_t<I, cols>::kind,
                                                   std::tuple_element_t<I, cols>::fixed_width,
                                                   std::tuple_element_t<I, cols>::name(), err)),
         ...);
    }(std::make_index_sequence<kCols>{});
    if (!ok) {
        error = err;
        ArrowSchemaRelease(&schema);
        return false;
    }
    schema.flags = 0;
    return true;
}

// Build a record-batch array for `table` against a schema from build_dag_pdu_schema<Spec>. One bulk append
// per column (packet_id, then each spec field) — the columns are already the contiguous Arrow data layout.
template <class Spec>
bool build_dag_pdu_batch(const ArrowSchema& schema, const nanotins::dag_pdu_table<Spec>& table,
                         ArrowArray& batch, std::string& error) {
    using cols = nanotins::columns_of_spec<Spec>;
    constexpr std::size_t kCols = std::tuple_size_v<cols>;
    if (ArrowArrayInitFromSchema(&batch, const_cast<ArrowSchema*>(&schema), nullptr) != NANOARROW_OK) {
        error = "dag pdu array init failed";
        return false;
    }
    const std::int64_t n = static_cast<std::int64_t>(table.size());
    bool ok = ArrowBufferAppend(ArrowArrayBuffer(batch.children[0], 1), table.packet_id.data(),
                                n * static_cast<std::int64_t>(sizeof(std::uint64_t))) == NANOARROW_OK;
    batch.children[0]->length = n;
    batch.children[0]->null_count = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((ok = ok && ArrowBufferAppend(
                         ArrowArrayBuffer(batch.children[I + 1], 1), table.template column<I>().data(),
                         n * static_cast<std::int64_t>(sizeof(typename std::tuple_element_t<I, cols>::elem))) ==
                         NANOARROW_OK,
          batch.children[I + 1]->length = n, batch.children[I + 1]->null_count = 0),
         ...);
    }(std::make_index_sequence<kCols>{});
    if (!ok) {
        error = "dag pdu bulk fill failed";
        ArrowArrayRelease(&batch);
        return false;
    }
    batch.length = n;
    batch.null_count = 0;
    if (ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
        error = "dag pdu array finalize failed";
        ArrowArrayRelease(&batch);
        return false;
    }
    return true;
}

// One writer session per DAG PDU table; append a fragment per chunk (lazily created on first non-empty).
template <class Spec>
class DagPduAppender {
public:
    DagPduAppender(std::filesystem::path path, bool compress)
        : path_(std::move(path)), compress_(compress) {}

    bool append(const nanotins::dag_pdu_table<Spec>& table, std::string& error) {
        if (table.size() == 0) {
            return true;
        }
        if (!opened_ && !open(error)) {
            return false;
        }
        ArrowArray batch{};
        if (!build_dag_pdu_batch<Spec>(schema_, table, batch, error)) {
            return false;
        }
        bool ok = nano_lance_write_batch(&writer_, &batch, &schema_) == NANO_LANCE_OK &&
                  nano_lance_writer_commit(&writer_, /*is_append=*/committed_) == NANO_LANCE_OK;
        if (!ok) {
            error = nano_lance_writer_last_error(&writer_);
        }
        batch.release(&batch);
        committed_ = committed_ || ok;
        return ok;
    }

    void close() {
        if (opened_) {
            nano_lance_writer_close(&writer_);
            schema_.release(&schema_);
            opened_ = false;
        }
    }

private:
    bool open(std::string& error) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        if (!build_dag_pdu_schema<Spec>(schema_, error)) {
            return false;
        }
        if (nano_lance_writer_init(&writer_, path_.string().c_str(), 3) != NANO_LANCE_OK) {
            error = nano_lance_writer_last_error(&writer_);
            schema_.release(&schema_);
            return false;
        }
        nano_lance_writer_set_ignore_nullability(&writer_, true);
        nano_lance_writer_set_compression(&writer_, compress_);
        opened_ = true;
        return true;
    }

    std::filesystem::path path_;
    bool compress_;
    NanoLanceWriter writer_{};
    ArrowSchema schema_{};
    bool opened_ = false;
    bool committed_ = false;
};

// One-shot: write a single-fragment DAG PDU table (the spec sibling of write_pdu_table).
template <class Spec>
bool write_dag_pdu_table(const std::filesystem::path& path, const nanotins::dag_pdu_table<Spec>& table,
                         bool compress, std::string& error) {
    DagPduAppender<Spec> appender(path, compress);
    if (!appender.append(table, error)) {
        appender.close();
        return false;
    }
    appender.close();
    return true;
}

}  // namespace pdu_io
