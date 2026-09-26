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

/// What a read asks of a dataset, beyond its path: the version, the columns, the fragments, the rows,
/// and Lance's row identity columns. A default request is a full read of the latest version.
struct LanceScanRequest {
    /// Top-level columns to read (their children come along). Null reads every column; an empty list
    /// reads none, which is only useful with a row id column.
    const std::vector<std::string>* columns = nullptr;
    /// Logical rows (deleted rows not counted) over the fragments read.
    LanceRowRange range;
    /// The version to read; the latest when `has_version` is false.
    bool has_version = false;
    std::uint64_t version = 0;
    /// Fragments to read, in this order. Null reads every fragment, by id.
    const std::vector<std::uint64_t>* fragment_ids = nullptr;
    /// Add `_rowid` / `_rowaddr` (uint64) after the data columns. A row's address is its fragment id
    /// in the high 32 bits and its offset in the fragment (deleted rows counted) in the low; without
    /// stable row ids -- all nanolance writes -- the row id is the address.
    bool with_row_id = false;
    bool with_row_address = false;
    /// An SQL filter (nanolance/expr.hpp): only rows for which it is TRUE are read. Its columns are
    /// decoded whether or not they are returned. With a filter, `range` counts the rows that pass.
    const std::string* filter = nullptr;
    /// Read the rows of the fragments' deletion files too (every physical row, in file order): what an
    /// operation that writes a new column for existing fragments needs.
    bool include_deleted_rows = false;
};

/// A read as `request` describes it. See lance_table_read_dataset for the ownership rules.
bool lance_dataset_scan(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                        ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches, std::string& error,
                        bool trusted_input = false);

/// lance_table_take with the version, projection and row id columns of `request` (its range is ignored).
bool lance_dataset_take(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                        const std::vector<std::uint64_t>& indices, ArrowSchema& out_schema,
                        std::vector<ArrowArray>& out_batches, std::string& error, bool trusted_input = false);

/// The rows at row `addresses` (see LanceScanRequest::with_row_address): ascending, each once, one
/// batch per fragment. A deleted row is still addressable.
bool lance_dataset_take_rows(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                             const std::vector<std::uint64_t>& addresses, ArrowSchema& out_schema,
                             std::vector<ArrowArray>& out_batches, std::string& error,
                             bool trusted_input = false);

/// A standalone Lance data file (what pylance's LanceFileWriter writes, or one of a dataset's files).
struct LanceFileInfo {
    std::uint64_t num_rows = 0;
    std::uint32_t num_columns = 0;
    /// Per column, per page: its rows and the (offset, size) of each of its buffers.
    struct Page {
        std::uint64_t rows = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> buffers;
        std::string encoding;  // the page layout, described
    };
    std::vector<std::vector<Page>> pages;
};

/// Read a standalone data file, as `request` asks (its columns and range; the version, fragment and
/// row id fields do not apply). The file's own schema is the schema.
bool lance_file_read(const std::filesystem::path& file_path, const LanceScanRequest& request,
                     ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches, std::string& error);

/// The rows of a standalone data file at `rows` (ascending, each once; one batch).
bool lance_file_take(const std::filesystem::path& file_path, const LanceScanRequest& request,
                     const std::vector<std::uint64_t>& rows, ArrowSchema& out_schema,
                     std::vector<ArrowArray>& out_batches, std::string& error);

/// A standalone data file's row count and schema.
bool lance_file_info(const std::filesystem::path& file_path, LanceFileInfo& info, ArrowSchema& out_schema,
                     std::string& error);

/// The schema of `request`'s version (its other fields are ignored).
bool lance_dataset_schema(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                          ArrowSchema& out_schema, std::string& error);

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

/// Read the rows at `indices` (logical row numbers, as a full read numbers them: deleted rows are not
/// counted), optionally projected -- random access, e.g. a shuffled training mini-batch.
///
/// The rows come back in ascending order, each once, one batch per fragment touched: sort and dedupe
/// happen here, so reorder afterwards if your indices were not ascending (the Python `take` does).
/// Fragments without a requested row are never opened, and within a fragment only the pages holding a
/// requested row are read -- for large values (images, audio: FullZip pages) only the rows themselves.
/// An index past the end is an error.
bool lance_table_take(const std::filesystem::path& dataset_path, const std::vector<std::string>* column_names,
                      const std::vector<std::uint64_t>& indices, ArrowSchema& out_schema,
                      std::vector<ArrowArray>& out_batches, std::string& error, bool trusted_input = false);

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

    /// As `open`, for a whole scan request.
    static bool open_request(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                             ArrowSchema& out_schema, LanceTableStream& out, std::string& error,
                             bool trusted_input = false);

    /// Decode the next data file. Returns false on failure; on success with no data left,
    /// `out_batch.release` is null. The caller owns each batch it receives.
    bool next(ArrowArray& out_batch, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Hand an opened stream and its schema to an ArrowArrayStream, which owns both from then on.
void lance_table_stream_export(LanceTableStream&& stream, ArrowSchema&& schema, ArrowArrayStream& out);

}  // namespace nano_lance
