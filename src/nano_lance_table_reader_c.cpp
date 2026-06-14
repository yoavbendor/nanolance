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

}  // namespace

extern "C" int nano_lance_table_read_dataset(const char* dataset_path, struct ArrowSchema* out_schema,
                                             struct ArrowArray** out_batches, size_t* out_batch_count,
                                             char* error_message, size_t error_message_capacity) {
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
    if (!nano_lance::lance_table_read_dataset(std::filesystem::path(dataset_path), *out_schema, batches, error)) {
        ArrowSchemaRelease(out_schema);
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
