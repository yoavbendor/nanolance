// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nano_lance {

/// A half-open row range: rows `[offset, offset + length)`.
///
/// What this buys is honest but specific: fragments the range does not touch are **never opened**,
/// so the I/O saved is proportional to the fragments skipped, not to the rows dropped. The row
/// semantics are exact -- you get precisely the rows you asked for -- but a range inside a single
/// fragment still decodes that whole fragment. Datasets written with `max_rows_per_fragment` get the
/// full benefit; a one-fragment dataset gets none.
///
/// A range that runs past the end is clamped to the end; an `offset` past the end is an error rather
/// than an empty result, since it almost always means the caller's arithmetic is wrong.
struct LanceRowRange {
    static constexpr std::uint64_t kAllRows = ~std::uint64_t{0};

    std::uint64_t offset = 0;
    std::uint64_t length = kAllRows;

    bool is_whole_dataset() const { return offset == 0U && length == kAllRows; }
};


/// Read all committed rows from a nano_lance_writer dataset into Arrow batches (writer parity only).
/// Rebuilds ingest-shaped schemas (e.g. dematerialized `lance.blob.v2` children).
///
/// \p trusted_input (default false) opts a self-produced pipeline out of the untrusted-input DoS/OOM
/// budget checks (declared zstd size, row/column/manifest-element counts vs. `ReadLimits`) — set it only
/// when the dataset is known-good (e.g. round-tripping your own writer output), never for a file that
/// could originate from another party. It does NOT disable any bounds check: every offset/size is still
/// validated against the real buffer with overflow-safe arithmetic before use, exactly as in the default
/// path. Because those budget checks already cost nothing measurable (validated once per page/header,
/// not per value — see docs/SAFETY.md), this is an escape hatch for exotic legitimate files that exceed
/// the default budget, not a speed option.
/// On failure `out_schema` is left RELEASED and `out_batches` empty -- the caller must not release
/// either again. (Releasing an ArrowSchema twice jumps through a null `release` pointer; the C shim
/// used to do exactly that, so every failed read through the C API or the Python bindings segfaulted
/// instead of reporting an error.)
bool lance_table_read_dataset(const std::filesystem::path& dataset_path, ArrowSchema& out_schema,
                              std::vector<ArrowArray>& out_batches, std::string& error,
                              bool trusted_input = false);

/// Same as lance_table_read_dataset but only reads the named top-level columns (and their children).
/// All other columns are skipped during decode — significantly faster when the table has many columns
/// and only a small subset is needed (e.g. fuselance only needs filename + blob columns).
bool lance_table_read_dataset_projected(const std::filesystem::path& dataset_path,
                                        const std::vector<std::string>& column_names,
                                        ArrowSchema& out_schema,
                                        std::vector<ArrowArray>& out_batches,
                                        std::string& error, bool trusted_input = false);

/// Read rows `[range.offset, range.offset + range.length)`, optionally projected.
/// `column_names` may be null to read every column. See `LanceRowRange` for what a range costs.
bool lance_table_read_dataset_range(const std::filesystem::path& dataset_path,
                                    const std::vector<std::string>* column_names,
                                    const LanceRowRange& range, ArrowSchema& out_schema,
                                    std::vector<ArrowArray>& out_batches, std::string& error,
                                    bool trusted_input = false);

/// The dataset's Arrow schema, read from the manifest alone -- no data file is opened.
///
/// This is the cheap "what is in here?" call: opening a dataset to look at its columns should not
/// cost a decode. On failure `out_schema` is left released, as the read functions leave it.
bool lance_table_read_schema(const std::filesystem::path& dataset_path, ArrowSchema& out_schema,
                             std::string& error);

/// The dataset's row count, summed from the manifest's fragments. O(fragments), not O(rows): no data
/// file is opened, so this stays constant-time as the dataset grows.
bool lance_table_count_rows(const std::filesystem::path& dataset_path, std::uint64_t& out_rows,
                            std::string& error);

/// A fragment-at-a-time reader: the same decode as lance_table_read_dataset, but one data file per
/// `next()` instead of all of them before you get anything.
///
/// What this buys, stated precisely because the plan originally overstated it: **larger-than-memory
/// datasets and time-to-first-batch**. It does NOT halve peak memory on an ordinary single-fragment
/// dataset -- there is only one batch there either way. (The 2x peak that used to cost was inside a
/// single fragment's decode and is gone independently; see docs/PROGRESS.md.)
///
/// Usage:
///
///     nano_lance::LanceTableStream stream;
///     ArrowSchema schema{};
///     if (!nano_lance::LanceTableStream::open(path, nullptr, schema, stream, error)) { ... }
///     for (;;) {
///         ArrowArray batch{};
///         if (!stream.next(batch, error)) { ... }   // false == failure
///         if (batch.release == nullptr) { break; }  // end of stream
///         ...
///         ArrowArrayRelease(&batch);
///     }
///     ArrowSchemaRelease(&schema);
class LanceTableStream {
public:
    LanceTableStream();
    ~LanceTableStream();
    LanceTableStream(LanceTableStream&&) noexcept;
    LanceTableStream& operator=(LanceTableStream&&) noexcept;
    LanceTableStream(const LanceTableStream&) = delete;
    LanceTableStream& operator=(const LanceTableStream&) = delete;

    /// Open `dataset_path` and produce its Arrow schema. `column_names` is null to read every column,
    /// or names the top-level columns to project (the rest are skipped during decode).
    ///
    /// On failure `out_schema` is left RELEASED, exactly as lance_table_read_dataset leaves it, and
    /// `out` is left unopened -- calling next() on it reports "stream is not open" rather than
    /// crashing. The caller owns `out_schema` and may release it as soon as this returns; the stream
    /// keeps its own copy.
    static bool open(const std::filesystem::path& dataset_path,
                     const std::vector<std::string>* column_names, ArrowSchema& out_schema,
                     LanceTableStream& out, std::string& error, bool trusted_input = false);

    /// As `open`, restricted to `range`. Fragments outside it are never opened.
    static bool open_range(const std::filesystem::path& dataset_path,
                           const std::vector<std::string>* column_names, const LanceRowRange& range,
                           ArrowSchema& out_schema, LanceTableStream& out, std::string& error,
                           bool trusted_input = false);

    /// Decode the next data file. Returns false on failure; on success with no data left,
    /// `out_batch.release` is null. The caller owns each batch it receives.
    bool next(ArrowArray& out_batch, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nano_lance
