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
/// the schema. Predicates and values are SQL (nanolance/expr.hpp). Every operation works on the
/// latest version and fails with "commit conflict" if another writer committed in the meantime.
namespace nano_lance {

/// Delete the rows where `predicate` is TRUE.
bool dataset_delete(const std::filesystem::path& dataset_path, const std::string& predicate, std::uint64_t& deleted,
                    std::uint64_t& new_version, std::string& error);

/// Set `assignments` (column, SQL value) on the rows where `predicate` is TRUE (every row when null).
/// The rows are rewritten into a new fragment, as Lance does.
bool dataset_update(const std::filesystem::path& dataset_path, const std::string* predicate,
                    const std::vector<std::pair<std::string, std::string>>& assignments, std::uint64_t& updated,
                    std::uint64_t& new_version, std::string& error);

/// Lance's merge insert: match `source` rows to the dataset's by the `on` columns.
struct MergeInsertSpec {
    enum class WhenMatched { DoNothing, UpdateAll, Fail, Delete };
    std::vector<std::string> on;
    WhenMatched when_matched = WhenMatched::DoNothing;
    bool when_not_matched_insert_all = true;
    bool when_not_matched_by_source_delete = false;
    std::string when_not_matched_by_source_condition;  // SQL over the dataset's rows; empty: all
};

struct MergeInsertStats {
    std::uint64_t inserted = 0;
    std::uint64_t updated = 0;
    std::uint64_t deleted = 0;
};

/// `source` is consumed (released) whatever the outcome. Its columns are the dataset's, in any order.
bool dataset_merge_insert(const std::filesystem::path& dataset_path, const MergeInsertSpec& spec,
                          ArrowArrayStream& source, MergeInsertStats& stats, std::uint64_t& new_version,
                          std::string& error);

/// New columns, each an SQL expression over the existing ones (`price * 2`).
bool dataset_add_columns_sql(const std::filesystem::path& dataset_path,
                             const std::vector<std::pair<std::string, std::string>>& columns,
                             std::uint64_t& new_version, std::string& error);

/// New columns, all null: the fields of `schema` (a struct).
bool dataset_add_columns_nulls(const std::filesystem::path& dataset_path, const ArrowSchema& schema,
                               std::uint64_t& new_version, std::string& error);

/// New columns from `stream`, one row per row of the dataset in its order. The dataset must have no
/// deleted rows. `stream` is consumed.
bool dataset_add_columns_stream(const std::filesystem::path& dataset_path, ArrowArrayStream& stream,
                                std::uint64_t& new_version, std::string& error);

/// Remove columns (top-level names or `struct.child` paths) from the schema. The data stays in the
/// files, unreferenced, as Lance leaves it.
bool dataset_drop_columns(const std::filesystem::path& dataset_path, const std::vector<std::string>& columns,
                          std::uint64_t& new_version, std::string& error);

struct ColumnAlteration {
    std::string path;                  // "col" or "struct.child"
    std::optional<std::string> rename;
    std::optional<bool> nullable;
    const ArrowSchema* data_type = nullptr;  // top-level columns: rewrite as this type
};

bool dataset_alter_columns(const std::filesystem::path& dataset_path, const std::vector<ColumnAlteration>& alterations,
                           std::uint64_t& new_version, std::string& error);

struct CompactionOptions {
    std::uint64_t target_rows_per_fragment = 1024ULL * 1024ULL;
    bool materialize_deletions = true;
    double materialize_deletions_threshold = 0.1;  // fraction of a fragment's rows deleted
};

struct CompactionMetrics {
    std::uint64_t fragments_removed = 0;
    std::uint64_t fragments_added = 0;
    std::uint64_t files_removed = 0;
    std::uint64_t files_added = 0;
};

/// Rewrite small fragments (and fragments with many deleted rows) into fewer, larger ones. Commits
/// nothing when there is nothing to compact (`new_version` is then the current one).
bool dataset_compact_files(const std::filesystem::path& dataset_path, const CompactionOptions& options,
                           CompactionMetrics& metrics, std::uint64_t& new_version, std::string& error);

}  // namespace nano_lance
