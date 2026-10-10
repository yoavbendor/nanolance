// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Python bindings for nanolance: fast Arrow <-> Lance reader/writer.

#include "arrow_capsule.hpp"

#include <nanolance/blob_v2_external.hpp>
#include <nanolance/dataset.hpp>
#include <nanolance/dataset_ops.hpp>
#include <nanolance/dataset_refs.hpp>
#include <nanolance/dataset_transaction.hpp>
#include <nanolance/manifest_writer.hpp>
#include <nanolance/expr.hpp>
#include <nanolance/jsonb.hpp>
#include <nanolance/fts_search.hpp>
#include <nanolance/index_optimize.hpp>
#include <nanolance/index_segments.hpp>
#include <nanolance/lance_table_reader.hpp>
#include <nanolance/nano_lance_reader.h>
#include <nanolance/nano_lance_writer.h>
#include <nanolance/row_ids.hpp>
#include <nanolance/scalar_index.hpp>
#include <nanolance/schema_mapper.hpp>
#include <nanolance/vector_search.hpp>
#include <nanolance/writer_internal.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/tuple.h>

#include <cstdint>
#include <cstring>
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

std::vector<std::uint64_t> ds_resolve_row_ids(const std::filesystem::path& path,
                                              std::optional<std::uint64_t> version,
                                              const std::vector<std::uint64_t>& ids) {
    std::vector<std::uint64_t> addresses;
    std::string error;
    bool ok = false;
    {
        nb::gil_scoped_release release;
        ok = nano_lance::resolve_row_ids(path, version, ids, addresses, error, /*skip_missing=*/true);
    }
    if (!ok) {
        throw_dataset(error);
    }
    return addresses;
}

std::pair<std::vector<std::uint64_t>, std::vector<std::uint64_t>> ds_row_versions(
    const std::filesystem::path& path, std::optional<std::uint64_t> version,
    const std::vector<std::uint64_t>& addresses) {
    std::vector<std::uint64_t> created;
    std::vector<std::uint64_t> updated;
    std::string error;
    bool ok = false;
    {
        nb::gil_scoped_release release;
        ok = nano_lance::row_versions_at(path, version, addresses, created, updated, error);
    }
    if (!ok) {
        throw_dataset(error);
    }
    return {std::move(created), std::move(updated)};
}

nb::object ds_scan(const std::filesystem::path& path, std::optional<std::uint64_t> version,
                   std::optional<std::vector<std::string>> columns,
                   std::optional<std::vector<std::uint64_t>> fragment_ids, std::uint64_t offset, std::int64_t length,
                   bool with_row_id, bool with_row_address, bool stream, std::optional<std::string> filter,
                   int blob_handling, bool use_scalar_index, bool include_deleted_rows) {
    auto request = make_request(version, columns, fragment_ids, with_row_id, with_row_address);
    request.use_scalar_index = use_scalar_index;
    request.include_deleted_rows = include_deleted_rows;
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
    d["transaction_file"] = v.transaction_file;
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
    nb::dict metadata;
    for (const auto& [key, value] : info.schema_metadata) {
        metadata[nb::bytes(key.data(), key.size())] = nb::bytes(value.data(), value.size());
    }
    return nb::make_tuple(info.num_rows, info.num_columns, ExportedSchema::adopt(std::move(schema)), columns, metadata);
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

    /// An append that covers only these top-level columns (the rest read as null).
    void project(const std::vector<std::string>& columns) {
        std::string error;
        if (!nano_lance::writer_project_append(&writer_, columns, error)) {
            throw std::invalid_argument(error);
        }
    }

    void set_transaction_property(const std::string& key, const std::string& value) {
        if (nano_lance_writer_set_transaction_property(&writer_, key.c_str(), value.c_str()) != 0) {
            throw std::runtime_error("failed to set a transaction property");
        }
    }
    void set_blob_pack_file_size(std::uint64_t bytes) {
        if (nano_lance_writer_set_blob_pack_file_size(&writer_, bytes) != 0) {
            throw std::runtime_error("failed to set the blob pack file size");
        }
    }
    void set_stable_row_ids(bool enable) {
        if (nano_lance_writer_set_stable_row_ids(&writer_, enable ? 1 : 0) != 0) {
            throw std::runtime_error(writer_.last_error);
        }
    }

    void set_initial_config(const std::string& key, const std::string& value) {
        if (nano_lance_writer_set_initial_config(&writer_, key.c_str(), value.c_str()) != 0) {
            throw std::runtime_error("failed to set the dataset config");
        }
    }

    /// The staged data files as fragments (lance.table.DataFragment messages, ids not yet assigned)
    /// and the schema they were written with (lance.file.Field messages) -- for a commit made apart
    /// (LanceFragment.create / write_fragments) instead of finish().
    nb::tuple take(bool keep_empty) {
        std::vector<nano_lance::NewFragment> files;
        nano_lance::LanceSchemaMapping mapping;
        std::string error;
        bool ok = false;
        {
            nb::gil_scoped_release release;
            std::lock_guard<std::mutex> lock(mutex_);
            ok = nano_lance::writer_take_staged(&writer_, files, mapping, error, keep_empty);
        }
        if (!ok) {
            throw_dataset(error);
        }
        nb::list fragments;
        for (const auto& f : files) {
            const auto bytes = nano_lance::pb::encode_data_fragment(nano_lance::make_data_fragment(mapping, f, 0));
            fragments.append(nb::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        }
        nb::list fields;
        for (const auto& f : mapping.fields) {
            const auto bytes = nano_lance::pb::encode_field(nano_lance::make_manifest_field(f));
            fields.append(nb::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        }
        nano_lance_writer_close(&writer_);
        open_ = false;
        return nb::make_tuple(fragments, fields);
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


/// A segment operation: InvalidArgument failures are ValueError, as Lance's are.
template <typename F>
void run_segment_op(F&& f) {
    std::string error;
    nano_lance::SegmentErrorKind kind{};
    bool ok = false;
    {
        nb::gil_scoped_release release;
        ok = f(error, kind);
    }
    if (!ok) {
        if (kind == nano_lance::SegmentErrorKind::InvalidArgument) {
            throw nb::value_error(error.c_str());
        }
        throw_dataset(error);
    }
}

std::vector<float> floats_of(const std::optional<nb::bytes>& b) {
    std::vector<float> out;
    if (b) {
        out.resize(b->size() / sizeof(float));
        std::memcpy(out.data(), b->c_str(), out.size() * sizeof(float));
    }
    return out;
}

nb::bytes bytes_of(const void* data, std::size_t size) { return nb::bytes(static_cast<const char*>(data), size); }

std::vector<std::uint8_t> vec_of(const nb::bytes& b) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(b.c_str());
    return {p, p + b.size()};
}

std::array<std::uint8_t, 16> uuid_of(const nb::bytes& b) {
    if (b.size() != 16U) {
        throw nb::value_error("a segment UUID is 16 bytes");
    }
    std::array<std::uint8_t, 16> out{};
    std::memcpy(out.data(), b.c_str(), 16);
    return out;
}

nano_lance::VectorIndexOptions vector_options_of(const nb::dict& d) {
    nano_lance::VectorIndexOptions o;
    if (d.contains("metric")) {
        const auto metric = nb::cast<std::string>(d["metric"]);
        if (!nano_lance::parse_vector_metric(metric, o.metric)) {
            throw nb::value_error(("metric '" + metric + "' is not supported (l2, cosine, dot)").c_str());
        }
    }
    if (d.contains("num_partitions") && !d["num_partitions"].is_none()) {
        o.num_partitions = nb::cast<std::uint32_t>(d["num_partitions"]);
    }
    if (d.contains("target_partition_size") && !d["target_partition_size"].is_none()) {
        o.target_partition_size = nb::cast<std::uint32_t>(d["target_partition_size"]);
    }
    const auto u32 = [&](const char* key, std::uint32_t& out) {
        if (d.contains(key) && !d[key].is_none()) {
            out = nb::cast<std::uint32_t>(d[key]);
        }
    };
    u32("num_sub_vectors", o.num_sub_vectors);
    u32("num_bits", o.num_bits);
    u32("max_iters", o.max_iters);
    u32("sample_rate", o.sample_rate);
    u32("m", o.hnsw_m);
    u32("ef_construction", o.hnsw_ef_construction);
    u32("max_level", o.hnsw_max_level);
    if (d.contains("seed") && !d["seed"].is_none()) {
        o.seed = nb::cast<std::uint64_t>(d["seed"]);
    }
    return o;
}

nb::dict segment_dict(const std::vector<std::uint8_t>& message) {
    nano_lance::IndexSegmentInfo s;
    std::string error;
    if (!nano_lance::decode_index_segment(message, s, error)) {
        throw nb::value_error(error.c_str());
    }
    nb::dict d;
    d["uuid"] = bytes_of(s.uuid.data(), 16);
    d["name"] = s.name;
    d["fields"] = s.fields;
    d["dataset_version"] = s.dataset_version;
    d["fragment_ids"] = s.has_fragment_ids ? nb::cast(s.fragment_ids) : nb::none();
    d["index_version"] = s.index_version;
    d["created_at"] = s.created_at;
    d["details_type_url"] = s.details_type_url;
    d["details_value"] = bytes_of(s.details_value.data(), s.details_value.size());
    nb::list files;
    for (const auto& [path, size] : s.files) {
        files.append(nb::make_tuple(path, size));
    }
    d["files"] = files;
    return d;
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
        d["indexed_fragments"] = w.indexed_fragments;
        return d;
    }, "Counters of the work done since the last reset (tests and diagnostics; not a stable API).");
    m.def("_reset_work_stats", [] { nano_lance_reset_work_stats(); });
    m.def("_ds_scan", &ds_scan, nb::arg("path"), nb::arg("version").none(), nb::arg("columns").none(),
          nb::arg("fragment_ids").none(), nb::arg("offset"), nb::arg("length"), nb::arg("with_row_id"),
          nb::arg("with_row_address"), nb::arg("stream"), nb::arg("filter").none() = nb::none(),
          nb::arg("blob_handling") = 0, nb::arg("use_scalar_index") = true,
          nb::arg("include_deleted_rows") = false);
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
    m.def("_external_blob_size", [](const std::string& uri) {
        std::uint64_t size = 0;
        char message[512] = {0};
        int rc = 0;
        {
            nb::gil_scoped_release release;
            rc = nano_lance_external_blob_size(uri.c_str(), &size, message, sizeof(message));
        }
        if (rc != NANO_LANCE_READER_OK) {
            throw_dataset(std::string("cannot size external blob ") + uri + ": " + message);
        }
        return size;
    }, nb::arg("uri"));
    // BlobHandling, in the order of the C++ enum.
    m.attr("BLOB_INGEST") = 0;
    m.attr("BLOB_DESCRIPTIONS") = 1;
    m.attr("BLOB_BINARY") = 2;
    m.attr("BLOB_LOCATIONS") = 3;
    m.def("_ds_schema", &ds_schema, nb::arg("path"), nb::arg("version").none());
    m.def("_ds_row_versions", &ds_row_versions, nb::arg("path"), nb::arg("version").none(), nb::arg("addresses"));
    m.def("_ds_resolve_row_ids", &ds_resolve_row_ids, nb::arg("path"), nb::arg("version").none(), nb::arg("ids"));
    m.def("_row_id_sequence_encode", [](const std::vector<std::uint64_t>& values) {
        const auto encoded = nano_lance::RowIdSequence::from_values(values).encode();
        return bytes_of(encoded.data(), encoded.size());
    }, nb::arg("values"));
    m.def("_row_id_sequence_decode", [](const nb::bytes& data) {
        nano_lance::RowIdSequence sequence;
        std::string error;
        if (!nano_lance::RowIdSequence::decode(reinterpret_cast<const std::uint8_t*>(data.c_str()), data.size(),
                                               sequence, error)) {
            throw std::runtime_error(error);
        }
        return sequence.to_vector();
    }, nb::arg("data"));
    m.def("_version_sequence_decode", [](const nb::bytes& data) {
        nano_lance::RowVersionSequence sequence;
        std::string error;
        if (!nano_lance::RowVersionSequence::decode(reinterpret_cast<const std::uint8_t*>(data.c_str()),
                                                    data.size(), sequence, error)) {
            throw std::runtime_error(error);
        }
        return sequence.to_vector();
    }, nb::arg("data"));
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
    // Tags, cleanup and drop (dataset_refs.hpp).
    m.def("_ds_tags", [](const std::filesystem::path& path) {
        std::vector<nano_lance::TagInfo> tags;
        std::string error;
        if (!nano_lance::list_tags(path, tags, error)) {
            throw_dataset(error);
        }
        nb::list out;
        for (const auto& t : tags) {
            nb::dict d;
            d["name"] = t.name;
            d["branch"] = t.branch ? nb::object(nb::str(t.branch->c_str())) : nb::none();
            d["version"] = t.version;
            d["created_at"] = t.created_at;
            d["updated_at"] = t.updated_at;
            d["manifest_size"] = t.manifest_size;
            d["metadata"] = t.metadata;
            out.append(d);
        }
        return out;
    });
    m.def("_ds_create_tag", [](const std::filesystem::path& path, const std::string& name, std::uint64_t version) {
        std::string error;
        if (!nano_lance::create_tag(path, name, version, error)) {
            throw_dataset(error);
        }
    });
    m.def("_ds_update_tag", [](const std::filesystem::path& path, const std::string& name, std::uint64_t version) {
        std::string error;
        if (!nano_lance::update_tag(path, name, version, error)) {
            throw_dataset(error);
        }
    });
    m.def("_ds_delete_tag", [](const std::filesystem::path& path, const std::string& name) {
        std::string error;
        if (!nano_lance::delete_tag(path, name, error)) {
            throw_dataset(error);
        }
    });
    m.def("_ds_replace_tag_metadata", [](const std::filesystem::path& path, const std::string& name,
                                         const std::map<std::string, std::string>& metadata) {
        std::string error;
        if (!nano_lance::replace_tag_metadata(path, name, metadata, error)) {
            throw_dataset(error);
        }
    });
    m.def(
        "_ds_cleanup",
        [](const std::filesystem::path& path, std::uint64_t read_version, std::optional<std::int64_t> older_than_ns,
           std::optional<std::uint64_t> retain_versions, bool delete_unverified, bool error_if_tagged,
           std::optional<std::uint64_t> delete_rate_limit, std::optional<std::vector<std::uint64_t>> versions,
           bool execute, std::size_t max_candidates) {
            nano_lance::CleanupPolicy policy;
            nano_lance::CleanupResult result;
            std::string error;
            bool ok = true;
            {
                nb::gil_scoped_release release;
                if (older_than_ns) {
                    policy.before_timestamp_ns =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() -
                        *older_than_ns;
                }
                if (versions) {
                    policy.versions.emplace(versions->begin(), versions->end());
                }
                policy.delete_unverified = delete_unverified;
                policy.error_if_tagged_old_versions = error_if_tagged;
                policy.delete_rate_limit = delete_rate_limit;
                ok = (!retain_versions || nano_lance::cleanup_retain_versions(path, *retain_versions, policy, error)) &&
                     nano_lance::cleanup_old_versions(path, read_version, policy, execute, max_candidates, result,
                                                      error);
            }
            if (!ok) {
                throw_dataset(error);
            }
            nb::dict stats;
            stats["bytes_removed"] = result.stats.bytes_removed;
            stats["old_versions"] = result.stats.old_versions;
            stats["data_files_removed"] = result.stats.data_files_removed;
            stats["transaction_files_removed"] = result.stats.transaction_files_removed;
            stats["index_files_removed"] = result.stats.index_files_removed;
            stats["deletion_files_removed"] = result.stats.deletion_files_removed;
            nb::list files;
            for (const auto& f : result.candidates) {
                nb::dict d;
                d["path"] = f.path;
                d["kind"] = f.kind;
                d["unverified"] = f.unverified;
                d["size_bytes"] = f.size_bytes;
                files.append(d);
            }
            return nb::make_tuple(result.read_version, stats, files, result.candidates_truncated);
        },
        nb::arg("path"), nb::arg("read_version"), nb::arg("older_than_ns").none(), nb::arg("retain_versions").none(),
        nb::arg("delete_unverified"), nb::arg("error_if_tagged"), nb::arg("delete_rate_limit").none(),
        nb::arg("versions").none(), nb::arg("execute"), nb::arg("max_candidates"));
    m.def("_ds_drop", [](const std::filesystem::path& path, bool ignore_not_found) {
        std::string error;
        bool ok = false;
        {
            nb::gil_scoped_release release;
            ok = nano_lance::drop_dataset(path, ignore_not_found, error);
        }
        if (!ok) {
            throw_dataset(error);
        }
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
        .def("project", &StagedWriter::project)
        .def("set_initial_config", &StagedWriter::set_initial_config)
        .def("set_stable_row_ids", &StagedWriter::set_stable_row_ids)
        .def("set_transaction_property", &StagedWriter::set_transaction_property)
        .def("set_blob_pack_file_size", &StagedWriter::set_blob_pack_file_size)
        .def("finish", &StagedWriter::finish, nb::arg("mode"), nb::arg("keep_empty") = false)
        .def("take", &StagedWriter::take, nb::arg("keep_empty") = false);
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
    // JSON columns: arrow.json text <-> Lance's JSONB (jsonb.hpp), a whole array at a time. Each
    // returns (schema capsule, array capsule): large_binary for _json_encode, utf8 for _json_decode.
    auto convert_json = [](nb::handle array, bool encode) {
        auto imported = nanolance_py::arrow_capsule::import_batch(array);
        ArrowArrayView view;
        ArrowError err;
        if (ArrowArrayViewInitFromSchema(&view, imported.first.get(), &err) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, imported.second.get(), &err) != NANOARROW_OK) {
            ArrowArrayViewReset(&view);
            throw std::invalid_argument(std::string("JSON conversion: ") + err.message);
        }
        auto* schema = static_cast<ArrowSchema*>(std::malloc(sizeof(ArrowSchema)));
        auto* out = static_cast<ArrowArray*>(std::malloc(sizeof(ArrowArray)));
        ArrowSchemaInit(schema);
        nanolance_py::arrow_capsule::nanoarrow_check(
            "ArrowSchemaSetType", ArrowSchemaSetType(schema, encode ? NANOARROW_TYPE_LARGE_BINARY : NANOARROW_TYPE_STRING));
        nanolance_py::arrow_capsule::nanoarrow_check(
            "ArrowArrayInitFromSchema", ArrowArrayInitFromSchema(out, schema, nullptr));
        nanolance_py::arrow_capsule::nanoarrow_check("ArrowArrayStartAppending", ArrowArrayStartAppending(out));
        std::string converted;
        std::string error;
        const auto length = imported.second->length;
        for (std::int64_t i = 0; i < length; ++i) {
            if (ArrowArrayViewIsNull(&view, i)) {
                nanolance_py::arrow_capsule::nanoarrow_check("ArrowArrayAppendNull", ArrowArrayAppendNull(out, 1));
                continue;
            }
            const auto value = ArrowArrayViewGetBytesUnsafe(&view, i);
            const std::string_view text(value.data.as_char, static_cast<std::size_t>(value.size_bytes));
            const bool ok = encode ? nano_lance::jsonb::encode(text, converted, error)
                                   : nano_lance::jsonb::to_text(text, converted, error);
            if (!ok) {
                ArrowArrayViewReset(&view);
                ArrowArrayRelease(out);
                ArrowSchemaRelease(schema);
                std::free(out);
                std::free(schema);
                throw std::invalid_argument(encode ? "Failed to encode JSON: " + error : "Failed to decode JSONB: " + error);
            }
            ArrowBufferView bytes;
            bytes.data.as_char = converted.data();
            bytes.size_bytes = static_cast<std::int64_t>(converted.size());
            nanolance_py::arrow_capsule::nanoarrow_check("ArrowArrayAppendBytes", ArrowArrayAppendBytes(out, bytes));
        }
        ArrowArrayViewReset(&view);
        nanolance_py::arrow_capsule::nanoarrow_check("ArrowArrayFinishBuildingDefault",
                                                     ArrowArrayFinishBuildingDefault(out, nullptr));
        return nb::make_tuple(nanolance_py::arrow_capsule::detail::make_schema_capsule(schema),
                              nanolance_py::arrow_capsule::detail::make_array_capsule(out));
    };
    m.def("_json_encode", [convert_json](nb::handle array) { return convert_json(array, true); });
    m.def("_json_decode", [convert_json](nb::handle array) { return convert_json(array, false); });

    // A SQL filter evaluated over one record batch, as the scan evaluates it: one byte per row, 1
    // where it holds. For filters the scan cannot push down (on _rowid / _rowaddr).
    m.def("_filter_mask", [](nb::handle batch, const std::string& sql) {
        auto imported = nanolance_py::arrow_capsule::import_batch(batch);
        nano_lance::expr::Expression expression;
        std::string error;
        std::vector<std::uint8_t> keep;
        if (!nano_lance::expr::Expression::parse(sql, expression, error) ||
            !expression.bind(*imported.first, error) || !expression.filter(*imported.second, keep, error)) {
            throw std::invalid_argument(error);
        }
        return nb::bytes(reinterpret_cast<const char*>(keep.data()), keep.size());
    });
    m.def("_ds_merge_insert", [](const std::filesystem::path& path, const std::vector<std::string>& on,
                                 bool update_all, bool insert_all, bool delete_by_source,
                                 const std::string& delete_condition, nb::handle data,
                                 const std::string& update_condition, const std::string& when_matched,
                                 bool uncommitted) {
        nano_lance::MergeInsertSpec spec;
        nano_lance::MergeInsertSpec::Uncommitted transaction;
        if (uncommitted) {
            spec.uncommitted = &transaction;
        }
        spec.on = on;
        spec.when_matched = !update_all             ? nano_lance::MergeInsertSpec::WhenMatched::DoNothing
                            : update_condition.empty() ? nano_lance::MergeInsertSpec::WhenMatched::UpdateAll
                                                       : nano_lance::MergeInsertSpec::WhenMatched::UpdateIf;
        if (when_matched == "fail") {
            spec.when_matched = nano_lance::MergeInsertSpec::WhenMatched::Fail;
        } else if (when_matched == "delete") {
            spec.when_matched = nano_lance::MergeInsertSpec::WhenMatched::Delete;
        }
        spec.when_matched_condition = update_condition;
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
        if (uncommitted) {
            return nb::make_tuple(d, version,
                                  nb::make_tuple(transaction.read_version, transaction.operation_field,
                                                 bytes_of(transaction.operation.data(), transaction.operation.size())));
        }
        return nb::make_tuple(d, version, nb::none());
    });
    m.def("_ds_commit_hand_built", [](const std::filesystem::path& path, std::uint32_t operation_field,
                                       const nb::bytes& operation, std::uint64_t base_version,
                                       std::uint64_t read_version, const std::map<std::string, std::string>& properties,
                                       bool detached, bool enable_stable_row_ids) {
        nano_lance::HandBuiltCommit commit;
        commit.detached = detached;
        commit.enable_stable_row_ids = enable_stable_row_ids;
        commit.operation_field = operation_field;
        commit.operation = vec_of(operation);
        commit.base_version = base_version;
        commit.read_version = read_version;
        commit.transaction_properties = properties;
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_commit_hand_built(path, commit, version, e); });
        return version;
    });
    m.def("_ds_fragment_messages", [](const std::filesystem::path& path, std::optional<std::uint64_t> version) {
        std::vector<std::vector<std::uint8_t>> out;
        run_op([&](std::string& e) {
            return nano_lance::dataset_fragment_messages(path, version.has_value(), version.value_or(0), out, e);
        });
        nb::list list;
        for (const auto& b : out) {
            list.append(bytes_of(b.data(), b.size()));
        }
        return list;
    });
    m.def("_ds_field_messages", [](const std::filesystem::path& path, std::optional<std::uint64_t> version) {
        std::vector<std::vector<std::uint8_t>> fields;
        std::map<std::string, std::vector<std::uint8_t>> metadata;
        run_op([&](std::string& e) {
            return nano_lance::dataset_field_messages(path, version.has_value(), version.value_or(0), fields,
                                                      metadata, e);
        });
        nb::list list;
        for (const auto& b : fields) {
            list.append(bytes_of(b.data(), b.size()));
        }
        nb::dict meta;
        for (const auto& [k, v] : metadata) {
            meta[nb::str(k.c_str(), k.size())] = bytes_of(v.data(), v.size());
        }
        return nb::make_tuple(list, meta);
    });
    m.def("_arrow_schema_from_field_messages", [](const std::vector<nb::bytes>& messages,
                                                   const std::map<std::string, nb::bytes>& metadata) {
        std::vector<nano_lance::pb::Field> fields;
        for (const auto& m : messages) {
            nano_lance::pb::Field f;
            if (!nano_lance::pb::decode_field(vec_of(m), f)) {
                throw nb::value_error("Failed to decode a schema field");
            }
            fields.push_back(std::move(f));
        }
        std::map<std::string, std::vector<std::uint8_t>> meta;
        for (const auto& [k, v] : metadata) {
            meta[k] = vec_of(v);
        }
        std::string error;
        ArrowSchema schema{};
        if (!nano_lance::lance_fields_arrow_schema(fields, meta, schema, error)) {
            throw_dataset(error);
        }
        return ExportedSchema::adopt(std::move(schema));
    });
    m.def("_lance_field_messages_from_arrow", [](nb::handle schema) {
        SchemaHolder holder;
        schema_of(schema, holder);
        nano_lance::LanceSchemaMapping mapping;
        std::string error;
        if (!nano_lance::map_arrow_schema(holder.schema, mapping, error)) {
            throw nb::value_error(error.c_str());
        }
        nb::list out;
        for (const auto& f : mapping.fields) {
            const auto bytes = nano_lance::pb::encode_field(nano_lance::make_manifest_field(f));
            out.append(bytes_of(bytes.data(), bytes.size()));
        }
        return out;
    });
    m.def("_fragment_delete", [](const std::filesystem::path& path, std::uint64_t version, std::uint64_t fragment_id,
                                 std::optional<std::string> predicate, const std::vector<std::uint32_t>& offsets)
              -> nb::object {
        std::vector<std::uint8_t> out;
        bool emptied = false;
        run_op([&](std::string& e) {
            return nano_lance::fragment_delete_rows(path, version, fragment_id, predicate ? &*predicate : nullptr,
                                                    offsets, out, emptied, e);
        });
        if (emptied) {
            return nb::none();
        }
        return bytes_of(out.data(), out.size());
    });
    m.def("_fragment_add_columns_sql", [](const std::filesystem::path& path, std::uint64_t version,
                                          std::uint64_t fragment_id,
                                          const std::vector<std::pair<std::string, std::string>>& columns,
                                          std::int32_t max_field_id) {
        std::vector<std::uint8_t> fragment;
        std::vector<std::vector<std::uint8_t>> fields;
        run_op([&](std::string& e) {
            return nano_lance::fragment_add_columns_sql(path, version, fragment_id, columns, max_field_id, fragment,
                                                        fields, e);
        });
        nb::list field_list;
        for (const auto& b : fields) {
            field_list.append(bytes_of(b.data(), b.size()));
        }
        return nb::make_tuple(bytes_of(fragment.data(), fragment.size()), field_list);
    });
    m.def("_fragment_write_columns", [](const std::filesystem::path& path, std::uint64_t version,
                                        std::uint64_t fragment_id, nb::handle data, bool replace,
                                        std::int32_t max_field_id) {
        ArrowArrayStream stream = stream_of(data);
        std::vector<std::uint8_t> fragment;
        std::vector<std::vector<std::uint8_t>> fields;
        std::vector<std::int32_t> written;
        run_op([&](std::string& e) {
            return nano_lance::fragment_write_columns(path, version, fragment_id, stream, replace, max_field_id,
                                                      fragment, fields, written, e);
        });
        nb::list field_list;
        for (const auto& b : fields) {
            field_list.append(bytes_of(b.data(), b.size()));
        }
        return nb::make_tuple(bytes_of(fragment.data(), fragment.size()), field_list, written);
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
    m.def("_ds_create_scalar_index", [](const std::filesystem::path& path, const std::string& column,
                                          const std::string& index_type, const std::string& name, bool replace) {
        nano_lance::ScalarIndexType type{};
        if (!nano_lance::parse_scalar_index_type(index_type, type)) {
            throw nb::value_error(("unsupported index type '" + index_type + "'").c_str());
        }
        nano_lance::ScalarIndexOptions options;
        options.name = name;
        options.replace = replace;
        std::uint64_t version = 0;
        run_op([&](std::string& e) {
            return nano_lance::dataset_create_scalar_index(path, column, type, options, version, e);
        });
        return version;
    });
    m.def("_ds_create_inverted_index", [](const std::filesystem::path& path, const std::string& column,
                                          const std::string& name, bool replace, const std::string& params) {
        nano_lance::InvertedIndexOptions options;
        options.name = name;
        options.replace = replace;
        std::string error;
        if (!nano_lance::fts::parse_params(params, options.params, error)) {
            throw nb::value_error(error.c_str());
        }
        std::uint64_t version = 0;
        run_op([&](std::string& e) {
            return nano_lance::dataset_create_inverted_index(path, column, options, version, e);
        });
        return version;
    });
    m.def("_index_segment_decode", [](const nb::bytes& message) { return segment_dict(vec_of(message)); });
    m.def("_index_segment_encode", [](const nb::dict& d) {
        nano_lance::IndexSegmentInfo s;
        s.uuid = uuid_of(nb::cast<nb::bytes>(d["uuid"]));
        s.name = nb::cast<std::string>(d["name"]);
        s.fields = nb::cast<std::vector<std::int32_t>>(d["fields"]);
        s.dataset_version = nb::cast<std::uint64_t>(d["dataset_version"]);
        s.has_fragment_ids = !d["fragment_ids"].is_none();
        if (s.has_fragment_ids) {
            s.fragment_ids = nb::cast<std::vector<std::uint32_t>>(d["fragment_ids"]);
        }
        s.index_version = nb::cast<std::uint32_t>(d["index_version"]);
        s.created_at = nb::cast<std::uint64_t>(d["created_at"]);
        s.details_type_url = nb::cast<std::string>(d["details_type_url"]);
        s.details_value = vec_of(nb::cast<nb::bytes>(d["details_value"]));
        for (auto item : nb::cast<nb::list>(d["files"])) {
            auto t = nb::cast<nb::tuple>(item);
            s.files.emplace_back(nb::cast<std::string>(t[0]), nb::cast<std::uint64_t>(t[1]));
        }
        const auto out = nano_lance::encode_index_segment(s);
        return bytes_of(out.data(), out.size());
    });
    m.def("_index_segment_details", [](const std::filesystem::path& path, const std::string& column,
                                       const nb::bytes& uuid) {
        std::string url;
        std::vector<std::uint8_t> value;
        const auto id = uuid_of(uuid);
        run_op([&](std::string& e) { return nano_lance::infer_index_segment_details(path, column, id, url, value, e); });
        return nb::make_tuple(url, bytes_of(value.data(), value.size()));
    });
    m.def("_index_build_segment",
          [](const std::filesystem::path& path, const std::string& column, const std::string& type,
             const std::string& name, std::optional<std::uint64_t> version,
             std::optional<std::vector<std::uint64_t>> fragments, std::optional<nb::bytes> uuid,
             const std::string& inverted_params, const nb::dict& vector, std::optional<nb::bytes> ivf_centroids,
             std::optional<nb::bytes> pq_codebook) {
              nano_lance::IndexSegmentBuild b;
              b.type = type;
              b.name = name;
              b.version = version.value_or(0U);
              b.fragments = std::move(fragments);
              if (uuid) {
                  b.uuid = uuid_of(*uuid);
              }
              if (type == "INVERTED") {
                  std::string error;
                  if (!nano_lance::fts::parse_params(inverted_params, b.inverted.params, error)) {
                      throw nb::value_error(error.c_str());
                  }
              }
              b.vector = vector_options_of(vector);
              b.ivf_centroids = floats_of(ivf_centroids);
              b.pq_codebook = floats_of(pq_codebook);
              std::vector<std::uint8_t> segment;
              run_segment_op([&](std::string& e, nano_lance::SegmentErrorKind& k) {
                  return nano_lance::dataset_build_index_segment(path, column, b, segment, e, k);
              });
              return bytes_of(segment.data(), segment.size());
          });
    m.def("_index_merge_segments", [](const std::filesystem::path& path, const std::vector<nb::bytes>& segments) {
        std::vector<std::vector<std::uint8_t>> in;
        for (const auto& s : segments) {
            in.push_back(vec_of(s));
        }
        std::vector<std::uint8_t> merged;
        run_segment_op([&](std::string& e, nano_lance::SegmentErrorKind& k) {
            return nano_lance::dataset_merge_index_segments(path, in, merged, e, k);
        });
        return bytes_of(merged.data(), merged.size());
    });
    m.def("_index_commit_segments", [](const std::filesystem::path& path, const std::string& name,
                                       const std::string& column, const std::vector<nb::bytes>& segments) {
        std::vector<std::vector<std::uint8_t>> in;
        for (const auto& s : segments) {
            in.push_back(vec_of(s));
        }
        std::uint64_t version = 0;
        run_segment_op([&](std::string& e, nano_lance::SegmentErrorKind& k) {
            return nano_lance::dataset_commit_index_segments(path, name, column, in, version, e, k);
        });
        return version;
    });
    m.def("_index_train_model", [](const std::filesystem::path& path, std::optional<std::uint64_t> version,
                                   const std::string& column, const std::string& type, const nb::dict& options,
                                   std::optional<std::vector<std::uint64_t>> fragments,
                                   std::optional<nb::bytes> ivf_centroids) {
        auto o = vector_options_of(options);
        o.type = type;
        const auto centroids = floats_of(ivf_centroids);
        nano_lance::TrainedVectorModel model;
        run_op([&](std::string& e) {
            return nano_lance::dataset_train_vector_model(path, version.value_or(0U), column, o,
                                                          fragments ? &*fragments : nullptr,
                                                          ivf_centroids ? &centroids : nullptr, model, e);
        });
        return nb::make_tuple(model.dim, model.partitions,
                              bytes_of(model.centroids.data(), model.centroids.size() * sizeof(float)),
                              bytes_of(model.codebook.data(), model.codebook.size() * sizeof(float)));
    });
    m.def("_ds_drop_index", [](const std::filesystem::path& path, const std::string& name) {
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_drop_index(path, name, version, e); });
        return version;
    });
    m.def("_ds_list_indices", [](const std::filesystem::path& path, std::optional<std::uint64_t> version) {
        std::vector<nano_lance::IndexInfo> indices;
        run_op([&](std::string& e) {
            return nano_lance::dataset_list_indices(path, version.has_value(), version.value_or(0), indices, e);
        });
        nb::list out;
        for (const auto& i : indices) {
            nb::dict d;
            d["name"] = i.name;
            d["uuid"] = i.uuid;
            d["type"] = i.type;
            d["fields"] = i.fields;
            d["fragment_ids"] = i.fragment_ids;
            d["dataset_version"] = i.dataset_version;
            d["index_version"] = i.index_version;
            d["type_url"] = i.type_url;
            d["field_ids"] = i.field_ids;
            d["created_at"] = i.created_at;
            d["size_bytes"] = i.size_bytes;
            d["rows_indexed"] = i.rows_indexed;
            out.append(d);
        }
        return out;
    });
    m.def("_ds_fields", [](const std::filesystem::path& path, std::optional<std::uint64_t> version) {
        std::vector<nano_lance::DatasetField> fields;
        run_op([&](std::string& e) {
            return nano_lance::dataset_fields(path, version.has_value(), version.value_or(0), fields, e);
        });
        nb::list out;
        for (const auto& f : fields) {
            nb::dict d;
            d["id"] = f.id;
            d["parent_id"] = f.parent_id;
            d["name"] = f.name;
            d["logical_type"] = f.logical_type;
            d["nullable"] = f.nullable;
            d["encoding"] = f.encoding;
            d["metadata"] = f.metadata;
            out.append(d);
        }
        return out;
    });
    m.def("_ds_update_field_metadata",
          [](const std::filesystem::path& path,
             const std::map<std::int32_t, std::vector<std::pair<std::string, std::optional<std::string>>>>& updates,
             bool replace) {
              std::map<std::int32_t, nano_lance::FieldMetadataUpdate> by_id;
              for (const auto& [id, entries] : updates) {
                  by_id[id] = nano_lance::FieldMetadataUpdate{entries, replace};
              }
              std::uint64_t version = 0;
              run_op([&](std::string& e) { return nano_lance::dataset_update_field_metadata(path, by_id, version, e); });
              return version;
          });
    m.def("_lance_fields_from_arrow", [](nb::handle schema) {
        SchemaHolder holder;
        schema_of(schema, holder);
        nano_lance::LanceSchemaMapping mapping;
        std::string error;
        if (!nano_lance::map_arrow_schema(holder.schema, mapping, error)) {
            throw nb::value_error(error.c_str());
        }
        nb::list out;
        for (const auto& f : mapping.fields) {
            nb::dict d;
            d["id"] = f.id;
            d["parent_id"] = f.parent_id;
            d["name"] = f.name;
            d["logical_type"] = nano_lance::lance_on_disk_logical_type(f.logical_type);
            d["nullable"] = f.nullable;
            d["encoding"] = nano_lance::lance_on_disk_field_encoding(f.logical_type);
            d["metadata"] = f.metadata;
            out.append(d);
        }
        return out;
    });
    m.def("_ds_data_stats", [](const std::filesystem::path& path, std::optional<std::uint64_t> version) {
        std::vector<std::pair<std::int32_t, std::uint64_t>> stats;
        run_op([&](std::string& e) {
            return nano_lance::dataset_data_stats(path, version.has_value(), version.value_or(0), stats, e);
        });
        return stats;
    });
    m.def("_vector_index_model", [](const std::filesystem::path& dir) {
        nano_lance::VectorIndexModel model;
        run_op([&](std::string& e) { return nano_lance::read_vector_index_model(dir, model, e); });
        nb::dict d;
        d["index_metadata"] = model.index_metadata;
        d["sub_index_metadata"] = model.sub_index_metadata;
        d["storage_metadata"] = model.storage_metadata;
        nb::list centroids;
        for (std::size_t p = 0; p < model.partitions && model.dim != 0U; ++p) {
            nb::list row;
            for (std::size_t j = 0; j < model.dim && p * model.dim + j < model.centroids.size(); ++j) {
                row.append(static_cast<double>(model.centroids[p * model.dim + j]));
            }
            centroids.append(row);
        }
        d["centroids"] = centroids;
        d["partition_sizes"] = model.partition_sizes;
        d["loss"] = model.has_loss ? nb::cast(model.loss) : nb::none();
        return d;
    });
    m.def("_ds_explain_filter", [](const std::filesystem::path& path, std::optional<std::uint64_t> version,
                                   const std::string& filter) {
        std::vector<std::string> lines;
        run_op([&](std::string& e) {
            return nano_lance::dataset_explain_filter(path, version.has_value(), version.value_or(0), filter, lines, e);
        });
        return lines;
    });
    m.def("_ds_nearest", [](const std::filesystem::path& path, std::optional<std::uint64_t> version,
                            const std::string& column, const std::vector<float>& key, std::uint64_t k,
                            std::uint32_t minimum_nprobes, std::optional<std::uint32_t> maximum_nprobes,
                            std::optional<std::uint32_t> refine_factor, std::optional<std::string> metric,
                            bool use_index, std::optional<float> lower_bound, std::optional<float> upper_bound,
                            std::optional<std::string> filter, bool prefilter, bool fast_search,
                            std::optional<std::uint32_t> ef) {
        nano_lance::NearestQuery q;
        q.ef = ef;
        q.has_version = version.has_value();
        q.version = version.value_or(0);
        q.column = column;
        q.key = key;
        q.k = k;
        q.minimum_nprobes = minimum_nprobes;
        q.maximum_nprobes = maximum_nprobes;
        q.refine_factor = refine_factor;
        if (metric) {
            nano_lance::VectorMetric m;
            if (!nano_lance::parse_vector_metric(*metric, m)) {
                throw nb::value_error(("unknown distance type '" + *metric + "'").c_str());
            }
            q.metric = m;
        }
        q.use_index = use_index;
        q.lower_bound = lower_bound;
        q.upper_bound = upper_bound;
        q.filter = filter;
        q.prefilter = prefilter;
        q.fast_search = fast_search;
        nano_lance::NearestResult result;
        run_op([&](std::string& e) { return nano_lance::dataset_nearest(path, q, result, e); });
        return std::make_tuple(result.row_ids, result.distances, result.plan);
    });
    m.def("_ds_full_text_search", [](const std::filesystem::path& path, std::optional<std::uint64_t> version,
                                     const std::string& query, std::optional<std::uint64_t> limit,
                                     std::optional<std::string> filter, bool prefilter, bool fast_search) {
        nano_lance::FtsSearchRequest r;
        r.has_version = version.has_value();
        r.version = version.value_or(0);
        std::string error;
        if (!nano_lance::parse_fts_query(query, r.query, error)) {
            throw nb::value_error(error.c_str());
        }
        r.limit = limit;
        r.filter = filter;
        r.prefilter = prefilter;
        r.fast_search = fast_search;
        nano_lance::FtsSearchResult result;
        run_op([&](std::string& e) { return nano_lance::dataset_full_text_search(path, r, result, e); });
        return std::make_tuple(result.row_ids, result.scores, result.plan);
    });
    m.def("_ds_create_vector_index", [](const std::filesystem::path& path, const std::string& column,
                                        const std::string& type, const std::string& name, const std::string& metric,
                                        bool replace, std::optional<std::uint32_t> num_partitions,
                                        std::optional<std::uint32_t> target_partition_size,
                                        std::uint32_t num_sub_vectors, std::uint32_t num_bits, std::uint32_t max_iters,
                                        std::uint32_t sample_rate, std::optional<std::uint64_t> seed,
                                        std::uint32_t hnsw_m, std::uint32_t ef_construction,
                                        std::uint32_t max_level) {
        nano_lance::VectorIndexOptions o;
        o.type = type;
        o.name = name;
        if (!nano_lance::parse_vector_metric(metric, o.metric)) {
            throw nb::value_error(("metric '" + metric + "' is not supported (l2, cosine, dot)").c_str());
        }
        o.replace = replace;
        o.num_partitions = num_partitions;
        o.target_partition_size = target_partition_size;
        o.num_sub_vectors = num_sub_vectors;
        o.num_bits = num_bits;
        o.max_iters = max_iters;
        o.sample_rate = sample_rate;
        o.seed = seed;
        o.hnsw_m = hnsw_m;
        o.hnsw_ef_construction = ef_construction;
        o.hnsw_max_level = max_level;
        std::uint64_t version = 0;
        run_op([&](std::string& e) { return nano_lance::dataset_create_vector_index(path, column, o, version, e); });
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
                                  bool materialize_deletions, double threshold, bool reindex,
                                  std::optional<std::uint64_t> max_source_fragments,
                                  std::optional<std::uint64_t> max_source_rows,
                                  std::optional<std::uint64_t> max_source_bytes,
                                  const std::vector<std::uint64_t>& excluded_fragment_ids,
                                  std::uint64_t max_bytes_per_file, std::uint64_t batch_size) {
        nano_lance::CompactionOptions options;
        options.reindex = reindex;
        options.max_source_fragments = max_source_fragments;
        options.max_source_rows = max_source_rows;
        options.max_source_bytes = max_source_bytes;
        options.excluded_fragment_ids = excluded_fragment_ids;
        options.max_bytes_per_file = max_bytes_per_file;
        options.batch_size = batch_size;
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
        d["indexes_reindexed"] = metrics.indexes_reindexed;
        d["indexes_not_reindexed"] = metrics.indexes_not_reindexed;
        return nb::make_tuple(d, version);
    });
    m.def("_ds_optimize_indices", [](const std::filesystem::path& path, const std::vector<std::string>& names,
                                     std::optional<std::uint32_t> num_indices_to_merge, bool retrain) {
        nano_lance::OptimizeIndicesOptions options;
        options.index_names = names;
        options.num_indices_to_merge = num_indices_to_merge;
        options.retrain = retrain;
        nano_lance::OptimizeIndicesResult result;
        run_op([&](std::string& e) { return nano_lance::dataset_optimize_indices(path, options, result, e); });
        return nb::make_tuple(result.optimized, result.committed, result.version);
    });
    m.def("open_stream", &read_table_stream, nb::arg("path"), nb::arg("columns") = nb::none(),
          nb::arg("offset") = 0, nb::arg("length") = -1,
          "Open a Lance dataset as a streaming Arrow handle: one batch decoded per pull, so peak "
          "memory tracks one fragment rather than the dataset.");
}
