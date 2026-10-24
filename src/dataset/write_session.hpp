// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/manifest_writer.hpp"
#include "nanolance/schema_mapper.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct ArrowArray;
struct ArrowSchema;

namespace nano_lance {

/// How a write-session call ended. The C API (nano_lance_writer.h) returns these values as its codes.
enum class WriteStatus : int { Ok = 0, InvalidArgument = 1, InvalidState = 2, IoError = 3, Unsupported = 4 };

/// How finish relates the new version to what is at the path (NANO_LANCE_COMMIT_* in the C API).
constexpr int kCommitCreate = 0;     // fail if a dataset exists there
constexpr int kCommitAppend = 1;     // add the fragments to the latest version
constexpr int kCommitOverwrite = 2;  // a new version holding only these fragments (or create one)

/// The options a session opens with (NanoLanceWriteOptions, without the C types).
struct WriteOptions {
    int compression_level = 0;
    bool append = false;
    bool compression = false;
    bool disable_structural_encoding = false;
    bool blob_uri_dictionary = false;
    bool borrow_buffers = false;
    /// Per field: "auto", "plain", "bitpack", "bss-zstd" or "zstd" (set_column_encoding's values).
    std::vector<std::pair<std::string, std::string>> column_encodings;
    std::uint64_t max_pending_bytes = 0;
    bool stage_fragments = false;
};

/// One dataset write: batches are buffered and committed as fragments, each commit a version (or, staged,
/// all of them at finish). The C API's NanoLanceWriter is a handle on one of these; nanolance's own
/// operations (update, merge_insert, add_columns) use it directly. Every call that fails leaves its
/// message in error(); one that succeeds clears it, as the C API's last_error.
class WriteSession {
public:
    struct State;
    WriteSession();
    ~WriteSession();
    WriteSession(const WriteSession&) = delete;
    WriteSession& operator=(const WriteSession&) = delete;

    /// Open the dataset at `path` (a local path or a registered object store) for a new write.
    WriteStatus open(const std::string& path, const WriteOptions& opts);
    WriteStatus set_ignore_nullability(bool ignore_nullability);
    WriteStatus set_blob_uri_dictionary(bool enable);
    WriteStatus set_compression(bool enable);
    WriteStatus set_structural_encoding(bool enable);
    WriteStatus set_column_encoding(const char* field_name,
                                          const char* encoding);
    WriteStatus set_max_pending_bytes(uint64_t max_pending_bytes);
    WriteStatus set_borrow_buffers(bool enable);
    WriteStatus write_batch(struct ArrowArray* batch, struct ArrowSchema* schema);
    WriteStatus commit(bool is_append);
    WriteStatus finish(int mode, uint64_t* version_out);
    WriteStatus set_initial_config(const char* key, const char* value);
    WriteStatus add_global_buffer(const uint8_t* data, size_t size);
    WriteStatus set_schema_metadata_entry(const char* key, const uint8_t* value,
                                                size_t size);
    WriteStatus set_stable_row_ids(int enable);
    WriteStatus set_initial_bases(size_t count, const char* const* paths,
                                        const char* const* names, const int* is_dataset_root);
    WriteStatus set_fragment_targets(size_t count, const char* const* dirs,
                                           const uint32_t* base_ids);
    WriteStatus set_blob_pack_file_size(uint64_t bytes);
    WriteStatus set_transaction_property(const char* key, const char* value);

    std::uint64_t pending_batches() const;
    std::uint64_t pending_rows() const;
    /// Bytes held for rows not yet committed (every buffer's capacity, borrowed caller buffers too).
    std::uint64_t pending_bytes() const;
    const std::string& error() const { return error_; }

    // Staged writes (stage_fragments), for operations that commit their own manifest.
    bool set_field_id_base(std::int32_t first_id, std::string& error);
    bool project_append(const std::vector<std::string>& keep, std::string& error);
    bool set_file_events(std::function<void(int, const std::string&, std::uint64_t)> events,
                            std::string& error);
    bool take_staged(std::vector<NewFragment>& out, LanceSchemaMapping& mapping,
                        std::string& error, bool keep_empty = false);

private:
    WriteStatus fail(WriteStatus status, const std::string& message) {
        error_ = message;
        return status;
    }
    /// Commit what is pending as one fragment.
    WriteStatus commit_pending(bool is_append);

    std::unique_ptr<State> state_;
    std::string error_;
};

}  // namespace nano_lance
