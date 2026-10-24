// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// dataset.hpp: what a dataset's manifests say about it, and the metadata-only versions it commits. A two-version
// dataset is written with the C writer; then its versions, fragments, fields, transactions and data stats are
// read back, config / table / schema / field metadata are changed (each one new version), and an old version is
// restored. Errors: a path with no dataset, a field id the schema lacks.

#include "nanolance/dataset.hpp"

#include "support.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

using dataset_test::require;
namespace nl = nano_lance;

int main() {
    const auto path = dataset_test::scratch("nano_lance_dataset_test");
    dataset_test::write_dataset(path, {{1, 2, 3}, {4, 5}});
    std::string error;

    // Versions: one per commit, oldest first.
    std::uint64_t latest = 0;
    require(nl::dataset_latest_version(path, latest, error) && latest == 2, "latest version is 2: " + error);
    std::vector<nl::DatasetVersionInfo> versions;
    require(nl::dataset_versions(path, versions, error), "versions: " + error);
    require(versions.size() == 2 && versions[0].version == 1 && versions[1].version == 2, "two versions, in order");
    require(versions[1].timestamp_ns > 0, "a version records its timestamp");

    // A version's description: fragments and their rows.
    nl::DatasetInfo info;
    require(nl::dataset_info(path, false, 0, info, error), "info: " + error);
    require(info.version.version == 2 && info.latest_version == 2, "latest info is version 2");
    require(info.fragments.size() == 2 && info.num_rows() == 5, "two fragments, five rows");
    require(info.fragments[0].files.size() == 1 && !info.fragments[0].files[0].path.empty(), "one data file each");
    require(!info.data_storage_version.empty(), "the data storage version is named");
    require(!info.stable_row_ids(), "no stable row ids by default");
    nl::DatasetInfo first;
    require(nl::dataset_info(path, true, 1, first, error) && first.fragments.size() == 1 && first.num_rows() == 3,
            "version 1 has one fragment of three rows");
    nl::DatasetInfo none;
    require(!nl::dataset_info(path, true, 99, none, error), "a version that does not exist is an error");

    // The transaction that made version 2 (an append).
    std::vector<std::uint8_t> txn;
    bool found = false;
    require(nl::dataset_transaction(path, 2, txn, found, error), "transaction: " + error);
    require(found && !txn.empty(), "version 2 has a transaction");
    bool migrate = true;
    require(nl::dataset_needs_migration(path, false, 0, migrate, error) && !migrate,
            "a current writer's manifest needs no migration");

    // Fields and the bytes per field.
    std::vector<nl::DatasetField> fields;
    require(nl::dataset_fields(path, false, 0, fields, error), "fields: " + error);
    require(fields.size() == 1 && fields[0].name == "id" && fields[0].logical_type == "int64" &&
                fields[0].parent_id == -1,
            "one int64 field named id");
    std::vector<std::pair<std::int32_t, std::uint64_t>> stats;
    require(nl::dataset_data_stats(path, false, 0, stats, error), "data stats: " + error);
    require(stats.size() == 1 && stats[0].first == fields[0].id && stats[0].second > 0, "the id field has bytes");

    // Config: upsert, then remove; each a new version.
    std::uint64_t v = 0;
    require(nl::dataset_update_config(path, {{"k", "v"}}, {}, v, error) && v == 3, "config upsert is version 3");
    require(nl::dataset_info(path, false, 0, info, error) && info.config.at("k") == "v", "the key is set");
    require(nl::dataset_update_config(path, {}, {"k"}, v, error) && v == 4, "config remove is version 4");
    require(nl::dataset_info(path, false, 0, info, error) && info.config.count("k") == 0, "the key is gone");

    // Table and schema metadata: upsert, then replace.
    require(nl::dataset_update_table_metadata(path, {{"a", "1"}}, false, v, error), "table metadata: " + error);
    require(nl::dataset_update_table_metadata(path, {{"b", "2"}}, false, v, error), "table metadata: " + error);
    require(nl::dataset_info(path, false, 0, info, error) && info.table_metadata.size() == 2, "upserted twice");
    require(nl::dataset_update_table_metadata(path, {{"c", "3"}}, true, v, error), "table metadata: " + error);
    require(nl::dataset_info(path, false, 0, info, error) && info.table_metadata ==
                std::map<std::string, std::string>{{"c", "3"}},
            "replace leaves only the new map");
    require(nl::dataset_update_schema_metadata(path, {{"s", "x"}}, false, v, error), "schema metadata: " + error);
    require(nl::dataset_info(path, false, 0, info, error) && info.schema_metadata.at("s") == "x", "schema metadata set");

    // Field metadata, by field id; an unknown id is refused and commits nothing.
    nl::FieldMetadataUpdate update;
    update.entries.emplace_back("m", std::string("y"));
    require(nl::dataset_update_field_metadata(path, {{fields[0].id, update}}, v, error), "field metadata: " + error);
    require(nl::dataset_fields(path, false, 0, fields, error) && fields[0].metadata.at("m") == "y",
            "field metadata set");
    std::uint64_t before = 0;
    require(nl::dataset_latest_version(path, before, error), "latest");
    require(!nl::dataset_update_field_metadata(path, {{12345, update}}, v, error), "an unknown field id is an error");
    std::uint64_t after = 0;
    require(nl::dataset_latest_version(path, after, error) && after == before, "a refused change commits nothing");

    // Restore version 1: a new version with its fragments.
    std::uint64_t restored = 0;
    require(nl::dataset_restore(path, 1, restored, error), "restore: " + error);
    require(restored == before + 1, "restore commits the next version");
    require(nl::dataset_info(path, false, 0, info, error) && info.fragments.size() == 1 && info.num_rows() == 3,
            "the restored version has version 1's fragment");
    require(dataset_test::read_column(path, "id") == std::vector<std::int64_t>{1, 2, 3}, "and reads its rows");

    // No dataset at the path.
    std::uint64_t missing = 0;
    require(!nl::dataset_latest_version(path / "nothing", missing, error) && !error.empty(),
            "a path with no dataset is an error");

    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::cout << "dataset: OK\n";
    return 0;
}
