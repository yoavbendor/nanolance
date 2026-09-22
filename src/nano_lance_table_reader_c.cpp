// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_reader.h"

#include <cstring>
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
int read_dataset_impl(const char* dataset_path, bool trusted_input, const std::vector<std::string>* columns,
                      struct ArrowSchema* out_schema,
                      struct ArrowArray** out_batches, size_t* out_batch_count, char* error_message,
                      size_t error_message_capacity) {
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
    const bool ok =
        columns == nullptr
            ? nano_lance::lance_table_read_dataset(std::filesystem::path(dataset_path), *out_schema, batches,
                                                   error, trusted_input)
            : nano_lance::lance_table_read_dataset_projected(std::filesystem::path(dataset_path), *columns,
                                                             *out_schema, batches, error, trusted_input);
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

}  // namespace

extern "C" int nano_lance_table_read_dataset(const char* dataset_path, struct ArrowSchema* out_schema,
                                             struct ArrowArray** out_batches, size_t* out_batch_count,
                                             char* error_message, size_t error_message_capacity) {
    return read_dataset_impl(dataset_path, /*trusted_input=*/false, /*columns=*/nullptr, out_schema,
                             out_batches, out_batch_count, error_message, error_message_capacity);
}

extern "C" int nano_lance_table_read_dataset_ex(const char* dataset_path, int trusted_input,
                                                struct ArrowSchema* out_schema, struct ArrowArray** out_batches,
                                                size_t* out_batch_count, char* error_message,
                                                size_t error_message_capacity) {
    return read_dataset_impl(dataset_path, trusted_input != 0, /*columns=*/nullptr, out_schema, out_batches,
                             out_batch_count, error_message, error_message_capacity);
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
    columns.reserve(column_count);
    for (size_t i = 0; i < column_count; ++i) {
        if (column_names[i] == nullptr) {
            set_error(error_message, error_message_capacity, "column_names contains a null entry");
            return NANO_LANCE_READER_INVALID_ARGUMENT;
        }
        columns.emplace_back(column_names[i]);
    }
    return read_dataset_impl(dataset_path, trusted_input != 0, &columns, out_schema, out_batches,
                             out_batch_count, error_message, error_message_capacity);
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
