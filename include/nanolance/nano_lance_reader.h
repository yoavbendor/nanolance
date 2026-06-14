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
    NANO_LANCE_READER_PARSE_ERROR = 3
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

struct ArrowSchema;
struct ArrowArray;

/// Read all batches from a nano_lance_writer dataset into nanoarrow arrays (writer parity only).
/// On success, \p out_schema and \p out_batches are initialized; release with `nano_lance_table_read_result_free`.
/// Requires linking `nano_lance_writer` (implementation lives there).
int nano_lance_table_read_dataset(const char* dataset_path, struct ArrowSchema* out_schema,
                                  struct ArrowArray** out_batches, size_t* out_batch_count, char* error_message,
                                  size_t error_message_capacity);

void nano_lance_table_read_result_free(struct ArrowSchema* schema, struct ArrowArray* batches, size_t batch_count);

#ifdef __cplusplus
}
#endif
