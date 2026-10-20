// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/// Changes to a dataset, each committed as one new version, the way Lance makes them: rows are
/// deleted with deletion files, updated and merged rows are rewritten into new fragments, added
/// columns are new data files beside a fragment's others, dropped and renamed columns change only
/// the schema. Predicates and values are SQL (nanolance/expr.hpp). An operation reads `read_version`
/// (the version its handle was opened at, as pylance commits from there) or, when none is given, the
/// latest. Lance's verdict on a concurrent commit decides: a compatible one is rebased, an incompatible
/// one fails with "Incompatible transaction:", and a retryable one re-runs the operation on the latest
/// version (as Lance's own retries do) and fails with "commit conflict" when the retries run out.
namespace nano_lance {

/// Delete the rows where `predicate` is TRUE.
bool dataset_delete(const std::filesystem::path& dataset_path, const std::string& predicate, std::uint64_t& deleted,
                    std::uint64_t& new_version, std::string& error, std::optional<std::uint64_t> read_version = std::nullopt);

struct RowAddrMask;

/// Set `assignments` (column, SQL value) on the rows where `predicate` is TRUE (every row when null), and
/// only on the rows `row_mask` allows when given (rows selected by _rowid / _rowaddr). The rows are
/// rewritten into a new fragment, as Lance does.
bool dataset_update(const std::filesystem::path& dataset_path, const std::string* predicate,
                    const std::vector<std::pair<std::string, std::string>>& assignments, std::uint64_t& updated,
                    std::uint64_t& new_version, std::string& error, const RowAddrMask* row_mask = nullptr,
                    std::optional<std::uint64_t> read_version = std::nullopt);

/// Lance's merge insert: match `source` rows to the dataset's by the `on` columns.
struct MergeInsertSpec {
    enum class WhenMatched { DoNothing, UpdateAll, UpdateIf, Fail, Delete };
    std::vector<std::string> on;
    WhenMatched when_matched = WhenMatched::DoNothing;
    /// UpdateIf: an SQL condition over the matched pair's columns, `source.<column>` and
    /// `target.<column>` (`source.ts > target.ts`); a matched row is updated where it is TRUE and
    /// left as it is otherwise.
    std::string when_matched_condition;
    bool when_not_matched_insert_all = true;
    bool when_not_matched_by_source_delete = false;
    std::string when_not_matched_by_source_condition;  // SQL over the dataset's rows; empty: all
    /// Set: write the change's files but commit nothing, and return the transaction that would commit
    /// it (pylance's execute_uncommitted), as a lance.table.Transaction operation.
    struct Uncommitted {
        std::uint64_t read_version = 0;
        std::uint32_t operation_field = 0;
        std::vector<std::uint8_t> operation;
    };
    Uncommitted* uncommitted = nullptr;
    /// Where the new data files go, in turn: a directory and base id per target (as
    /// nano_lance_writer_set_fragment_targets). Empty: the dataset's own data directory.
    std::vector<std::string> target_dirs;
    std::vector<std::uint32_t> target_base_ids;
};

struct MergeInsertStats {
    std::uint64_t inserted = 0;
    std::uint64_t updated = 0;
    std::uint64_t deleted = 0;
};

/// `source` is consumed (released) whatever the outcome. Its columns are the dataset's, in any order.
bool dataset_merge_insert(const std::filesystem::path& dataset_path, const MergeInsertSpec& spec,
                          ArrowArrayStream& source, MergeInsertStats& stats, std::uint64_t& new_version,
                          std::string& error, std::optional<std::uint64_t> read_version = std::nullopt);

/// New columns, each an SQL expression over the existing ones (`price * 2`).
bool dataset_add_columns_sql(const std::filesystem::path& dataset_path,
                             const std::vector<std::pair<std::string, std::string>>& columns,
                             std::uint64_t& new_version, std::string& error, std::optional<std::uint64_t> read_version = std::nullopt);

/// New columns, all null: the fields of `schema` (a struct).
bool dataset_add_columns_nulls(const std::filesystem::path& dataset_path, const ArrowSchema& schema,
                               std::uint64_t& new_version, std::string& error, std::optional<std::uint64_t> read_version = std::nullopt);

/// New columns from `stream`, one row per row of the dataset in its order. The dataset must have no
/// deleted rows. `stream` is consumed.
bool dataset_add_columns_stream(const std::filesystem::path& dataset_path, ArrowArrayStream& stream,
                                std::uint64_t& new_version, std::string& error, std::optional<std::uint64_t> read_version = std::nullopt);

/// Remove columns (top-level names or `struct.child` paths) from the schema. The data stays in the
/// files, unreferenced, as Lance leaves it.
bool dataset_drop_columns(const std::filesystem::path& dataset_path, const std::vector<std::string>& columns,
                          std::uint64_t& new_version, std::string& error, std::optional<std::uint64_t> read_version = std::nullopt);

struct ColumnAlteration {
    std::string path;                  // "col" or "struct.child"
    std::optional<std::string> rename;
    std::optional<bool> nullable;
    const ArrowSchema* data_type = nullptr;  // top-level columns: rewrite as this type
};

bool dataset_alter_columns(const std::filesystem::path& dataset_path, const std::vector<ColumnAlteration>& alterations,
                           std::uint64_t& new_version, std::string& error, std::optional<std::uint64_t> read_version = std::nullopt);

struct CompactionOptions {
    std::uint64_t target_rows_per_fragment = 1024ULL * 1024ULL;
    bool materialize_deletions = true;
    double materialize_deletions_threshold = 0.1;  // fraction of a fragment's rows deleted
    /// Fold the rewritten rows back into the indexes that covered them (index_optimize.hpp), as
    /// Lance's compaction remaps its indexes: committed as a second version. Without it, an index
    /// stops covering the fragments compaction rewrote, and those rows are scanned.
    bool reindex = true;
    /// Lance's deferred index remap: the indexes keep their old row addresses, and a fragment reuse index
    /// (`__lance_frag_reuse`) records how the rewritten rows moved. Instead of `reindex`. Not with stable
    /// row ids.
    bool defer_index_remap = false;
    /// Lance's source budgets: plan whole tasks, in order, while their source fragments, live rows
    /// and data-file bytes stay within every one set. A first task over budget plans nothing; a
    /// budget of 0 is refused.
    std::optional<std::uint64_t> max_source_fragments;
    std::optional<std::uint64_t> max_source_rows;
    std::optional<std::uint64_t> max_source_bytes;
    /// Fragments left as they are; each one also ends the run of candidates around it.
    std::vector<std::uint64_t> excluded_fragment_ids;
    /// A new data file ends once it holds about this many bytes (0: no limit); rows go to the writer
    /// `batch_size` at a time (0: Lance's scanner default, 8192), so a small limit cuts often.
    std::uint64_t max_bytes_per_file = 0;
    std::uint64_t batch_size = 0;
};

struct CompactionMetrics {
    std::uint64_t fragments_removed = 0;
    std::uint64_t fragments_added = 0;
    std::uint64_t files_removed = 0;
    std::uint64_t files_added = 0;
    /// With `reindex`: the indexes given back their compacted rows, and those that could not be (an
    /// index of a kind nanolance cannot build), which cover fewer fragments from now on.
    std::vector<std::string> indexes_reindexed;
    std::vector<std::string> indexes_not_reindexed;
};

/// Rewrite small fragments (and fragments with many deleted rows) into fewer, larger ones. Commits
/// nothing when there is nothing to compact (`new_version` is then the current one).
bool dataset_compact_files(const std::filesystem::path& dataset_path, const CompactionOptions& options,
                           CompactionMetrics& metrics, std::uint64_t& new_version, std::string& error, std::optional<std::uint64_t> read_version = std::nullopt);

// ── compaction as pylance's Compaction.plan / execute / commit ──────────────────────────────────
//
// plan: the tasks (each a list of fragment ids) of the latest version; execute: one task rewritten into new
// data files, uncommitted (the fragment messages, ids unassigned); commit: the rewrites committed as one
// version, with the indexes remapped (reindex, or the fragment reuse index with `defer_index_remap`).

/// One task rewritten: the fragments it removes, and the new fragments (lance.table.DataFragment messages).
struct CompactionRewrite {
    std::vector<std::uint64_t> old_fragment_ids;
    std::vector<std::vector<std::uint8_t>> new_fragments;
};

/// The tasks the planner would run on the latest version (`read_version` is that version).
bool dataset_compact_plan(const std::filesystem::path& dataset_path, const CompactionOptions& options,
                          std::uint64_t& read_version, std::vector<std::vector<std::uint64_t>>& tasks,
                          std::string& error);

/// Rewrites the fragments `fragment_ids` of version `read_version` into new data files, committing nothing.
bool dataset_compact_execute(const std::filesystem::path& dataset_path, const CompactionOptions& options,
                             std::uint64_t read_version, const std::vector<std::uint64_t>& fragment_ids,
                             CompactionRewrite& out, std::string& error);

/// Commits rewrites made from version `read_version` as one Rewrite version (planned again by the caller on a
/// conflict). Nothing when `rewrites` is empty.
bool dataset_compact_commit(const std::filesystem::path& dataset_path, const CompactionOptions& options,
                            std::uint64_t read_version, const std::vector<CompactionRewrite>& rewrites,
                            CompactionMetrics& metrics, std::uint64_t& new_version, std::string& error);

// ── one fragment, uncommitted ────────────────────────────────────────────────────────────────────
//
// What pylance's LanceFragment.delete / merge_columns / update_columns do: write the files of a
// change to one fragment of version `version` and return the fragment as it would then be (a
// lance.table.DataFragment message), for a hand-built transaction (dataset_transaction.hpp) to commit.

/// Delete the rows of fragment `fragment_id` where `predicate` is TRUE, or (`predicate` null) the rows
/// at physical `offsets`: a new deletion file, with the fragment's earlier deletions too.
/// `emptied` when no row is left (the fragment is then to be deleted outright; no file is written).
bool fragment_delete_rows(const std::filesystem::path& dataset_path, std::uint64_t version, std::uint64_t fragment_id,
                          const std::string* predicate, const std::vector<std::uint32_t>& offsets,
                          std::vector<std::uint8_t>& fragment_out, bool& emptied, std::string& error);

/// New columns for fragment `fragment_id`, each an SQL expression over its other columns: one new data
/// file, the columns numbered after `max_field_id` (merge_columns with a dict).
bool fragment_add_columns_sql(const std::filesystem::path& dataset_path, std::uint64_t version,
                              std::uint64_t fragment_id,
                              const std::vector<std::pair<std::string, std::string>>& columns,
                              std::int32_t max_field_id, std::vector<std::uint8_t>& fragment_out,
                              std::vector<std::vector<std::uint8_t>>& new_fields, std::string& error);

/// Write `stream` -- one row per physical row of fragment `fragment_id`, deleted rows included -- as a
/// new data file of the fragment. `replace`: its columns are the dataset's own (top-level names) and
/// keep their field ids, which other files of the fragment then tombstone (Lance's update_columns);
/// otherwise they are new columns, numbered after `max_field_id` (merge_columns), and
/// `new_fields` holds their schema fields (lance.file.Field messages). `stream` is consumed.
bool fragment_write_columns(const std::filesystem::path& dataset_path, std::uint64_t version,
                            std::uint64_t fragment_id, ArrowArrayStream& stream, bool replace,
                            std::int32_t max_field_id, std::vector<std::uint8_t>& fragment_out,
                            std::vector<std::vector<std::uint8_t>>& new_fields,
                            std::vector<std::int32_t>& fields_written, std::string& error);

}  // namespace nano_lance
