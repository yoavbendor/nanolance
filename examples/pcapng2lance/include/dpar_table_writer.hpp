// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Write a DPAR rule table (a std::vector<Row> of described rows accumulated by a palette Kind) as its own
// Lance table. The DPAR sibling of dag_table_writer.hpp's DagPduAppender<Spec>: same open -> append
// (write_batch + commit) -> close lifecycle over the nano_lance_writer C API. The difference is the source
// shape — a DPAR table is row-major (std::vector<Row>), not a contiguous SoA — so the Arrow batch is built
// with nanotins::dpar::table_to_arrow (materialize -> soatins::to_arrow) and the schema with
// nanotins::dpar::table_schema<Row>, rather than the per-column bulk append the spec tables use.

#include "nanotins/dpar_arrow.hpp"  // table_to_arrow, table_schema

#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace dpar_io {

// One writer session per rule table; appends a fragment per chunk (lazily opened on the first non-empty
// append). Mirrors pdu_io::DagPduAppender.
template <class Row>
class DparTableAppender {
public:
    DparTableAppender(std::filesystem::path path, bool compress)
        : path_(std::move(path)), compress_(compress) {}

    bool append(const std::vector<Row>& rows, std::string& error) {
        if (rows.empty()) {
            return true;
        }
        if (!opened_ && !open(error)) {
            return false;
        }
        ArrowArray batch{};
        if (!nanotins::dpar::table_to_arrow(rows, batch, error)) {
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
        if (!nanotins::dpar::table_schema<Row>(schema_, error)) {
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

// One-shot: write a single-fragment rule table to a Lance dataset (the DPAR sibling of
// pdu_io::write_dag_pdu_table).
template <class Row>
bool write_dpar_table(const std::filesystem::path& path, const std::vector<Row>& rows, bool compress,
                      std::string& error) {
    DparTableAppender<Row> appender(path, compress);
    if (!appender.append(rows, error)) {
        appender.close();
        return false;
    }
    appender.close();
    return true;
}

}  // namespace dpar_io
