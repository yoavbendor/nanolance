// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/parallel.hpp"
#include "nanolance/nano_lance_reader.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void set_error(char* error_message, const size_t error_message_capacity, const std::string& msg) {
    if (error_message != nullptr && error_message_capacity > 0U) {
        std::strncpy(error_message, msg.c_str(), error_message_capacity - 1U);
        error_message[error_message_capacity - 1U] = '\0';
    }
}

int map_status(const std::string& error) {
    if (error.find("not found") != std::string::npos || error.find("failed to open") != std::string::npos ||
        error.find("unreadable") != std::string::npos) {
        return NANO_LANCE_READER_IO_ERROR;
    }
    if (error.find("Invalid") != std::string::npos || error.find("must not") != std::string::npos) {
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    return NANO_LANCE_READER_PARSE_ERROR;
}

/// `columns` is null for "every column"; otherwise it names the top-level columns to decode and the
/// rest are skipped entirely (see lance_table_read_dataset_projected). Everything else about the two
/// paths is identical, so they share this body rather than duplicating the ownership contract -- the
/// part that, when it was duplicated, segfaulted every failed read.
/// Collect `column_names` into owned strings. NULL/0 means "every column" and yields an empty vector,
/// which callers pass as a null `columns` pointer.
int collect_columns(const char* const* column_names, size_t column_count, std::vector<std::string>& out,
                    char* error_message, size_t error_message_capacity) {
    out.clear();
    if (column_count == 0) {
        return NANO_LANCE_READER_OK;
    }
    if (column_names == nullptr) {
        set_error(error_message, error_message_capacity,
                  "column_names is required when column_count is nonzero");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    out.reserve(column_count);
    for (size_t i = 0; i < column_count; ++i) {
        if (column_names[i] == nullptr) {
            set_error(error_message, error_message_capacity, "column_names contains a null entry");
            return NANO_LANCE_READER_INVALID_ARGUMENT;
        }
        out.emplace_back(column_names[i]);
    }
    return NANO_LANCE_READER_OK;
}

/// A negative `length` means "to the end of the dataset" -- the usual C convention, and clearer at a
/// call site than spelling out a sentinel like UINT64_MAX.
nano_lance::LanceRowRange make_range(uint64_t offset, int64_t length) {
    nano_lance::LanceRowRange range;
    range.offset = offset;
    range.length = length < 0 ? nano_lance::LanceRowRange::kAllRows : static_cast<uint64_t>(length);
    return range;
}

/// Nothing may unwind through a C entry point: an exception crossing `extern "C"` terminates the
/// caller's process. The read path is written to refuse bad input by return value, but allocation can
/// still throw -- a file may declare more data than the machine has -- so every entry point runs
/// inside this and turns an exception into an error status with a message.
template <class Body>
int guarded(char* error_message, size_t error_message_capacity, Body&& body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        set_error(error_message, error_message_capacity,
                  "out of memory: the dataset declares more data than could be allocated");
    } catch (const std::exception& e) {
        set_error(error_message, error_message_capacity, std::string("internal error: ") + e.what());
    } catch (...) {
        set_error(error_message, error_message_capacity, "internal error");
    }
    return NANO_LANCE_READER_IO_ERROR;
}

int read_dataset_impl_unguarded(const char* dataset_path, bool trusted_input, const std::vector<std::string>* columns,
                      const nano_lance::LanceRowRange& range, struct ArrowSchema* out_schema,
                      struct ArrowArray** out_batches, size_t* out_batch_count, char* error_message,
                      size_t error_message_capacity, const std::vector<std::uint64_t>* take = nullptr) {
    if (out_schema == nullptr || out_batches == nullptr || out_batch_count == nullptr) {
        set_error(error_message, error_message_capacity, "out_schema, out_batches, and out_batch_count are required");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    if (dataset_path == nullptr) {
        set_error(error_message, error_message_capacity, "dataset_path is required");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }

    *out_batches = nullptr;
    *out_batch_count = 0;
    ArrowSchemaInit(out_schema);

    std::vector<ArrowArray> batches;
    std::string error;
    const bool ok = take != nullptr
                        ? nano_lance::lance_table_take(std::filesystem::path(dataset_path), columns, *take, *out_schema,
                                                       batches, error, trusted_input)
                        : nano_lance::lance_table_read_dataset_range(std::filesystem::path(dataset_path), columns,
                                                                     range, *out_schema, batches, error,
                                                                     trusted_input);
    if (!ok) {
        // lance_table_read_dataset already released the schema (see its declaration). Releasing it
        // again here called through a null `release` pointer -- every failed read from the C API or
        // the Python bindings segfaulted the process instead of returning this status.
        set_error(error_message, error_message_capacity, error);
        return map_status(error);
    }

    if (batches.empty()) {
        *out_batches = nullptr;
        *out_batch_count = 0;
        return NANO_LANCE_READER_OK;
    }

    auto* heap_batches = static_cast<ArrowArray*>(std::malloc(batches.size() * sizeof(ArrowArray)));
    if (heap_batches == nullptr) {
        ArrowSchemaRelease(out_schema);
        for (auto& batch : batches) {
            ArrowArrayRelease(&batch);
        }
        set_error(error_message, error_message_capacity, "out of memory allocating batch array");
        return NANO_LANCE_READER_IO_ERROR;
    }

    for (std::size_t i = 0; i < batches.size(); ++i) {
        heap_batches[i] = batches[i];
    }
    *out_batches = heap_batches;
    *out_batch_count = batches.size();
    return NANO_LANCE_READER_OK;
}

int read_dataset_impl(const char* dataset_path, bool trusted_input, const std::vector<std::string>* columns,
                      const nano_lance::LanceRowRange& range, struct ArrowSchema* out_schema,
                      struct ArrowArray** out_batches, size_t* out_batch_count, char* error_message,
                      size_t error_message_capacity, const std::vector<std::uint64_t>* take = nullptr) {
    bool returned = false;
    const int rc = guarded(error_message, error_message_capacity, [&] {
        const int inner = read_dataset_impl_unguarded(dataset_path, trusted_input, columns, range, out_schema,
                                                      out_batches, out_batch_count, error_message,
                                                      error_message_capacity, take);
        returned = true;
        return inner;
    });
    if (!returned && out_schema != nullptr && out_schema->release != nullptr) {
        // An exception can leave the schema built and the batches not: release it, as any other
        // failure does, so the caller's contract (nothing to free on error) holds.
        out_schema->release(out_schema);
        std::memset(out_schema, 0, sizeof(*out_schema));
    }
    return rc;
}

}  // namespace

extern "C" int nano_lance_table_read_dataset(const char* dataset_path, struct ArrowSchema* out_schema,
                                             struct ArrowArray** out_batches, size_t* out_batch_count,
                                             char* error_message, size_t error_message_capacity) {
    return read_dataset_impl(dataset_path, /*trusted_input=*/false, /*columns=*/nullptr,
                             nano_lance::LanceRowRange{}, out_schema, out_batches, out_batch_count,
                             error_message, error_message_capacity);
}

extern "C" int nano_lance_table_read_dataset_ex(const char* dataset_path, int trusted_input,
                                                struct ArrowSchema* out_schema, struct ArrowArray** out_batches,
                                                size_t* out_batch_count, char* error_message,
                                                size_t error_message_capacity) {
    return read_dataset_impl(dataset_path, trusted_input != 0, /*columns=*/nullptr,
                             nano_lance::LanceRowRange{}, out_schema, out_batches, out_batch_count,
                             error_message, error_message_capacity);
}

extern "C" int nano_lance_table_read_dataset_projected(const char* dataset_path,
                                                       const char* const* column_names, size_t column_count,
                                                       int trusted_input, struct ArrowSchema* out_schema,
                                                       struct ArrowArray** out_batches, size_t* out_batch_count,
                                                       char* error_message, size_t error_message_capacity) {
    if (column_names == nullptr || column_count == 0) {
        // A zero-column projection reaches the schema mapper as "no root fields" and comes back with
        // that internal wording, which tells a caller nothing about what they did. Refuse it here
        // instead. (Reading zero columns to get just a row count needs its own entry point, not an
        // empty projection.)
        set_error(error_message, error_message_capacity, "at least one column name is required");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    std::vector<std::string> columns;
    if (const int rc = collect_columns(column_names, column_count, columns, error_message,
                                       error_message_capacity);
        rc != NANO_LANCE_READER_OK) {
        return rc;
    }
    return read_dataset_impl(dataset_path, trusted_input != 0, &columns, nano_lance::LanceRowRange{},
                             out_schema, out_batches, out_batch_count, error_message,
                             error_message_capacity);
}

extern "C" int nano_lance_table_read_dataset_range(const char* dataset_path,
                                                   const char* const* column_names, size_t column_count,
                                                   uint64_t offset, int64_t length, int trusted_input,
                                                   struct ArrowSchema* out_schema,
                                                   struct ArrowArray** out_batches, size_t* out_batch_count,
                                                   char* error_message, size_t error_message_capacity) {
    // NULL/0 here means "every column", matching nano_lance_table_open_stream. That differs from
    // nano_lance_table_read_dataset_projected, which refuses a zero-column projection -- there an
    // empty list is a caller mistake, here it is the ordinary "no projection" case.
    std::vector<std::string> columns;
    if (const int rc = collect_columns(column_names, column_count, columns, error_message,
                                       error_message_capacity);
        rc != NANO_LANCE_READER_OK) {
        return rc;
    }
    return read_dataset_impl(dataset_path, trusted_input != 0, columns.empty() ? nullptr : &columns,
                             make_range(offset, length), out_schema, out_batches, out_batch_count,
                             error_message, error_message_capacity);
}

extern "C" int nano_lance_table_take(const char* dataset_path, const char* const* column_names, size_t column_count,
                                     const uint64_t* indices, size_t index_count, int trusted_input,
                                     struct ArrowSchema* out_schema, struct ArrowArray** out_batches,
                                     size_t* out_batch_count, char* error_message, size_t error_message_capacity) {
    if (indices == nullptr && index_count != 0U) {
        set_error(error_message, error_message_capacity, "indices is NULL but index_count is not 0");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    std::vector<std::string> columns;
    if (const int rc = collect_columns(column_names, column_count, columns, error_message,
                                       error_message_capacity);
        rc != NANO_LANCE_READER_OK) {
        return rc;
    }
    const std::vector<std::uint64_t> take(indices, indices + index_count);
    return read_dataset_impl(dataset_path, trusted_input != 0, columns.empty() ? nullptr : &columns,
                             nano_lance::LanceRowRange{}, out_schema, out_batches, out_batch_count, error_message,
                             error_message_capacity, &take);
}

namespace {

/// Backing state for the ArrowArrayStream handed out by nano_lance_table_open_stream.
struct StreamPrivate {
    nano_lance::LanceTableStream stream;
    ArrowSchema schema{};     // served by get_schema; deep-copied per call, as the interface requires
    std::string last_error;

    ~StreamPrivate() {
        if (schema.release != nullptr) {
            ArrowSchemaRelease(&schema);
        }
    }
};

StreamPrivate* stream_private(struct ArrowArrayStream* stream) {
    return static_cast<StreamPrivate*>(stream->private_data);
}

int stream_get_schema(struct ArrowArrayStream* stream, struct ArrowSchema* out) {
    auto* self = stream_private(stream);
    // A deep copy every call: the Arrow C stream interface lets a consumer call get_schema more than
    // once and take ownership of each result, so handing out our only copy would be a use-after-free
    // the second time.
    if (ArrowSchemaDeepCopy(&self->schema, out) != NANOARROW_OK) {
        self->last_error = "failed to copy the dataset schema";
        return EIO;
    }
    return 0;
}

int stream_get_next(struct ArrowArrayStream* stream, struct ArrowArray* out) {
    auto* self = stream_private(stream);
    try {
        std::string error;
        if (!self->stream.next(*out, error)) {
            self->last_error = error;
            return EIO;
        }
    } catch (const std::bad_alloc&) {
        self->last_error = "out of memory: the dataset declares more data than could be allocated";
        return ENOMEM;
    } catch (const std::exception& e) {
        self->last_error = std::string("internal error: ") + e.what();
        return EIO;
    }
    return 0;  // end of stream leaves out->release null, which is what the interface expects
}

const char* stream_get_last_error(struct ArrowArrayStream* stream) {
    auto* self = stream_private(stream);
    return self->last_error.empty() ? nullptr : self->last_error.c_str();
}

void stream_release(struct ArrowArrayStream* stream) {
    delete stream_private(stream);
    stream->private_data = nullptr;
    stream->release = nullptr;
}

}  // namespace

namespace {

int open_stream_impl(const char* dataset_path, const char* const* column_names, size_t column_count,
                     const nano_lance::LanceRowRange& range, int trusted_input,
                     struct ArrowArrayStream* out_stream, char* error_message,
                     size_t error_message_capacity) {
    if (out_stream == nullptr) {
        set_error(error_message, error_message_capacity, "out_stream is required");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    std::memset(out_stream, 0, sizeof(*out_stream));
    if (dataset_path == nullptr) {
        set_error(error_message, error_message_capacity, "dataset_path is required");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }

    std::vector<std::string> columns;
    if (const int rc = collect_columns(column_names, column_count, columns, error_message,
                                       error_message_capacity);
        rc != NANO_LANCE_READER_OK) {
        return rc;
    }

    auto self = std::make_unique<StreamPrivate>();
    std::string error;
    const int rc = guarded(error_message, error_message_capacity, [&] {
        if (!nano_lance::LanceTableStream::open_range(std::filesystem::path(dataset_path),
                                                      columns.empty() ? nullptr : &columns, range,
                                                      self->schema, self->stream, error,
                                                      trusted_input != 0)) {
            // open() already released the schema on failure (see its declaration), and
            // ~StreamPrivate checks `release` before touching it, so unwinding here is safe.
            set_error(error_message, error_message_capacity, error);
            return map_status(error);
        }
        return static_cast<int>(NANO_LANCE_READER_OK);
    });
    if (rc != NANO_LANCE_READER_OK) {
        return rc;
    }

    out_stream->get_schema = &stream_get_schema;
    out_stream->get_next = &stream_get_next;
    out_stream->get_last_error = &stream_get_last_error;
    out_stream->release = &stream_release;
    out_stream->private_data = self.release();
    return NANO_LANCE_READER_OK;
}

}  // namespace

extern "C" int nano_lance_table_open_stream(const char* dataset_path, const char* const* column_names,
                                            size_t column_count, int trusted_input,
                                            struct ArrowArrayStream* out_stream, char* error_message,
                                            size_t error_message_capacity) {
    return open_stream_impl(dataset_path, column_names, column_count, nano_lance::LanceRowRange{},
                            trusted_input, out_stream, error_message, error_message_capacity);
}

extern "C" int nano_lance_table_open_stream_range(const char* dataset_path,
                                                  const char* const* column_names, size_t column_count,
                                                  uint64_t offset, int64_t length, int trusted_input,
                                                  struct ArrowArrayStream* out_stream, char* error_message,
                                                  size_t error_message_capacity) {
    return open_stream_impl(dataset_path, column_names, column_count, make_range(offset, length),
                            trusted_input, out_stream, error_message, error_message_capacity);
}

extern "C" int nano_lance_table_read_schema(const char* dataset_path, struct ArrowSchema* out_schema,
                                           char* error_message, size_t error_message_capacity) {
    if (out_schema == nullptr) {
        set_error(error_message, error_message_capacity, "out_schema is required");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    std::memset(out_schema, 0, sizeof(*out_schema));
    if (dataset_path == nullptr) {
        set_error(error_message, error_message_capacity, "dataset_path is required");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    return guarded(error_message, error_message_capacity, [&] {
        std::string error;
        if (!nano_lance::lance_table_read_schema(std::filesystem::path(dataset_path), *out_schema, error)) {
            // The C++ side leaves out_schema released on failure; zero it so the caller cannot be
            // tempted to release it a second time. Double-releasing a schema is what used to segfault
            // every failed read through this shim.
            std::memset(out_schema, 0, sizeof(*out_schema));
            set_error(error_message, error_message_capacity, error);
            return map_status(error);
        }
        return static_cast<int>(NANO_LANCE_READER_OK);
    });
}

extern "C" int nano_lance_table_count_rows(const char* dataset_path, uint64_t* out_rows,
                                           char* error_message, size_t error_message_capacity) {
    if (out_rows == nullptr) {
        set_error(error_message, error_message_capacity, "out_rows is required");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    *out_rows = 0;
    if (dataset_path == nullptr) {
        set_error(error_message, error_message_capacity, "dataset_path is required");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    return guarded(error_message, error_message_capacity, [&] {
        std::string error;
        if (!nano_lance::lance_table_count_rows(std::filesystem::path(dataset_path), *out_rows, error)) {
            set_error(error_message, error_message_capacity, error);
            return map_status(error);
        }
        return static_cast<int>(NANO_LANCE_READER_OK);
    });
}

extern "C" void nano_lance_table_read_result_free(struct ArrowSchema* schema, struct ArrowArray* batches,
                                                  size_t batch_count) {
    if (schema != nullptr && schema->release != nullptr) {
        ArrowSchemaRelease(schema);
    }
    if (batches == nullptr) {
        return;
    }
    for (size_t i = 0; i < batch_count; ++i) {
        if (batches[i].release != nullptr) {
            ArrowArrayRelease(&batches[i]);
        }
    }
    std::free(batches);
}

void nano_lance_set_threads(size_t threads) { nano_lance::parallel::set_threads(threads); }

size_t nano_lance_threads(void) { return nano_lance::parallel::threads(); }
