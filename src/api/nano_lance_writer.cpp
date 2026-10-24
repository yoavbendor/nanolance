// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The writer's C API (nano_lance_writer.h): a NanoLanceWriter is a handle on a WriteSession
// (dataset/write_session.hpp), and each call forwards to it and copies its message into last_error.

#include "nanolance/nano_lance_writer.h"
#include "nanolance/writer_internal.hpp"

#include "dataset/write_session.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

using nano_lance::WriteSession;
using nano_lance::WriteStatus;

namespace {

WriteSession* session_of(NanoLanceWriter* writer) {
    return writer == nullptr ? nullptr : static_cast<WriteSession*>(writer->private_data);
}

const WriteSession* session_of(const NanoLanceWriter* writer) {
    return writer == nullptr ? nullptr : static_cast<const WriteSession*>(writer->private_data);
}

void set_last_error(NanoLanceWriter* writer, const std::string& message) {
    if (writer != nullptr) {
        const auto n = std::min(message.size(), sizeof(writer->last_error) - 1U);
        std::memcpy(writer->last_error, message.data(), n);
        writer->last_error[n] = '\0';
    }
}

/// A session call's result, its message copied into the handle.
int report(NanoLanceWriter* writer, WriteStatus status, const WriteSession& session) {
    set_last_error(writer, session.error());
    return static_cast<int>(status);
}

int not_initialized(NanoLanceWriter* writer) {
    set_last_error(writer, "writer is not initialized");
    return NANO_LANCE_INVALID_STATE;
}

}  // namespace

extern "C" {

void nano_lance_write_options_init(NanoLanceWriteOptions* options) {
    if (options != nullptr) {
        *options = NanoLanceWriteOptions{};
    }
}

int nano_lance_writer_open(NanoLanceWriter* writer, const char* path, const NanoLanceWriteOptions* options) {
    if (writer == nullptr) {
        return NANO_LANCE_INVALID_ARGUMENT;
    }
    if (writer->private_data != nullptr) {
        set_last_error(writer, "close writer before opening another dataset");
        return NANO_LANCE_INVALID_STATE;
    }
    set_last_error(writer, "");
    // A NULL options pointer and a zeroed struct mean the same thing, and both mean the defaults.
    const NanoLanceWriteOptions defaults{};
    const NanoLanceWriteOptions& opts = options != nullptr ? *options : defaults;
    if (opts.num_column_encodings != 0U && opts.column_encodings == nullptr) {
        set_last_error(writer, "column_encodings is NULL but num_column_encodings is not zero");
        return NANO_LANCE_INVALID_ARGUMENT;
    }
    nano_lance::WriteOptions session_options;
    session_options.compression_level = opts.compression_level;
    session_options.append = opts.append;
    session_options.compression = opts.compression;
    session_options.disable_structural_encoding = opts.disable_structural_encoding;
    session_options.blob_uri_dictionary = opts.blob_uri_dictionary;
    session_options.borrow_buffers = opts.borrow_buffers;
    session_options.max_pending_bytes = opts.max_pending_bytes;
    session_options.stage_fragments = opts.stage_fragments;
    for (std::size_t i = 0; i < opts.num_column_encodings; ++i) {
        const auto& entry = opts.column_encodings[i];
        session_options.column_encodings.emplace_back(entry.field_name != nullptr ? entry.field_name : "",
                                                      entry.encoding != nullptr ? entry.encoding : "");
    }
    auto session = std::make_unique<WriteSession>();
    const auto status = session->open(path != nullptr ? path : "", session_options);
    set_last_error(writer, session->error());
    if (status != WriteStatus::Ok) {
        return static_cast<int>(status);
    }
    writer->private_data = session.release();
    return NANO_LANCE_OK;
}

int nano_lance_writer_init(NanoLanceWriter* writer, const char* path, int compression_level) {
    if (writer == nullptr) {
        return NANO_LANCE_INVALID_ARGUMENT;
    }
    // Historically this reset private_data without looking at it, so a caller that re-inits without
    // closing leaks rather than being told. Kept, because changing it would break those callers.
    writer->private_data = nullptr;
    NanoLanceWriteOptions options{};
    options.compression_level = compression_level;
    return nano_lance_writer_open(writer, path, &options);
}

int nano_lance_writer_init_append(NanoLanceWriter* writer, const char* path, int compression_level) {
    NanoLanceWriteOptions options{};
    options.compression_level = compression_level;
    options.append = true;
    const auto rc = nano_lance_writer_open(writer, path, &options);
    if (rc == NANO_LANCE_INVALID_STATE && writer != nullptr && writer->private_data != nullptr) {
        set_last_error(writer, "close writer before init_append");
        return NANO_LANCE_INVALID_STATE;
    }
    return rc;
}

int nano_lance_writer_set_ignore_nullability(NanoLanceWriter* writer, bool ignore_nullability) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_ignore_nullability(ignore_nullability), *session);
}

int nano_lance_writer_set_blob_uri_dictionary(NanoLanceWriter* writer, bool enable) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_blob_uri_dictionary(enable), *session);
}

int nano_lance_writer_set_compression(NanoLanceWriter* writer, bool enable) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_compression(enable), *session);
}

int nano_lance_writer_set_structural_encoding(NanoLanceWriter* writer, bool enable) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_structural_encoding(enable), *session);
}

int nano_lance_writer_set_column_encoding(NanoLanceWriter* writer, const char* field_name,
                                          const char* encoding) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_column_encoding(field_name, encoding), *session);
}

int nano_lance_writer_set_max_pending_bytes(NanoLanceWriter* writer, uint64_t max_pending_bytes) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_max_pending_bytes(max_pending_bytes), *session);
}

int nano_lance_writer_set_borrow_buffers(NanoLanceWriter* writer, bool enable) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_borrow_buffers(enable), *session);
}

int nano_lance_write_batch(NanoLanceWriter* writer, struct ArrowArray* batch, struct ArrowSchema* schema) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->write_batch(batch, schema), *session);
}

int nano_lance_writer_commit(NanoLanceWriter* writer, bool is_append) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->commit(is_append), *session);
}

int nano_lance_writer_finish(NanoLanceWriter* writer, int mode, uint64_t* version_out) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->finish(mode, version_out), *session);
}

int nano_lance_writer_set_initial_config(NanoLanceWriter* writer, const char* key, const char* value) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_initial_config(key, value), *session);
}

int nano_lance_writer_add_global_buffer(NanoLanceWriter* writer, const uint8_t* data, size_t size) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->add_global_buffer(data, size), *session);
}

int nano_lance_writer_set_schema_metadata_entry(NanoLanceWriter* writer, const char* key, const uint8_t* value,
                                                size_t size) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_schema_metadata_entry(key, value, size), *session);
}

int nano_lance_writer_set_stable_row_ids(NanoLanceWriter* writer, int enable) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_stable_row_ids(enable), *session);
}

int nano_lance_writer_set_initial_bases(NanoLanceWriter* writer, size_t count, const char* const* paths,
                                        const char* const* names, const int* is_dataset_root) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_initial_bases(count, paths, names, is_dataset_root), *session);
}

int nano_lance_writer_set_fragment_targets(NanoLanceWriter* writer, size_t count, const char* const* dirs,
                                           const uint32_t* base_ids) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_fragment_targets(count, dirs, base_ids), *session);
}

int nano_lance_writer_set_blob_pack_file_size(NanoLanceWriter* writer, uint64_t bytes) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_blob_pack_file_size(bytes), *session);
}

int nano_lance_writer_set_transaction_property(NanoLanceWriter* writer, const char* key, const char* value) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        return not_initialized(writer);
    }
    return report(writer, session->set_transaction_property(key, value), *session);
}

int nano_lance_writer_close(NanoLanceWriter* writer) {
    if (writer == nullptr) {
        return NANO_LANCE_INVALID_ARGUMENT;
    }
    delete session_of(writer);
    writer->private_data = nullptr;
    set_last_error(writer, "");
    return NANO_LANCE_OK;
}

const char* nano_lance_writer_last_error(const NanoLanceWriter* writer) {
    if (writer == nullptr) {
        return "writer is null";
    }
    return writer->last_error;
}

uint64_t nano_lance_writer_pending_batches(const NanoLanceWriter* writer) {
    const auto* session = session_of(writer);
    return session == nullptr ? 0U : session->pending_batches();
}

uint64_t nano_lance_writer_pending_rows(const NanoLanceWriter* writer) {
    const auto* session = session_of(writer);
    return session == nullptr ? 0U : session->pending_rows();
}

uint64_t nano_lance_writer_pending_bytes(const NanoLanceWriter* writer) {
    const auto* session = session_of(writer);
    return session == nullptr ? 0U : session->pending_bytes();
}

}  // extern "C"

namespace nano_lance {

// The session behind a C handle, for the Python bindings' staged writes (writer_internal.hpp).

bool writer_set_field_id_base(NanoLanceWriter* writer, std::int32_t first_id, std::string& error) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        error = "the field id base must be set on a new writer before its first batch";
        return false;
    }
    return session->set_field_id_base(first_id, error);
}

bool writer_project_append(NanoLanceWriter* writer, const std::vector<std::string>& keep, std::string& error) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        error = "only a new append writer can write part of the schema";
        return false;
    }
    return session->project_append(keep, error);
}

bool writer_set_file_events(NanoLanceWriter* writer, std::function<void(int, const std::string&, std::uint64_t)> events,
                            std::string& error) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        error = "not a staged writer";
        return false;
    }
    return session->set_file_events(std::move(events), error);
}

bool writer_take_staged(NanoLanceWriter* writer, std::vector<NewFragment>& out, LanceSchemaMapping& mapping,
                        std::string& error, bool keep_empty) {
    auto* session = session_of(writer);
    if (session == nullptr) {
        error = "not a staged writer";
        return false;
    }
    const bool ok = session->take_staged(out, mapping, error, keep_empty);
    set_last_error(writer, session->error());
    return ok;
}

}  // namespace nano_lance
