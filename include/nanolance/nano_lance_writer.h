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

/// One column's declared encoding, for NanoLanceWriteOptions::column_encodings.
/// `encoding` takes the same values as nano_lance_writer_set_column_encoding.
typedef struct NanoLanceColumnEncoding {
    const char* field_name;
    const char* encoding;
} NanoLanceColumnEncoding;

/// Everything a writer needs to know before its first batch, in one place.
///
/// The setters below each have to be called after init and before write_batch, and each returns
/// INVALID_STATE when it is not -- an ordering rule that can only be documented and checked at
/// runtime. Passing this struct to nano_lance_writer_open makes that ordering unrepresentable
/// instead: there is no "after" for the options to be in.
///
/// **A zero-initialized struct means the defaults.** `NanoLanceWriteOptions options = {0};` is the
/// same writer that `nano_lance_writer_init(w, path, 0)` gives you, and it stays that way as fields
/// are added. That is why the one option that is ON by default is spelled as a negation:
/// `disable_structural_encoding` rather than `structural_encoding`. `nano_lance_write_options_init`
/// does the same thing for callers who would rather say it out loud.
typedef struct NanoLanceWriteOptions {
    /// zstd level 0..22; 0 means zstd's own default. Applies to whatever compression is enabled.
    int compression_level;
    /// Open an existing dataset for more fragments instead of creating one. The schema is reloaded
    /// from the latest manifest, and commits must pass is_append=true.
    bool append;
    /// zstd-compress variable-width (string/binary) column pages. See set_compression.
    bool compression;
    /// Turn OFF structural re-encodings (bitpacking, constant, RLE, dictionary), which are otherwise
    /// on. Negated so that a zeroed struct keeps the default. See set_structural_encoding.
    bool disable_structural_encoding;
    /// URI-dictionary encoding for `lance.blob.v2` external columns. nanolance-only layout; create
    /// mode only. See set_blob_uri_dictionary.
    bool blob_uri_dictionary;
    /// Borrow the caller's fixed-width Arrow buffers until commit instead of copying them. The
    /// buffers must stay valid and unmodified until commit/close. See set_borrow_buffers.
    bool borrow_buffers;
    /// Per-column encoding declarations; may be NULL when count is 0. The array and the strings it
    /// points at are read during this call only, and need not outlive it.
    const NanoLanceColumnEncoding* column_encodings;
    size_t num_column_encodings;
    /// Memory budget, in bytes, for rows buffered between commits; 0 = unlimited (the default). See
    /// set_max_pending_bytes.
    uint64_t max_pending_bytes;
    /// Stage fragments instead of publishing a version per commit: each commit (and each flush by
    /// max_pending_bytes) writes a data file, and nano_lance_writer_finish publishes all of them as
    /// one version -- how a Lance write behaves. Readers see nothing until then.
    bool stage_fragments;
} NanoLanceWriteOptions;

/// How nano_lance_writer_finish relates the new version to what is at the path.
enum {
    NANO_LANCE_COMMIT_CREATE = 0,    // fail if a dataset exists there
    NANO_LANCE_COMMIT_APPEND = 1,    // add the fragments to the latest version
    NANO_LANCE_COMMIT_OVERWRITE = 2  // a new version holding only these fragments (or create one)
};

/// Fill `options` with the defaults. Equivalent to zero-initializing it.
void nano_lance_write_options_init(NanoLanceWriteOptions* options);

/// Open a writer with all of its options at once. `options` may be NULL, meaning the defaults.
///
/// This is the entry point to reach for; `nano_lance_writer_init`, `nano_lance_writer_init_append`
/// and the `set_*` calls below are the older, order-dependent spelling of the same thing and now
/// delegate here.
int nano_lance_writer_open(NanoLanceWriter* writer, const char* path, const NanoLanceWriteOptions* options);

int nano_lance_writer_init(NanoLanceWriter* writer, const char* path, int compression_level);
/// Open an existing dataset for more fragments (reloads schema from latest manifest; commits must use `is_append=true`).
int nano_lance_writer_init_append(NanoLanceWriter* writer, const char* path, int compression_level);
/// Deprecated no-op, kept so existing callers still link and behave identically.
///
/// Fields whose Arrow schema sets ARROW_FLAG_NULLABLE are now accepted unconditionally. Gating on
/// the flag rejected essentially every real table (pyarrow marks all fields nullable) while
/// protecting nothing: what needed guarding is a null *value*, not a nullable *flag*.
///
/// A batch that actually contains a null is refused at write_batch, with a message naming the column
/// and row. nanolance writes no Lance validity information, so a null slot has nowhere to go; it used
/// to be written as the slot's raw bytes, silently turning [10, null, 30] into [10, 0, 30] -- and
/// stock Lance read those wrong values back without complaint. Fill or drop nulls before writing.
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
/// Bound the memory the writer holds for uncommitted rows -- for edge devices that must keep their
/// resident set small while saving. The writer buffers every batch until a commit, so without a
/// budget a session's whole data is in memory at once. With one, write_batch commits a fragment
/// itself whenever the buffered data reaches `max_pending_bytes`, then frees it.
///
///   * Plan for a peak of about 3-4x the budget, plus one batch (a batch is never split): buffers
///     grow by doubling, so the batch that crosses the budget can leave up to twice it allocated,
///     and encoding a fragment needs working space of its own. Measured: a 4 MiB budget peaked at
///     17 MiB of RSS writing 52 MiB (unbounded: 145 MiB); 2 MiB at 7.6 MiB, 8 MiB at 15.4 MiB
///     (tests/test_writer_memory.cpp, docs/PROGRESS.md). Borrowed buffers (set_borrow_buffers)
///     count toward the budget: they stay pinned until the flush.
///   * Each flush is a commit: one data file and one dataset version. A tight budget means more,
///     smaller fragments, which costs a little on read and a manifest write per flush. A few MiB to
///     tens of MiB is a reasonable range; far below the size of one batch it just flushes every batch.
///   * The caller's own nano_lance_writer_commit is unchanged: after write_batch has flushed, it
///     appends whatever is still pending (whatever `is_append` says) and is a no-op if nothing is.
///   * 0 turns it off. May be changed at any time. Not combinable with blob URI dictionary mode,
///     whose layout cannot be appended to.
int nano_lance_writer_set_max_pending_bytes(NanoLanceWriter* writer, uint64_t max_pending_bytes);
int nano_lance_write_batch(NanoLanceWriter* writer, struct ArrowArray* batch, struct ArrowSchema* schema);
int nano_lance_writer_commit(NanoLanceWriter* writer, bool is_append);
/// Staged writers only: commit what is pending, then publish every staged fragment as one version.
/// A write with no rows publishes a version with no fragments (the schema of the batches seen, or of
/// the dataset appended to). `version_out` may be NULL.
int nano_lance_writer_finish(NanoLanceWriter* writer, int mode, uint64_t* version_out);
int nano_lance_writer_close(NanoLanceWriter* writer);

const char* nano_lance_writer_last_error(const NanoLanceWriter* writer);
uint64_t nano_lance_writer_pending_batches(const NanoLanceWriter* writer);
/// Rows buffered and not yet committed.
uint64_t nano_lance_writer_pending_rows(const NanoLanceWriter* writer);
/// Bytes the writer holds for uncommitted rows, as max_pending_bytes counts them.
uint64_t nano_lance_writer_pending_bytes(const NanoLanceWriter* writer);

#ifdef __cplusplus
}
#endif
