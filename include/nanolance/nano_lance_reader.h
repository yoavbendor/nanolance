// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NanoLanceReaderMetadataEntry {
    char* key;
    uint8_t* value_bytes;
    size_t value_len;
} NanoLanceReaderMetadataEntry;

typedef struct NanoLanceReaderField {
    int32_t id;
    int32_t parent_id;
    int32_t type_field;
    int32_t encoding;
    bool nullable;
    char* name;
    char* logical_type;
    NanoLanceReaderMetadataEntry* metadata;
    size_t metadata_len;
} NanoLanceReaderField;

typedef struct NanoLanceReaderDataFile {
    char* path;
    uint64_t file_size_bytes;
    uint32_t file_major_version;
    uint32_t file_minor_version;
    int32_t* field_ids;
    size_t field_ids_len;
    int32_t* column_indices;
    size_t column_indices_len;
} NanoLanceReaderDataFile;

typedef struct NanoLanceReaderFragment {
    uint64_t id;
    uint64_t physical_rows;
    NanoLanceReaderDataFile* files;
    size_t files_len;
} NanoLanceReaderFragment;

typedef struct NanoLanceDatasetMetadata {
    uint64_t manifest_version;
    uint64_t total_physical_rows;
    char* file_format;
    char* format_version;
    bool has_max_fragment_id;
    uint32_t max_fragment_id;
    NanoLanceReaderField* fields;
    size_t fields_len;
    NanoLanceReaderFragment* fragments;
    size_t fragments_len;
} NanoLanceDatasetMetadata;

enum NanoLanceReaderStatus {
    NANO_LANCE_READER_OK = 0,
    NANO_LANCE_READER_INVALID_ARGUMENT = 1,
    NANO_LANCE_READER_IO_ERROR = 2,
    NANO_LANCE_READER_PARSE_ERROR = 3,
    /// The requested capability is not available in this build (e.g. the disk block cache when S3 is
    /// disabled, the AWS-SDK seam is used, or the linked nanos3reader predates it). Non-fatal: callers
    /// should continue without the feature.
    NANO_LANCE_READER_UNSUPPORTED = 4
};

/// Load latest `_versions/*.manifest` for a Lance dataset directory. On success, caller must
/// `nano_lance_dataset_metadata_free` exactly once.
int nano_lance_dataset_read_latest(const char* dataset_path, NanoLanceDatasetMetadata* out, char* error_message,
                                   size_t error_message_capacity);

void nano_lance_dataset_metadata_free(NanoLanceDatasetMetadata* metadata);

/// Read bytes from an external blob URI (`file://` always; `s3://` when built with S3 support).
/// \p size may be UINT64_MAX to read until EOF (capped by \p out_cap). On success returns NANO_LANCE_READER_OK and
/// sets \p bytes_read.
int nano_lance_fetch_external_blob(const char* uri, uint64_t position, uint64_t size, uint8_t* out_buf, size_t out_cap,
                                   size_t* bytes_read, char* error_message, size_t error_message_capacity);

/// Configure the optional on-disk LRU block cache used for `s3://` reads (no effect on `file://`). Call once
/// before the first fetch. \p max_blocks is the LRU capacity (the reader clamps it to 2..500); <= 0 disables
/// the cache. Each block is one 32 MiB read-ahead window persisted under \p cache_dir.
///
/// Returns NANO_LANCE_READER_OK when the cache is active. Returns NANO_LANCE_READER_UNSUPPORTED (non-fatal —
/// continue without a cache) when this build can't provide it: no S3 support, the AWS-SDK seam, or a
/// nanos3reader older than 0.2.0. Returns NANO_LANCE_READER_IO_ERROR if \p cache_dir can't be created.
/// \p error_message is set on any non-OK return.
int nano_lance_block_cache_configure(const char* cache_dir, int max_blocks, char* error_message,
                                     size_t error_message_capacity);

/// Cumulative block-cache hit/miss counters since process start (both 0 when no cache is active). Pass
/// nullptr to skip either output.
void nano_lance_block_cache_stats(uint64_t* out_hits, uint64_t* out_misses);

struct ArrowSchema;
struct ArrowArray;

/// Read all batches from a nano_lance_writer dataset into nanoarrow arrays (writer parity only).
/// On success, \p out_schema and \p out_batches are initialized; release with `nano_lance_table_read_result_free`.
/// Requires linking `nano_lance_writer` (implementation lives there).
int nano_lance_table_read_dataset(const char* dataset_path, struct ArrowSchema* out_schema,
                                  struct ArrowArray** out_batches, size_t* out_batch_count, char* error_message,
                                  size_t error_message_capacity);

/// Same as nano_lance_table_read_dataset, with an explicit trusted_input flag. Pass 0 for the default,
/// fully-checked read (identical to nano_lance_table_read_dataset). Pass nonzero to opt a known-good,
/// self-produced dataset out of the untrusted-input DoS/OOM budget checks (declared zstd size,
/// row/column/manifest-element counts) — every bounds check (offset/size validated against the real
/// buffer with overflow-safe arithmetic) still runs unconditionally either way. Never pass nonzero for a
/// dataset that could have come from another party.
int nano_lance_table_read_dataset_ex(const char* dataset_path, int trusted_input, struct ArrowSchema* out_schema,
                                     struct ArrowArray** out_batches, size_t* out_batch_count, char* error_message,
                                     size_t error_message_capacity);

/// Same as nano_lance_table_read_dataset_ex, but decodes only the named top-level columns (and their
/// children); every other column is skipped rather than decoded and thrown away. For a wide table
/// read for a few columns, the skipped decode IS the cost -- column materialization dominates the
/// read profile.
///
/// \p column_names holds \p column_count NUL-terminated names, none of them null, and must name at
/// least one column. An unknown name is an error rather than a silent empty column.
///
/// Ownership and the failure contract are exactly nano_lance_table_read_dataset's: release with
/// `nano_lance_table_read_result_free`, and on failure \p out_schema is left RELEASED.
int nano_lance_table_read_dataset_projected(const char* dataset_path, const char* const* column_names,
                                            size_t column_count, int trusted_input,
                                            struct ArrowSchema* out_schema, struct ArrowArray** out_batches,
                                            size_t* out_batch_count, char* error_message,
                                            size_t error_message_capacity);

/// Open a dataset as a streaming Arrow reader: one batch per data file, decoded on demand, instead
/// of every batch materialized before the caller sees any of them.
///
/// This is what makes a larger-than-memory dataset readable, and it is what gets you a first batch
/// without waiting for the last. It is NOT a way to halve peak memory on an ordinary single-fragment
/// dataset -- there is only one batch there either way.
///
/// \p column_names / \p column_count project, exactly as
/// nano_lance_table_read_dataset_projected does; pass NULL / 0 to read every column.
///
/// On success \p out_stream is a valid ArrowArrayStream that the CALLER releases
/// (`out_stream->release(out_stream)`), which also closes the dataset. On failure it is left zeroed,
/// so releasing it is unnecessary and calling through it is not possible.
int nano_lance_table_open_stream(const char* dataset_path, const char* const* column_names,
                                 size_t column_count, int trusted_input,
                                 struct ArrowArrayStream* out_stream, char* error_message,
                                 size_t error_message_capacity);

/// Read rows [offset, offset + length) -- a negative \p length means "to the end of the dataset".
///
/// \p column_names / \p column_count project; NULL/0 reads every column. (That differs from
/// nano_lance_table_read_dataset_projected, which refuses a zero-column projection: there an empty
/// list is a caller mistake, here it is the ordinary "no projection" case.)
///
/// Fragments the range does not touch are never opened, so the I/O saved is proportional to the
/// fragments skipped rather than to the rows dropped. The row semantics are exact either way.
int nano_lance_table_read_dataset_range(const char* dataset_path, const char* const* column_names,
                                        size_t column_count, uint64_t offset, int64_t length,
                                        int trusted_input, struct ArrowSchema* out_schema,
                                        struct ArrowArray** out_batches, size_t* out_batch_count,
                                        char* error_message, size_t error_message_capacity);

/// Read the rows at \p indices (\p index_count of them): logical row numbers, deleted rows not
/// counted -- random access, e.g. a shuffled training mini-batch. \p column_names / \p column_count
/// project as in nano_lance_table_read_dataset_range.
///
/// Rows come back in ascending order, each once, one batch per fragment touched. Only fragments and
/// pages holding a requested row are read, and for large values (FullZip pages) only the rows
/// themselves. An index past the end is an error. Free the
/// result with nano_lance_table_read_result_free.
int nano_lance_table_take(const char* dataset_path, const char* const* column_names, size_t column_count,
                          const uint64_t* indices, size_t index_count, int trusted_input,
                          struct ArrowSchema* out_schema, struct ArrowArray** out_batches,
                          size_t* out_batch_count, char* error_message, size_t error_message_capacity);

/// nano_lance_table_open_stream restricted to a row range; see
/// nano_lance_table_read_dataset_range for what a range costs.
int nano_lance_table_open_stream_range(const char* dataset_path, const char* const* column_names,
                                       size_t column_count, uint64_t offset, int64_t length,
                                       int trusted_input, struct ArrowArrayStream* out_stream,
                                       char* error_message, size_t error_message_capacity);

/// The dataset's Arrow schema, from the manifest alone -- no data file is opened, so this is the
/// cheap way to ask what columns a dataset has. The CALLER releases \p out_schema on success; on
/// failure it is left zeroed and must not be released.
int nano_lance_table_read_schema(const char* dataset_path, struct ArrowSchema* out_schema,
                                 char* error_message, size_t error_message_capacity);

/// The dataset's row count, summed from the manifest's fragments. O(fragments), not O(rows).
int nano_lance_table_count_rows(const char* dataset_path, uint64_t* out_rows, char* error_message,
                                size_t error_message_capacity);

void nano_lance_table_read_result_free(struct ArrowSchema* schema, struct ArrowArray* batches, size_t batch_count);

#ifdef __cplusplus
}
#endif
