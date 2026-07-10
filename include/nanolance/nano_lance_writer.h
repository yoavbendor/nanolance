// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ArrowArray;
struct ArrowSchema;

enum NanoLanceStatus {
    NANO_LANCE_OK = 0,
    NANO_LANCE_INVALID_ARGUMENT = 1,
    NANO_LANCE_INVALID_STATE = 2,
    NANO_LANCE_IO_ERROR = 3,
    NANO_LANCE_UNSUPPORTED = 4
};

typedef struct NanoLanceWriter {
    void* private_data;
    char last_error[512];
} NanoLanceWriter;

int nano_lance_writer_init(NanoLanceWriter* writer, const char* path, int compression_level);
/// Open an existing dataset for more fragments (reloads schema from latest manifest; commits must use `is_append=true`).
int nano_lance_writer_init_append(NanoLanceWriter* writer, const char* path, int compression_level);
int nano_lance_writer_set_ignore_nullability(NanoLanceWriter* writer, bool ignore_nullability);
/// Opt in to URI-dictionary encoding for `lance.blob.v2` external columns: store each distinct URI
/// once and reference it per row by index. Much smaller when many rows point at one object, but the
/// result is a nanolance-only layout (stock Lance/lance-c cannot read those blob columns). Must be
/// set before any batch is written, and is only supported for create-mode datasets (not append).
int nano_lance_writer_set_blob_uri_dictionary(NanoLanceWriter* writer, bool enable);
/// Opt in to Lance-compatible zstd compression of variable-width (string/binary) columns. Each
/// chunk's value bytes are stored as [uint64 LE uncompressed size][zstd frame] and the column's
/// PageLayout advertises General(ZSTD) so stock Lance can still read it. Off by default; must be set
/// before any batch is written. The zstd level is the writer's compression_level (0 = zstd default).
/// This controls ONLY zstd; the structural encodings below apply independently (see
/// nano_lance_writer_set_structural_encoding).
int nano_lance_writer_set_compression(NanoLanceWriter* writer, bool enable);
/// Enable/disable structural (lossless) re-encodings: integer bitpacking, ConstantLayout, RLE, and
/// string dictionary / dictionary+RLE. These shrink files and usually speed up writes with no CPU cost
/// like zstd, and stay stock-Lance-readable, so they are ON by default and independent of
/// set_compression (zstd). Disable to emit plain flat/variable-width pages ("raw" Lance output). Must
/// be set before any batch is written.
int nano_lance_writer_set_structural_encoding(NanoLanceWriter* writer, bool enable);
/// Declare a column's encoding up front, skipping the commit-time detection scans for it entirely
/// (constant / RLE / dictionary detection are data scans over the whole column; a caller that already
/// knows a column's shape can save that work). `encoding` is one of:
///   "auto"     -- default: detect as usual.
///   "plain"    -- flat pages, no structural encoding and no zstd for this column.
///   "bitpack"  -- FastLanes InlineBitpacking; integer columns (int/uint 8..64) only.
///   "bss-zstd" -- byte-stream-split + zstd; float/double columns only (implies zstd for this column
///                 even if set_compression is off).
///   "zstd"     -- variable-width (string/binary) columns: skip dictionary/constant detection and
///                 zstd the pages (requires set_compression(true) to actually compress).
/// All hinted encodings remain stock-Lance-readable and are valid for ANY data of the right type --
/// hints can cost size (e.g. bitpacking a constant column) but never correctness. Type compatibility
/// is validated at commit. Must be called before any batch is written.
int nano_lance_writer_set_column_encoding(NanoLanceWriter* writer, const char* field_name,
                                          const char* encoding);
/// Opt in to borrowing the caller's Arrow buffers instead of copying them at write_batch time, for
/// fixed-width columns (except bool). Contract: every buffer passed to write_batch must remain valid
/// and unmodified until commit/close. A column written in a single batch is encoded straight from the
/// caller's memory (zero-copy ingest); if a second batch arrives for a column, that column silently
/// falls back to the copying path (correct, just not zero-copy). Variable-width and bool columns
/// always copy. Off by default; must be set before any batch is written.
int nano_lance_writer_set_borrow_buffers(NanoLanceWriter* writer, bool enable);
int nano_lance_write_batch(NanoLanceWriter* writer, struct ArrowArray* batch, struct ArrowSchema* schema);
int nano_lance_writer_commit(NanoLanceWriter* writer, bool is_append);
int nano_lance_writer_close(NanoLanceWriter* writer);

const char* nano_lance_writer_last_error(const NanoLanceWriter* writer);
uint64_t nano_lance_writer_pending_batches(const NanoLanceWriter* writer);

#ifdef __cplusplus
}
#endif
