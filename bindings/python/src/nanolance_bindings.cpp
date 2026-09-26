// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Python bindings for nanolance: fast Arrow <-> Lance reader/writer.

#include "arrow_capsule.hpp"

#include <nanolance/blob_v2_external.hpp>
#include <nanolance/dataset.hpp>
#include <nanolance/dataset_ops.hpp>
#include <nanolance/lance_table_reader.hpp>
#include <nanolance/nano_lance_reader.h>
#include <nanolance/nano_lance_writer.h>

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
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


// ── Dataset-level API, for the pylance-compatible module (nanolance.lance) ─────────────────────────

[[noreturn]] void throw_dataset(const std::string& error) {
    throw std::runtime_error(error);
}

nano_lance::LanceScanRequest make_request(std::optional<std::uint64_t> version,
                                          const std::optional<std::vector<std::string>>& columns,
                                          const std::optional<std::vector<std::uint64_t>>& fragment_ids,
                                          bool with_row_id, bool with_row_address) {
    nano_lance::LanceScanRequest request;
    request.has_version = version.has_value();
    request.version = version.value_or(0U);
    request.columns = columns ? &*columns : nullptr;
    request.fragment_ids = fragment_ids ? &*fragment_ids : nullptr;
    request.with_row_id = with_row_id;
    request.with_row_address = with_row_address;
    return request;
}

ExportedTable table_of(ArrowSchema& schema, std::vector<ArrowArray>& batches) {
    ExportedTable out = ExportedTable::from_read_result(&schema, batches.data(), batches.size());
    return out;
}

nb::object ds_scan(const std::filesystem::path& path, std::optional<std::uint64_t> version,
                   std::optional<std::vector<std::string>> columns,
                   std::optional<std::vector<std::uint64_t>> fragment_ids, std::uint64_t offset, std::int64_t length,
                   bool with_row_id, bool with_row_address, bool stream, std::optional<std::string> filter,
                   int blob_handling) {
    auto request = make_request(version, columns, fragment_ids, with_row_id, with_row_address);
    request.blob_handling = static_cast<nano_lance::BlobHandling>(blob_handling);
    request.filter = filter ? &*filter : nullptr;
    request.range.offset = offset;
    request.range.length = length < 0 ? nano_lance::LanceRowRange::kAllRows : static_cast<std::uint64_t>(length);
    std::string error;
    ArrowSchema schema{};
    if (stream) {
        nano_lance::LanceTableStream reader;
        bool ok = false;
        {
            nb::gil_scoped_release release;
            ok = nano_lance::LanceTableStream::open_request(path, request, schema, reader, error);
        }
        if (!ok) {
            throw_dataset(error);
        }
        ArrowArrayStream out{};
        nano_lance::lance_table_stream_export(std::move(reader), std::move(schema), out);
        return nb::cast(ExportedStream::adopt(std::move(out)));
    }
    std::vector<ArrowArray> batches;
    bool ok = false;
    {
        nb::gil_scoped_release release;
        ok = nano_lance::lance_dataset_scan(path, request, schema, batches, error);
    }
    if (!ok) {
        throw_dataset(error);
    }
    return nb::cast(table_of(schema, batches));
}

ExportedTable ds_take(const std::filesystem::path& path, std::optional<std::uint64_t> version,
                      const std::vector<std::uint64_t>& rows, std::optional<std::vector<std::string>> columns,
                      bool with_row_id, bool with_row_address, bool addresses, int blob_handling) {
    auto request = make_request(version, columns, std::nullopt, with_row_id, with_row_address);
    request.blob_handling = static_cast<nano_lance::BlobHandling>(blob_handling);
    std::string error;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    bool ok = false;
    {
        nb::gil_scoped_release release;
        ok = addresses ? nano_lance::lance_dataset_take_rows(path, request, rows, schema, batches, error)
                       : nano_lance::lance_dataset_take(path, request, rows, schema, batches, error);
    }
    if (!ok) {
        throw_dataset(error);
    }
    return table_of(schema, batches);
}

ExportedSchema ds_schema(const std::filesystem::path& path, std::optional<std::uint64_t> version) {
    const auto request = make_request(version, std::nullopt, std::nullopt, false, false);
    std::string error;
    ArrowSchema schema{};
    if (!nano_lance::lance_dataset_schema(path, request, schema, error)) {
        throw_dataset(error);
    }
    return ExportedSchema::adopt(std::move(schema));
}

nb::dict version_dict(const nano_lance::DatasetVersionInfo& v) {
    nb::dict d;
    d["version"] = v.version;
    d["timestamp_ns"] = v.timestamp_ns;
    d["tag"] = v.tag;
    d["writer_library"] = v.writer_library;
    d["writer_version"] = v.writer_version;
    return d;
}

nb::dict ds_info(const std::filesystem::path& path, std::optional<std::uint64_t> version) {
    nano_lance::DatasetInfo info;
    std::string error;
    if (!nano_lance::dataset_info(path, version.has_value(), version.value_or(0U), info, error)) {
        throw_dataset(error);
    }
    nb::dict d = version_dict(info.version);
    d["latest_version"] = info.latest_version;
    d["data_storage_version"] = info.data_storage_version;
    d["max_fragment_id"] = info.has_max_fragment_id ? nb::cast(info.max_fragment_id) : nb::none();
    d["config"] = info.config;
    d["table_metadata"] = info.table_metadata;
    nb::dict schema_metadata;
    for (const auto& kv : info.schema_metadata) {
        schema_metadata[nb::bytes(kv.first.data(), kv.first.size())] = nb::bytes(kv.second.data(), kv.second.size());
    }
    d["schema_metadata"] = schema_metadata;
    d["reader_feature_flags"] = info.reader_feature_flags;
    d["writer_feature_flags"] = info.writer_feature_flags;
    nb::list fragments;
    for (const auto& f : info.fragments) {
        nb::dict fd;
        fd["id"] = f.id;
        fd["physical_rows"] = f.physical_rows;
        fd["deleted_rows"] = f.deleted_rows;
        fd["deletion_file"] = f.has_deletion_file ? nb::cast(f.deletion_file) : nb::none();
        nb::list files;
        for (const auto& file : f.files) {
            nb::dict fi;
            fi["path"] = file.path;
            fi["fields"] = file.fields;
            fi["major_version"] = file.major_version;
            fi["minor_version"] = file.minor_version;
            fi["size_bytes"] = file.size_bytes;
            files.append(fi);
        }
        fd["files"] = files;
        fragments.append(fd);
    }
    d["fragments"] = fragments;
    return d;
}

nb::list ds_versions(const std::filesystem::path& path) {
    std::vector<nano_lance::DatasetVersionInfo> versions;
    std::string error;
    if (!nano_lance::dataset_versions(path, versions, error)) {
        throw_dataset(error);
    }
    nb::list out;
    for (const auto& v : versions) {
        out.append(version_dict(v));
    }
    return out;
}

template <typename F>
std::uint64_t new_version_or_throw(F&& f) {
    std::uint64_t version = 0;
    std::string error;
    if (!f(version, error)) {
        throw_dataset(error);
    }
    return version;
}

ExportedTable file_read(const std::filesystem::path& path, std::optional<std::vector<std::string>> columns,
                        std::uint64_t offset, std::int64_t length) {
    nano_lance::LanceScanRequest request;
    request.columns = columns ? &*columns : nullptr;
    request.range.offset = offset;
    request.range.length = length < 0 ? nano_lance::LanceRowRange::kAllRows : static_cast<std::uint64_t>(length);
    std::string error;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    bool ok = false;
    {
        nb::gil_scoped_release release;
        ok = nano_lance::lance_file_read(path, request, schema, batches, error);
    }
    if (!ok) {
        throw_dataset(error);
    }
    return table_of(schema, batches);
}

ExportedTable file_take(const std::filesystem::path& path, const std::vector<std::uint64_t>& rows,
                        std::optional<std::vector<std::string>> columns) {
    nano_lance::LanceScanRequest request;
    request.columns = columns ? &*columns : nullptr;
    std::string error;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    bool ok = false;
    {
        nb::gil_scoped_release release;
        ok = nano_lance::lance_file_take(path, request, rows, schema, batches, error);
    }
    if (!ok) {
        throw_dataset(error);
    }
    return table_of(schema, batches);
}

nb::tuple file_info(const std::filesystem::path& path) {
    nano_lance::LanceFileInfo info;
    ArrowSchema schema{};
    std::string error;
    if (!nano_lance::lance_file_info(path, info, schema, error)) {
        throw_dataset(error);
    }
    nb::list columns;
    for (const auto& pages : info.pages) {
        nb::list column;
        for (const auto& page : pages) {
            nb::list buffers;
            for (const auto& [offset, size] : page.buffers) {
                buffers.append(nb::make_tuple(offset, size));
            }
            column.append(nb::make_tuple(page.rows, buffers, page.encoding));
        }
        columns.append(column);
    }
    return nb::make_tuple(info.num_rows, info.num_columns, ExportedSchema::adopt(std::move(schema)), columns);
}

/// A writer that stages fragments and publishes them as one version at finish() -- a Lance write.
class StagedWriter {
public:
    StagedWriter(const std::filesystem::path& path, const LanceWriterOptions& opts, bool append,
                 std::int64_t max_rows_per_file, std::uint64_t max_bytes_per_file)
        : max_rows_per_file_(max_rows_per_file) {
        NanoLanceWriteOptions options{};
        options.compression_level = opts.compression_level;
        options.append = append;
        options.compression = opts.compression;
        options.disable_structural_encoding = !opts.structural_encoding;
        options.max_pending_bytes = max_bytes_per_file;
        options.stage_fragments = true;
        const int rc = nano_lance_writer_open(&writer_, path.string().c_str(), &options);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("open", rc, &writer_);
        }
        open_ = true;
        nano_lance_writer_set_ignore_nullability(&writer_, true);
    }
    ~StagedWriter() {
        if (open_) {
            nano_lance_writer_close(&writer_);
        }
    }
    StagedWriter(const StagedWriter&) = delete;
    StagedWriter& operator=(const StagedWriter&) = delete;

    void write_batch(nb::handle batch) {
        auto imported = nanolance_py::arrow_capsule::import_batch(batch);
        const std::int64_t rows = imported.second->length;
        int rc = NANO_LANCE_OK;
        // The GIL is released while the batch is encoded, so Python threads sharing one writer (as
        // pylance's writers allow) would otherwise enter it at once.
        nb::gil_scoped_release release;
        std::lock_guard<std::mutex> lock(mutex_);
        {
            rc = nano_lance_write_batch(&writer_, imported.second.get(), imported.first.get());
        }
        if (rc != NANO_LANCE_OK) {
            nb::gil_scoped_acquire acquire;
            throw_lance_writer("write", rc, &writer_);
        }
        pending_ += rows;
        if (max_rows_per_file_ > 0 && pending_ >= max_rows_per_file_) {
            rc = nano_lance_writer_commit(&writer_, false);
            if (rc != NANO_LANCE_OK) {
                nb::gil_scoped_acquire acquire;
                throw_lance_writer("write", rc, &writer_);
            }
            pending_ = 0;
        }
    }

    std::uint64_t finish(int mode, bool keep_empty) {
        std::uint64_t version = 0;
        int rc = NANO_LANCE_OK;
        {
            nb::gil_scoped_release release;
            std::lock_guard<std::mutex> lock(mutex_);
            if (keep_empty && pending_ == 0) {
                rc = nano_lance_writer_commit(&writer_, false);
            }
            if (rc == NANO_LANCE_OK) {
                rc = nano_lance_writer_finish(&writer_, mode, &version);
            }
        }
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("commit", rc, &writer_);
        }
        nano_lance_writer_close(&writer_);
        open_ = false;
        return version;
    }

private:
    NanoLanceWriter writer_{};
    std::int64_t max_rows_per_file_ = 0;
    std::int64_t pending_ = 0;
    bool open_ = false;
    std::mutex mutex_;
};


// ── dataset changes ─────────────────────────────────────────────────────────────────────────────

/// The ArrowArrayStream of a Python object exporting __arrow_c_stream__ (moved out of its capsule).
ArrowArrayStream stream_of(nb::handle obj) {
    if (!nb::hasattr(obj, "__arrow_c_stream__")) {
        throw std::runtime_error("expected an object exporting __arrow_c_stream__");
    }
    nb::capsule cap = nb::cast<nb::capsule>(obj.attr("__arrow_c_stream__")());
    // Move the stream out and leave the capsule a released one: its destructor still frees the
    // producer's allocation.
    auto* src = static_cast<ArrowArrayStream*>(PyCapsule_GetPointer(cap.ptr(), "arrow_array_stream"));
    if (src == nullptr) {
        throw std::runtime_error("not an arrow_array_stream capsule");
    }
    ArrowArrayStream out = *src;
    src->release = nullptr;
    return out;
}

struct SchemaHolder {
    ArrowSchema schema{};
    ~SchemaHolder() {
        if (schema.release != nullptr) {
            schema.release(&schema);
        }
    }
};

void schema_of(nb::handle obj, SchemaHolder& out) {
    if (!nb::hasattr(obj, "__arrow_c_schema__")) {
        throw std::runtime_error("expected an object exporting __arrow_c_schema__");
    }
    nb::capsule cap = nb::cast<nb::capsule>(obj.attr("__arrow_c_schema__")());
    auto* src = static_cast<ArrowSchema*>(PyCapsule_GetPointer(cap.ptr(), "arrow_schema"));
    if (src == nullptr) {
        throw std::runtime_error("not an arrow_schema capsule");
    }
    out.schema = *src;
    src->release = nullptr;
}

template <typename F>
void run_op(F&& f) {
    std::string error;
    bool ok = false;
    {
        nb::gil_scoped_release release;
        ok = f(error);
    }
    if (!ok) {
        throw_dataset(error);
    }
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
    m.def("set_threads", [](std::size_t n) { nano_lance_set_threads(n); }, nb::arg("threads"),
          "Threads nanolance may use (1: all work on the calling thread; 0: the default).");
    m.def("get_threads", [] { return nano_lance_threads(); },
          "Threads nanolance may use: set_threads, else NANOLANCE_THREADS, else the CPUs available.");
    m.def("_work_stats", [] {
        NanoLanceWorkStats w{};
        nano_lance_work_stats(&w);
        nb::dict d;
        d["data_bytes_read"] = w.data_bytes_read;
        d["data_reads"] = w.data_reads;
        d["largest_read"] = w.largest_read;
        d["page_windows"] = w.page_windows;
        d["read_morsels"] = w.read_morsels;
        d["fragment_reads"] = w.fragment_reads;
        d["parallel_column_writes"] = w.parallel_column_writes;
        d["write_buffered_bytes"] = w.write_buffered_bytes;
        d["buffer_pool_hits"] = w.buffer_pool_hits;
        d["buffer_pool_misses"] = w.buffer_pool_misses;
        d["take_cache_hits"] = w.take_cache_hits;
        return d;
    }, "Counters of the work done since the last reset (tests and diagnostics; not a stable API).");
    m.def("_reset_work_stats", [] { nano_lance_reset_work_stats(); });
    m.def("_ds_scan", &ds_scan, nb::arg("path"), nb::arg("version").none(), nb::arg("columns").none(),
          nb::arg("fragment_ids").none(), nb::arg("offset"), nb::arg("length"), nb::arg("with_row_id"),
          nb::arg("with_row_address"), nb::arg("stream"), nb::arg("filter").none() = nb::none(),
          nb::arg("blob_handling") = 0);
    m.def("_ds_take", &ds_take, nb::arg("path"), nb::arg("version").none(), nb::arg("rows"),
          nb::arg("columns").none(), nb::arg("with_row_id"), nb::arg("with_row_address"), nb::arg("addresses"),
          nb::arg("blob_handling") = 0);
    m.def("_blob_read", [](const std::string& file, bool external, std::uint64_t position, std::uint64_t size,
                           std::uint64_t offset, std::uint64_t length) {
        nano_lance::BlobV2Location location;
        location.file = file;
        location.external = external;
        location.position = position;
        location.size = size;
        std::vector<std::uint8_t> bytes;
        std::string error;
        bool ok = false;
        {
            nb::gil_scoped_release release;
            ok = nano_lance::blob_v2_read(location, offset, length, bytes, error);
        }
        if (!ok) {
            throw_dataset(error);
        }
        return nb::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }, nb::arg("file"), nb::arg("external"), nb::arg("position"), nb::arg("size"), nb::arg("offset"),
       nb::arg("length"));
    // BlobHandling, in the order of the C++ enum.
    m.attr("BLOB_INGEST") = 0;
    m.attr("BLOB_DESCRIPTIONS") = 1;
    m.attr("BLOB_BINARY") = 2;
    m.attr("BLOB_LOCATIONS") = 3;
    m.def("_ds_schema", &ds_schema, nb::arg("path"), nb::arg("version").none());
    m.def("_ds_info", &ds_info, nb::arg("path"), nb::arg("version").none());
    m.def("_ds_versions", &ds_versions, nb::arg("path"));
    m.def("_ds_latest_version", [](const std::filesystem::path& path) {
        std::uint64_t v = 0;
        std::string error;
        if (!nano_lance::dataset_latest_version(path, v, error)) {
            throw_dataset(error);
        }
        return v;
    });
    m.def("_ds_restore", [](const std::filesystem::path& path, std::uint64_t version) {
        return new_version_or_throw([&](std::uint64_t& v, std::string& e) {
            return nano_lance::dataset_restore(path, version, v, e);
        });
    });
    m.def("_ds_update_config", [](const std::filesystem::path& path, const std::map<std::string, std::string>& upsert,
                                  const std::vector<std::string>& remove) {
        return new_version_or_throw([&](std::uint64_t& v, std::string& e) {
            return nano_lance::dataset_update_config(path, upsert, remove, v, e);
        });
    });
    m.def("_ds_update_table_metadata", [](const std::filesystem::path& path,
                                          const std::map<std::string, std::string>& values, bool replace) {
        return new_version_or_throw([&](std::uint64_t& v, std::string& e) {
            return nano_lance::dataset_update_table_metadata(path, values, replace, v, e);
        });
    });
    m.def("_ds_update_schema_metadata", [](const std::filesystem::path& path,
                                           const std::map<std::string, std::string>& values, bool replace) {
        return new_version_or_throw([&](std::uint64_t& v, std::string& e) {
            return nano_lance::dataset_update_schema_metadata(path, values, replace, v, e);
        });
    });
    m.def("_file_read", &file_read, nb::arg("path"), nb::arg("columns").none(), nb::arg("offset"),
          nb::arg("length"));
    m.def("_file_take", &file_take, nb::arg("path"), nb::arg("rows"), nb::arg("columns").none());
    m.def("_file_info", &file_info, nb::arg("path"));
    nb::class_<StagedWriter>(m, "_StagedWriter")
        .def(nb::init<const std::filesystem::path&, const LanceWriterOptions&, bool, std::int64_t, std::uint64_t>(),
             nb::arg("path"), nb::arg("options"), nb::arg("append"), nb::arg("max_rows_per_file"),
             nb::arg("max_bytes_per_file"))
        .def("write_batch", &StagedWriter::write_batch)
        .def("finish", &StagedWriter::finish, nb::arg("mode"), nb::arg("keep_empty") = false);
    m.attr("COMMIT_CREATE") = static_cast<int>(NANO_LANCE_COMMIT_CREATE);
    m.attr("COMMIT_APPEND") = static_cast<int>(NANO_LANCE_COMMIT_APPEND);
    m.attr("COMMIT_OVERWRITE") = static_cast<int>(NANO_LANCE_COMMIT_OVERWRITE);
    m.def("_ds_delete", [](const std::filesystem::path& path, const std::string& predicate) {
        std::uint64_t deleted = 0;
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_delete(path, predicate, deleted, version, e); });
        return nb::make_tuple(deleted, version);
    });
    m.def("_ds_update", [](const std::filesystem::path& path, std::optional<std::string> where,
                           const std::vector<std::pair<std::string, std::string>>& assignments) {
        std::uint64_t updated = 0;
        std::uint64_t version = 0;
        run_op([&](std::string& e) {
            return nano_lance::dataset_update(path, where ? &*where : nullptr, assignments, updated, version, e);
        });
        return nb::make_tuple(updated, version);
    }, nb::arg("path"), nb::arg("where").none(), nb::arg("assignments"));
    m.def("_ds_merge_insert", [](const std::filesystem::path& path, const std::vector<std::string>& on,
                                 bool update_all, bool insert_all, bool delete_by_source,
                                 const std::string& delete_condition, nb::handle data) {
        nano_lance::MergeInsertSpec spec;
        spec.on = on;
        spec.when_matched = update_all ? nano_lance::MergeInsertSpec::WhenMatched::UpdateAll
                                       : nano_lance::MergeInsertSpec::WhenMatched::DoNothing;
        spec.when_not_matched_insert_all = insert_all;
        spec.when_not_matched_by_source_delete = delete_by_source;
        spec.when_not_matched_by_source_condition = delete_condition;
        ArrowArrayStream stream = stream_of(data);
        nano_lance::MergeInsertStats stats;
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_merge_insert(path, spec, stream, stats, version, e); });
        nb::dict d;
        d["num_inserted_rows"] = stats.inserted;
        d["num_updated_rows"] = stats.updated;
        d["num_deleted_rows"] = stats.deleted;
        return nb::make_tuple(d, version);
    });
    m.def("_ds_add_columns_sql", [](const std::filesystem::path& path,
                                    const std::vector<std::pair<std::string, std::string>>& columns) {
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_add_columns_sql(path, columns, version, e); });
        return version;
    });
    m.def("_ds_add_columns_nulls", [](const std::filesystem::path& path, nb::handle schema) {
        SchemaHolder holder;
        schema_of(schema, holder);
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_add_columns_nulls(path, holder.schema, version, e); });
        return version;
    });
    m.def("_ds_add_columns_stream", [](const std::filesystem::path& path, nb::handle data) {
        ArrowArrayStream stream = stream_of(data);
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_add_columns_stream(path, stream, version, e); });
        return version;
    });
    m.def("_ds_drop_columns", [](const std::filesystem::path& path, const std::vector<std::string>& columns) {
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_drop_columns(path, columns, version, e); });
        return version;
    });
    m.def("_ds_alter_columns", [](const std::filesystem::path& path, nb::list alterations) {
        std::vector<nano_lance::ColumnAlteration> alts;
        std::vector<std::unique_ptr<SchemaHolder>> types;
        for (auto item : alterations) {
            nb::dict a = nb::cast<nb::dict>(item);
            nano_lance::ColumnAlteration alt;
            alt.path = nb::cast<std::string>(a["path"]);
            if (a.contains("name") && !a["name"].is_none()) {
                alt.rename = nb::cast<std::string>(a["name"]);
            }
            if (a.contains("nullable") && !a["nullable"].is_none()) {
                alt.nullable = nb::cast<bool>(a["nullable"]);
            }
            if (a.contains("data_type") && !a["data_type"].is_none()) {
                types.push_back(std::make_unique<SchemaHolder>());
                schema_of(a["data_type"], *types.back());
                alt.data_type = &types.back()->schema;
            }
            alts.push_back(std::move(alt));
        }
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_alter_columns(path, alts, version, e); });
        return version;
    });
    m.def("_ds_compact_files", [](const std::filesystem::path& path, std::uint64_t target_rows,
                                  bool materialize_deletions, double threshold) {
        nano_lance::CompactionOptions options;
        options.target_rows_per_fragment = target_rows;
        options.materialize_deletions = materialize_deletions;
        options.materialize_deletions_threshold = threshold;
        nano_lance::CompactionMetrics metrics;
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_compact_files(path, options, metrics, version, e); });
        nb::dict d;
        d["fragments_removed"] = metrics.fragments_removed;
        d["fragments_added"] = metrics.fragments_added;
        d["files_removed"] = metrics.files_removed;
        d["files_added"] = metrics.files_added;
        return nb::make_tuple(d, version);
    });
    m.def("open_stream", &read_table_stream, nb::arg("path"), nb::arg("columns") = nb::none(),
          nb::arg("offset") = 0, nb::arg("length") = -1,
          "Open a Lance dataset as a streaming Arrow handle: one batch decoded per pull, so peak "
          "memory tracks one fragment rather than the dataset.");
}
