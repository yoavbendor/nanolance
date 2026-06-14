// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Write one decoded-PDU list as its own Lance table: a `packet_id` column (link back to the packets
// table) followed by the reflected header columns. Reuses the nanotins arrow glue, so adding a protocol
// needs no table-writing code. MAC / IPv4 / IPv6 address columns are fixed-size-binary.
//
// `PduAppender<T>` keeps one writer session open and commits a fragment per chunk (the chunked-enrich
// path); `write_pdu_table` is the one-shot convenience (one fragment) used by --decode-l2l3.

#include "soatins/arrow_glue.hpp"
#include "nanotins/protocol_decode.hpp"

#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace pdu_io {

// Schema for a PDU table: struct[ packet_id u64, <reflected header columns...> ].
template <class T>
bool build_pdu_schema(ArrowSchema& schema, std::string& error) {
    constexpr std::size_t kCols = soatins::column_count<T>;
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, static_cast<int64_t>(kCols + 1)) != NANOARROW_OK) {
        error = "pdu schema alloc failed";
        return false;
    }
    if (!soatins::nt_set_column_schema(schema.children[0], soatins::arrow_kind::u64, 0, "packet_id", error) ||
        !soatins::nt_fill_struct_schema<T>(&schema, 1, error)) {
        ArrowSchemaRelease(&schema);
        return false;
    }
    schema.flags = 0;
    return true;
}

// Build a record-batch array for `col` against a schema from build_pdu_schema<T>.
template <class T>
bool build_pdu_batch(const ArrowSchema& schema, const protocols::PduColumn<T>& col, ArrowArray& batch,
                     std::string& error) {
    soatins::soa<T> soa;
    soa.resize(col.size());
    for (std::size_t i = 0; i < col.size(); ++i) {
        soa.store(i, col.rows[i]);
    }
    if (ArrowArrayInitFromSchema(&batch, const_cast<ArrowSchema*>(&schema), nullptr) != NANOARROW_OK ||
        ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
        error = "pdu array init failed";
        return false;
    }
    for (std::size_t i = 0; i < col.size(); ++i) {
        if (ArrowArrayAppendUInt(batch.children[0], col.packet_id[i]) != NANOARROW_OK ||
            !soatins::nt_append_scalar_row<T>(&batch, 1, soa, i) ||
            ArrowArrayFinishElement(&batch) != NANOARROW_OK) {
            error = "pdu row append failed";
            batch.release(&batch);
            return false;
        }
    }
    if (ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
        error = "pdu array finalize failed";
        batch.release(&batch);
        return false;
    }
    return true;
}

// One writer session per PDU table; append a fragment per chunk. Lazily created on the first non-empty
// chunk (a PDU type absent from the whole capture yields no table).
template <class T>
class PduAppender {
public:
    PduAppender(std::filesystem::path path, bool compress) : path_(std::move(path)), compress_(compress) {}

    bool append(const protocols::PduColumn<T>& col, std::string& error) {
        if (col.size() == 0) {
            return true;
        }
        if (!opened_ && !open(error)) {
            return false;
        }
        ArrowArray batch{};
        if (!build_pdu_batch<T>(schema_, col, batch, error)) {
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
        if (!build_pdu_schema<T>(schema_, error)) {
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

// One-shot: write a single-fragment PDU table.
template <class T>
bool write_pdu_table(const std::filesystem::path& path, const protocols::PduColumn<T>& col, bool compress,
                     std::string& error) {
    PduAppender<T> appender(path, compress);
    if (!appender.append(col, error)) {
        appender.close();
        return false;
    }
    appender.close();
    return true;
}

}  // namespace pdu_io
