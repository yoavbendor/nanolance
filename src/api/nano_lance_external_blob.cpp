// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The external-blob calls of the C API (nano_lance_reader.h), over io/external_blob.

#include "nanolance/nano_lance_reader.h"

#include "io/external_blob.hpp"

#include <cstring>
#include <string>

namespace {

int report(nano_lance::io::BlobStatus status, const std::string& error, char* message, size_t capacity) {
    if (message != nullptr && capacity > 0U) {
        std::strncpy(message, error.c_str(), capacity - 1U);
        message[capacity - 1U] = '\0';
    }
    switch (status) {
        case nano_lance::io::BlobStatus::Ok:
            return NANO_LANCE_READER_OK;
        case nano_lance::io::BlobStatus::InvalidArgument:
            return NANO_LANCE_READER_INVALID_ARGUMENT;
        case nano_lance::io::BlobStatus::Unsupported:
            return NANO_LANCE_READER_UNSUPPORTED;
        case nano_lance::io::BlobStatus::IoError:
            break;
    }
    return NANO_LANCE_READER_IO_ERROR;
}

}  // namespace

extern "C" {

int nano_lance_fetch_external_blob(const char* uri, uint64_t position, uint64_t size, uint8_t* out_buf, size_t out_cap,
                                   size_t* bytes_read, char* error_message, size_t error_message_capacity) {
    if (uri == nullptr || uri[0] == '\0' || out_buf == nullptr || out_cap == 0U || bytes_read == nullptr) {
        return report(nano_lance::io::BlobStatus::InvalidArgument, "invalid argument", error_message,
                      error_message_capacity);
    }
    std::string error;
    const auto status = nano_lance::io::fetch_external_blob(uri, position, size, out_buf, out_cap, *bytes_read, error);
    return report(status, error, error_message, error_message_capacity);
}

int nano_lance_external_blob_size(const char* uri, uint64_t* out_size, char* error_message,
                                  size_t error_message_capacity) {
    if (uri == nullptr || uri[0] == '\0' || out_size == nullptr) {
        return report(nano_lance::io::BlobStatus::InvalidArgument, "invalid argument", error_message,
                      error_message_capacity);
    }
    std::string error;
    const auto status = nano_lance::io::external_blob_size(uri, *out_size, error);
    return report(status, error, error_message, error_message_capacity);
}

int nano_lance_block_cache_configure(const char* cache_dir, int max_blocks, char* error_message,
                                     size_t error_message_capacity) {
    std::string error;
    const auto status = nano_lance::io::configure_block_cache(cache_dir, max_blocks, error);
    return report(status, error, error_message, error_message_capacity);
}

void nano_lance_block_cache_stats(uint64_t* out_hits, uint64_t* out_misses) {
    nano_lance::io::block_cache_stats(out_hits, out_misses);
}

}  // extern "C"
