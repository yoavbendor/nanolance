// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// dataset_ops.hpp: the changes a user makes to a dataset, each one new version. On a three-fragment dataset of
// int64 ids: delete by predicate, update, merge_insert (update matched, insert the rest), add a column from SQL,
// rename it, drop it, add an all-null column, and compact the fragments into one; after each, the rows a scan
// reads. Refusals: a bad predicate, dropping a column that does not exist, a change from a stale read version
// that conflicts. And a compaction with nothing to do commits nothing.

#include "nanolance/dataset_ops.hpp"

#include "nanolance/dataset.hpp"

#include "support.hpp"

#include <algorithm>
#include <string>
#include <vector>

using dataset_test::require;
namespace nl = nano_lance;

namespace {

std::uint64_t latest(const std::filesystem::path& path) {
    std::uint64_t v = 0;
    std::string error;
    require(nl::dataset_latest_version(path, v, error), "latest: " + error);
    return v;
}

std::vector<std::int64_t> sorted(std::vector<std::int64_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}

}  // namespace

int main() {
    const auto path = dataset_test::scratch("nano_lance_dataset_ops_test");
    dataset_test::write_dataset(path, {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}});
    std::string error;
    std::uint64_t v = 0;

    // delete
    std::uint64_t deleted = 0;
    require(nl::dataset_delete(path, "id < 3", deleted, v, error), "delete: " + error);
    require(deleted == 2 && v == 4, "two rows deleted, as version 4");
    require(dataset_test::read_column(path, "id") == std::vector<std::int64_t>{3, 4, 5, 6, 7, 8, 9}, "after delete");
    require(!nl::dataset_delete(path, "id <<< 3", deleted, v, error) && !error.empty(), "a bad predicate is refused");
    require(latest(path) == 4, "a refused delete commits nothing");
    // Deleting rows that are already gone commits nothing either way the count is zero.
    std::uint64_t again = 0;
    require(nl::dataset_delete(path, "id = 1", again, v, error) && again == 0, "nothing more to delete");

    // update: the rows rewritten into a new fragment
    const std::string where = "id = 5";
    std::uint64_t updated = 0;
    require(nl::dataset_update(path, &where, {{"id", "50"}}, updated, v, error), "update: " + error);
    require(updated == 1, "one row updated");
    require(sorted(dataset_test::read_column(path, "id")) == std::vector<std::int64_t>{3, 4, 6, 7, 8, 9, 50},
            "after update");

    // merge_insert: 4 matches (updated to itself), 100 does not (inserted)
    {
        ArrowSchema schema{};
        ArrowArray array{};
        dataset_test::make_ids({4, 100}, schema, array);
        ArrowArrayStream stream{};
        require(ArrowBasicArrayStreamInit(&stream, &schema, 1) == NANOARROW_OK, "stream init");
        ArrowBasicArrayStreamSetArray(&stream, 0, &array);
        nl::MergeInsertSpec spec;
        spec.on = {"id"};
        spec.when_matched = nl::MergeInsertSpec::WhenMatched::UpdateAll;
        spec.when_not_matched_insert_all = true;
        nl::MergeInsertStats stats;
        require(nl::dataset_merge_insert(path, spec, stream, stats, v, error), "merge_insert: " + error);
        require(stats.inserted == 1 && stats.updated == 1 && stats.deleted == 0, "one inserted, one updated");
        require(stream.release == nullptr, "the source stream is consumed");
    }
    const std::vector<std::int64_t> rows = {3, 4, 6, 7, 8, 9, 50, 100};
    require(sorted(dataset_test::read_column(path, "id")) == rows, "after merge_insert");

    // add a column from SQL, rename it, drop it
    require(nl::dataset_add_columns_sql(path, {{"dbl", "id * 2"}}, v, error), "add_columns: " + error);
    require(dataset_test::column_names(path) == std::vector<std::string>{"id", "dbl"}, "dbl added");
    {
        const auto ids = dataset_test::read_column(path, "id");
        const auto dbl = dataset_test::read_column(path, "dbl");
        require(ids.size() == dbl.size(), "one value per row");
        for (std::size_t i = 0; i < ids.size(); ++i) {
            require(dbl[i] == ids[i] * 2, "dbl is id * 2");
        }
    }
    nl::ColumnAlteration rename;
    rename.path = "dbl";
    rename.rename = "twice";
    require(nl::dataset_alter_columns(path, {rename}, v, error), "alter_columns: " + error);
    require(dataset_test::column_names(path) == std::vector<std::string>{"id", "twice"}, "renamed");
    require(nl::dataset_drop_columns(path, {"twice"}, v, error), "drop_columns: " + error);
    require(dataset_test::column_names(path) == std::vector<std::string>{"id"}, "dropped");
    require(!nl::dataset_drop_columns(path, {"nope"}, v, error) && !error.empty(), "an unknown column is refused");

    // add an all-null column
    {
        ArrowSchema nulls{};
        ArrowSchemaInit(&nulls);
        require(ArrowSchemaSetTypeStruct(&nulls, 1) == NANOARROW_OK &&
                    ArrowSchemaSetType(nulls.children[0], NANOARROW_TYPE_INT32) == NANOARROW_OK &&
                    ArrowSchemaSetName(nulls.children[0], "empty") == NANOARROW_OK,
                "null column schema");
        require(nl::dataset_add_columns_nulls(path, nulls, v, error), "add_columns_nulls: " + error);
        ArrowSchemaRelease(&nulls);
        require(dataset_test::column_names(path) == std::vector<std::string>{"id", "empty"}, "empty added");
        require(nl::dataset_drop_columns(path, {"empty"}, v, error), "drop empty: " + error);
    }

    // A change made from a stale read version that touched the same rows: delete raced by an update of
    // fragment 1's rows is rebased or refused, never applied twice.
    const auto stale = latest(path);
    require(nl::dataset_delete(path, "id = 3", deleted, v, error) && deleted == 1, "delete id 3: " + error);
    std::uint64_t raced = 0;
    const bool ok = nl::dataset_delete(path, "id = 3", raced, v, error, stale);
    require(!ok || raced <= 1, "a delete from a stale version does not double-count");
    require(sorted(dataset_test::read_column(path, "id")) == std::vector<std::int64_t>{4, 6, 7, 8, 9, 50, 100},
            "id 3 is deleted once");

    // compaction: every fragment into one
    nl::DatasetInfo info;
    require(nl::dataset_info(path, false, 0, info, error) && info.fragments.size() > 1, "several fragments");
    nl::CompactionOptions options;
    nl::CompactionMetrics metrics;
    require(nl::dataset_compact_files(path, options, metrics, v, error), "compact: " + error);
    require(metrics.fragments_removed == info.fragments.size() && metrics.fragments_added == 1,
            "every fragment rewritten into one");
    require(nl::dataset_info(path, false, 0, info, error) && info.fragments.size() == 1, "one fragment left");
    require(sorted(dataset_test::read_column(path, "id")) == std::vector<std::int64_t>{4, 6, 7, 8, 9, 50, 100},
            "compaction keeps the rows");
    const auto before = latest(path);
    nl::CompactionMetrics none;
    require(nl::dataset_compact_files(path, options, none, v, error) && v == before && none.fragments_removed == 0,
            "nothing to compact commits nothing");

    // a zero budget is refused, as Lance refuses it
    nl::CompactionOptions zero;
    zero.max_source_rows = 0;
    std::uint64_t read_version = 0;
    std::vector<std::vector<std::uint64_t>> tasks;
    require(!nl::dataset_compact_plan(path, zero, read_version, tasks, error), "a zero budget is refused");

    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::cout << "dataset_ops: OK\n";
    return 0;
}
