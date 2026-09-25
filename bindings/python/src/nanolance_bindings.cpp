// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Python bindings for nanolance: fast Arrow <-> Lance reader/writer.

#include "arrow_capsule.hpp"

#include <nanolance/nano_lance_reader.h>
#include <nanolance/nano_lance_writer.h>

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace nb = nanobind;
using nanolance_py::arrow_capsule::BatchIterator;
using nanolance_py::arrow_capsule::ExportedSchema;
using nanolance_py::arrow_capsule::ExportedStream;
using nanolance_py::arrow_capsule::ExportedTable;
using nanolance_py::arrow_capsule::detail::OwnedArray;
using nanolance_py::arrow_capsule::detail::OwnedSchema;

namespace {

void throw_lance_writer(const char* context, int code, const NanoLanceWriter* writer) {
    const char* err = writer ? nano_lance_writer_last_error(writer) : "";
    std::string msg = std::string(context) + " failed";
    if (err != nullptr && err[0] != '\0') {
        msg += ": ";
        msg += err;
    }
    (void)code;
    throw std::runtime_error(msg);
}

void throw_lance_reader(const char* context, int code, const char* err) {
    std::string msg = std::string(context) + " failed";
    if (err != nullptr && err[0] != '\0') {
        msg += ": ";
        msg += err;
    }
    (void)code;
    throw std::runtime_error(msg);
}

struct LanceWriterOptions {
    int compression_level = 3;
    bool compression = false;
    bool structural_encoding = true;
    bool blob_uri_dictionary = false;
    bool ignore_nullability = true;
    bool append = false;
};

void write_table(nb::handle table, const std::filesystem::path& path, const LanceWriterOptions& opts) {
    NanoLanceWriter writer{};
    int rc = opts.append ? nano_lance_writer_init_append(&writer, path.string().c_str(),
                                                         opts.compression_level)
                         : nano_lance_writer_init(&writer, path.string().c_str(),
                                                  opts.compression_level);
    if (rc != NANO_LANCE_OK) {
        throw_lance_writer("nano_lance_writer_init", rc, &writer);
    }

    if (opts.compression) {
        rc = nano_lance_writer_set_compression(&writer, true);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_writer_set_compression", rc, &writer);
        }
    }
    if (!opts.structural_encoding) {
        rc = nano_lance_writer_set_structural_encoding(&writer, false);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_writer_set_structural_encoding", rc, &writer);
        }
    }
    if (opts.blob_uri_dictionary) {
        rc = nano_lance_writer_set_blob_uri_dictionary(&writer, true);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_writer_set_blob_uri_dictionary", rc, &writer);
        }
    }
    if (opts.ignore_nullability) {
        rc = nano_lance_writer_set_ignore_nullability(&writer, true);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_writer_set_ignore_nullability", rc, &writer);
        }
    }

    BatchIterator it(table);
    OwnedSchema schema;
    OwnedArray array;
    bool any = false;
    while (it.next(&schema.schema, &array.array)) {
        rc = nano_lance_write_batch(&writer, &array.array, &schema.schema);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_write_batch", rc, &writer);
        }
        any = true;
        array.reset();
    }
    if (!any) {
        nano_lance_writer_close(&writer);
        throw std::runtime_error("cannot write empty table with no record batches");
    }

    rc = nano_lance_writer_commit(&writer, opts.append);
    if (rc != NANO_LANCE_OK) {
        throw_lance_writer("nano_lance_writer_commit", rc, &writer);
    }
    rc = nano_lance_writer_close(&writer);
    if (rc != NANO_LANCE_OK) {
        throw_lance_writer("nano_lance_writer_close", rc, &writer);
    }
}

// Streaming, context-managed writer: feed one RecordBatch at a time and only hold a single chunk in
// Python memory. Each committed fragment is written to disk immediately, so with `max_rows_per_fragment`
// set the writer's own buffered column data stays bounded too (nanolance buffers batches until a commit;
// a commit flushes them to a fragment and frees the buffer). A fragment is the Lance analogue of a
// Parquet row group; close() commits any pending rows and finalizes the dataset.
class LanceWriter {
public:
    LanceWriter(const std::filesystem::path& path, const LanceWriterOptions& opts,
                std::int64_t max_rows_per_fragment, std::uint64_t max_pending_bytes)
        : max_rows_per_fragment_(max_rows_per_fragment), append_mode_(opts.append) {
        int rc = opts.append
                     ? nano_lance_writer_init_append(&writer_, path.string().c_str(), opts.compression_level)
                     : nano_lance_writer_init(&writer_, path.string().c_str(), opts.compression_level);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_writer_init", rc, &writer_);
        }
        initialized_ = true;
        if (opts.compression) {
            rc = nano_lance_writer_set_compression(&writer_, true);
            if (rc != NANO_LANCE_OK) {
                throw_lance_writer("nano_lance_writer_set_compression", rc, &writer_);
            }
        }
        if (!opts.structural_encoding) {
            rc = nano_lance_writer_set_structural_encoding(&writer_, false);
            if (rc != NANO_LANCE_OK) {
                throw_lance_writer("nano_lance_writer_set_structural_encoding", rc, &writer_);
            }
        }
        if (opts.blob_uri_dictionary) {
            rc = nano_lance_writer_set_blob_uri_dictionary(&writer_, true);
            if (rc != NANO_LANCE_OK) {
                throw_lance_writer("nano_lance_writer_set_blob_uri_dictionary", rc, &writer_);
            }
        }
        if (opts.ignore_nullability) {
            rc = nano_lance_writer_set_ignore_nullability(&writer_, true);
            if (rc != NANO_LANCE_OK) {
                throw_lance_writer("nano_lance_writer_set_ignore_nullability", rc, &writer_);
            }
        }
        if (max_pending_bytes != 0U) {
            rc = nano_lance_writer_set_max_pending_bytes(&writer_, max_pending_bytes);
            if (rc != NANO_LANCE_OK) {
                throw_lance_writer("nano_lance_writer_set_max_pending_bytes", rc, &writer_);
            }
        }
        // In append mode the dataset already exists, so every commit is an append.
        committed_ = opts.append;
    }

    ~LanceWriter() {
        if (initialized_ && !closed_) {
            nano_lance_writer_close(&writer_);
        }
    }

    LanceWriter(const LanceWriter&) = delete;
    LanceWriter& operator=(const LanceWriter&) = delete;

    void write_batch(nb::handle batch) {
        if (closed_) {
            throw std::runtime_error("write_batch on a closed LanceWriter");
        }
        auto imported = nanolance_py::arrow_capsule::import_batch(batch);
        const std::int64_t rows = imported.second->length;
        int rc = nano_lance_write_batch(&writer_, imported.second.get(), imported.first.get());
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_write_batch", rc, &writer_);
        }
        // The C writer may have flushed a fragment itself (max_pending_bytes): take its count.
        const auto pending = static_cast<std::int64_t>(nano_lance_writer_pending_rows(&writer_));
        if (pending < pending_rows_ + rows) {
            committed_ = true;
        }
        pending_rows_ = pending;
        if (max_rows_per_fragment_ > 0 && pending_rows_ >= max_rows_per_fragment_) {
            flush();
        }
    }

    // Commit any buffered rows as a fragment (a no-op when nothing is pending). Lets callers force a
    // fragment boundary; called automatically when max_rows_per_fragment is reached and on close().
    void flush() {
        if (closed_) {
            throw std::runtime_error("flush on a closed LanceWriter");
        }
        if (pending_rows_ == 0) {
            return;
        }
        int rc = nano_lance_writer_commit(&writer_, /*is_append=*/committed_);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_writer_commit", rc, &writer_);
        }
        committed_ = true;
        pending_rows_ = 0;
    }

    void close() {
        if (closed_) {
            return;
        }
        flush();
        if (!committed_) {
            // Create-mode writer that never received a batch — no manifest was written.
            closed_ = true;
            nano_lance_writer_close(&writer_);
            throw std::runtime_error("cannot close LanceWriter with no batches written");
        }
        closed_ = true;
        int rc = nano_lance_writer_close(&writer_);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_writer_close", rc, &writer_);
        }
    }

    LanceWriter* enter() { return this; }

    bool exit(nb::handle exc_type, nb::handle /*exc*/, nb::handle /*tb*/) {
        if (closed_) {
            return false;
        }
        if (!exc_type.is_none()) {
            // An exception is propagating: release the writer without committing partial data and
            // without masking the original error.
            closed_ = true;
            nano_lance_writer_close(&writer_);
            return false;
        }
        close();
        return false;
    }

private:
    NanoLanceWriter writer_{};
    std::int64_t max_rows_per_fragment_ = 0;
    std::int64_t pending_rows_ = 0;
    bool append_mode_ = false;
    bool committed_ = false;
    bool initialized_ = false;
    bool closed_ = false;
};

/// The streaming reader: one batch decoded per consumer pull, instead of every batch up front.
/// `length < 0` means "to the end of the dataset".
ExportedStream read_table_stream(const std::filesystem::path& path,
                                 std::optional<std::vector<std::string>> columns,
                                 std::uint64_t offset, std::int64_t length) {
    std::vector<const char*> names;
    if (columns) {
        names.reserve(columns->size());
        for (const auto& name : *columns) {
            names.push_back(name.c_str());
        }
    }
    ArrowArrayStream stream{};
    char err[512] = {};
    const int rc = nano_lance_table_open_stream_range(
        path.string().c_str(), names.empty() ? nullptr : names.data(), names.size(), offset, length,
        /*trusted_input=*/0, &stream, err, sizeof(err));
    if (rc != NANO_LANCE_READER_OK) {
        throw_lance_reader("nano_lance_table_open_stream_range", rc, err);
    }
    return ExportedStream::adopt(std::move(stream));
}

/// Schema-only peek: reads the manifest, never a data file.
ExportedSchema read_schema(const std::filesystem::path& path) {
    ArrowSchema schema{};
    char err[512] = {};
    const int rc =
        nano_lance_table_read_schema(path.string().c_str(), &schema, err, sizeof(err));
    if (rc != NANO_LANCE_READER_OK) {
        throw_lance_reader("nano_lance_table_read_schema", rc, err);
    }
    return ExportedSchema::adopt(std::move(schema));
}

/// Row count from the manifest's fragments: O(fragments), not O(rows).
std::uint64_t count_rows(const std::filesystem::path& path) {
    std::uint64_t rows = 0;
    char err[512] = {};
    const int rc = nano_lance_table_count_rows(path.string().c_str(), &rows, err, sizeof(err));
    if (rc != NANO_LANCE_READER_OK) {
        throw_lance_reader("nano_lance_table_count_rows", rc, err);
    }
    return rows;
}

ExportedTable read_table_eager(const std::filesystem::path& path,
                               std::optional<std::vector<std::string>> columns, std::uint64_t offset,
                               std::int64_t length) {
    ArrowSchema schema{};
    ArrowArray* batches = nullptr;
    std::size_t batch_count = 0;
    char err[512] = {};

    // Hold the char* views alongside the strings: the C entry point copies them, but it reads them
    // first, so the std::strings must outlive the call.
    std::vector<const char*> names;
    if (columns) {
        names.reserve(columns->size());
        for (const auto& name : *columns) {
            names.push_back(name.c_str());
        }
    }
    const int rc = nano_lance_table_read_dataset_range(
        path.string().c_str(), names.empty() ? nullptr : names.data(), names.size(), offset, length,
        /*trusted_input=*/0, &schema, &batches, &batch_count, err, sizeof(err));
    if (rc != NANO_LANCE_READER_OK) {
        throw_lance_reader("nano_lance_table_read_dataset_range", rc, err);
    }
    ExportedTable out = ExportedTable::from_read_result(&schema, batches, batch_count);
    nano_lance_table_read_result_free(&schema, batches, batch_count);
    return out;
}

ExportedTable take_rows(const std::filesystem::path& path, const std::vector<std::uint64_t>& indices,
                        std::optional<std::vector<std::string>> columns) {
    ArrowSchema schema{};
    ArrowArray* batches = nullptr;
    std::size_t batch_count = 0;
    char err[512] = {};
    std::vector<const char*> names;
    if (columns) {
        names.reserve(columns->size());
        for (const auto& name : *columns) {
            names.push_back(name.c_str());
        }
    }
    const int rc = nano_lance_table_take(path.string().c_str(), names.empty() ? nullptr : names.data(), names.size(),
                                         indices.data(), indices.size(), /*trusted_input=*/0, &schema, &batches,
                                         &batch_count, err, sizeof(err));
    if (rc != NANO_LANCE_READER_OK) {
        throw_lance_reader("nano_lance_table_take", rc, err);
    }
    ExportedTable out = ExportedTable::from_read_result(&schema, batches, batch_count);
    nano_lance_table_read_result_free(&schema, batches, batch_count);
    return out;
}

}  // namespace

NB_MODULE(_nanolance, m) {
    m.doc() = "nanolance: fast zero-copy Arrow <-> Lance reader/writer";

    nb::class_<LanceWriterOptions>(m, "WriteOptions")
        .def(nb::init<>())
        .def_rw("compression_level", &LanceWriterOptions::compression_level)
        .def_rw("compression", &LanceWriterOptions::compression)
        .def_rw("structural_encoding", &LanceWriterOptions::structural_encoding)
        .def_rw("blob_uri_dictionary", &LanceWriterOptions::blob_uri_dictionary)
        .def_rw("ignore_nullability", &LanceWriterOptions::ignore_nullability)
        .def_rw("append", &LanceWriterOptions::append);

    m.def(
        "write_table",
        [](nb::handle table, const std::filesystem::path& path, const LanceWriterOptions& opts) {
            write_table(table, path, opts);
        },
        nb::arg("table"), nb::arg("path"), nb::arg("options") = LanceWriterOptions{},
        "Write an Arrow table to a Lance dataset directory.");

    nb::class_<LanceWriter>(m, "LanceWriter")
        .def(nb::init<const std::filesystem::path&, const LanceWriterOptions&, std::int64_t, std::uint64_t>(),
             nb::arg("path"), nb::arg("options") = LanceWriterOptions{},
             nb::arg("max_rows_per_fragment") = 0, nb::arg("max_pending_bytes") = 0,
             "Streaming Lance writer. Feed record batches with write_batch(); close() commits and "
             "finalizes. Use as a context manager. max_rows_per_fragment>0 flushes a fragment once that "
             "many rows are buffered; max_pending_bytes>0 once the buffered data reaches that many "
             "bytes -- either bounds memory for very large writes.")
        .def("write_batch", &LanceWriter::write_batch, nb::arg("batch"),
             "Append one Arrow RecordBatch (imported via the Arrow C array PyCapsule).")
        .def("flush", &LanceWriter::flush, "Commit buffered rows as a fragment (forces a boundary).")
        .def("close", &LanceWriter::close, "Commit pending rows and finalize the dataset.")
        .def("__enter__", &LanceWriter::enter, nb::rv_policy::reference_internal)
        .def("__exit__", &LanceWriter::exit, nb::arg("exc_type").none(), nb::arg("exc_value").none(),
             nb::arg("traceback").none());

    nb::class_<ExportedTable>(m, "LanceTable")
        .def("__arrow_c_stream__", &ExportedTable::arrow_c_stream, nb::arg("requested_schema") = nb::none(),
             "Arrow PyCapsule stream export (zero-copy into pyarrow/polars).")
        .def("__arrow_c_array_stream__", &ExportedTable::arrow_c_stream,
             nb::arg("requested_schema") = nb::none());

    nb::class_<ExportedStream>(m, "LanceStream")
        .def("__arrow_c_stream__", &ExportedStream::arrow_c_stream, nb::arg("requested_schema") = nb::none(),
             "Arrow PyCapsule stream export. Single-shot: a stream is consumed, not copied.")
        .def("__arrow_c_array_stream__", &ExportedStream::arrow_c_stream,
             nb::arg("requested_schema") = nb::none());

    nb::class_<ExportedSchema>(m, "LanceSchema")
        .def("__arrow_c_schema__", &ExportedSchema::arrow_c_schema,
             "Arrow PyCapsule schema export. Re-exportable: each call hands out a fresh copy.");

    m.def("read_schema", &read_schema, nb::arg("path"),
          "The dataset's Arrow schema, from the manifest alone -- no data file is opened.");

    m.def("count_rows", &count_rows, nb::arg("path"),
          "The dataset's row count, from the manifest's fragments. O(fragments), not O(rows).");

    m.def("read_table", &read_table_eager, nb::arg("path"), nb::arg("columns") = nb::none(),
          nb::arg("offset") = 0, nb::arg("length") = -1,
          "Read a Lance dataset, decoding every batch up front. `columns` names the top-level columns "
          "to read; the rest are skipped during decode rather than decoded and discarded.");
    m.def("take", &take_rows, nb::arg("path"), nb::arg("indices"), nb::arg("columns") = nb::none(),
          "Read the rows at `indices` (ascending, distinct), reading only the fragments and pages -- for "
          "large values only the rows -- that hold them.");
    m.def("open_stream", &read_table_stream, nb::arg("path"), nb::arg("columns") = nb::none(),
          nb::arg("offset") = 0, nb::arg("length") = -1,
          "Open a Lance dataset as a streaming Arrow handle: one batch decoded per pull, so peak "
          "memory tracks one fragment rather than the dataset.");
}
