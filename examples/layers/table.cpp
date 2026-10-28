// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer table: what a version is. A dataset is a directory of data files and one manifest per version; a manifest lists
// the version's fragments (the data files of one write) and its schema. This example writes two data files, commits
// them as versions 1 (CREATE) and 2 (APPEND), and reads the manifests back. The writers of the dataset layer sit above
// this layer, so the data files are written here, with write_lance_data_file and commit_dataset_version.
//
// Run it with: ctest --test-dir build -R nano_lance_example_table --output-on-failure

#include "nanolance/arrow_slice.hpp"  // completes the Arrow C structs (through nanoarrow), see IdBatch
#include "nanolance/array_accessor.hpp"
#include "nanolance/column_values.hpp"
#include "nanolance/data_file_writer.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/schema_mapper.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

// A fresh directory under the temp directory, removed when the example ends (also on an early return).
struct ScratchDir {
    std::filesystem::path path;
    explicit ScratchDir(const std::string& name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

int fail(const std::string& message) { std::cerr << "FAIL: " << message << "\n"; return 1; }

// One int64 column "id" as an Arrow struct batch, written out field by field in the C data interface's order (this
// layer's README does not name nanoarrow). The batch points at `values`, which must outlive it.
struct IdBatch {
    ArrowSchema id_schema{"l", "id", nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr};
    ArrowSchema* schema_children[1] = {&id_schema};
    ArrowSchema schema{"+s", nullptr, nullptr, 0, 1, schema_children, nullptr, nullptr, nullptr};
    const void* id_buffers[2] = {nullptr, nullptr};
    ArrowArray id_array{0, 0, 0, 2, 0, id_buffers, nullptr, nullptr, nullptr, nullptr};
    ArrowArray* array_children[1] = {&id_array};
    const void* root_buffers[1] = {nullptr};
    ArrowArray array{0, 0, 0, 1, 1, root_buffers, array_children, nullptr, nullptr, nullptr};

    explicit IdBatch(const std::vector<std::int64_t>& values) {
        id_buffers[1] = values.data();
        id_array.length = array.length = static_cast<std::int64_t>(values.size());
    }
    IdBatch(const IdBatch&) = delete;
    IdBatch& operator=(const IdBatch&) = delete;
};

// Writes `ids` as one data file and publishes it as a version. CREATE makes the dataset; APPEND adds a fragment to
// the latest version. `version` receives the new version number.
bool commit_ids(const std::filesystem::path& dataset, const std::string& file_name,
                const std::vector<std::int64_t>& ids, nano_lance::CommitMode mode, std::uint64_t& version,
                std::string& error) {
    IdBatch batch(ids);
    nano_lance::LanceSchemaMapping mapping;
    if (!nano_lance::map_arrow_schema(batch.schema, mapping, error)) {
        return false;
    }
    std::vector<nano_lance::ColumnValues> columns(nano_lance::lance_physical_fields(mapping).size());
    if (!nano_lance::append_batch_column_values(batch.array, mapping, columns, error)) {
        return false;
    }
    nano_lance::DataFileResult data;
    if (!nano_lance::write_lance_data_file(dataset, file_name, mapping, columns, ids.size(), 0, false, data, error)) {
        return false;
    }
    nano_lance::NewFragment fragment{.data_file = data, .rows = ids.size(), .base_id = {}};
    return nano_lance::commit_dataset_version(dataset, mapping, {fragment}, mode, version, error);
}

}  // namespace

int main() {
    const ScratchDir scratch{"nano_lance_example_table"};
    const auto dataset = scratch.path / "ids.lance";
    std::string error;
    std::uint64_t version = 0;

    // Step 1: CREATE publishes version 1 with one fragment of three rows. Step 2: APPEND publishes version 2,
    // which adds a fragment of two rows to version 1's.
    if (!commit_ids(dataset, "first.lance", {1, 2, 3}, nano_lance::CommitMode::Create, version, error) ||
        version != 1) {
        return fail("CREATE should publish version 1: " + error);
    }
    if (!commit_ids(dataset, "second.lance", {4, 5}, nano_lance::CommitMode::Append, version, error) ||
        version != 2) {
        return fail("APPEND should publish version 2: " + error);
    }

    // Step 3: every version has a manifest in _versions/. The latest one is read by default.
    if (nano_lance::list_manifest_versions(dataset, error) != std::vector<std::uint64_t>{1, 2}) {
        return fail("expected manifests for versions 1 and 2");
    }
    nano_lance::pb::Manifest latest;
    std::uint64_t latest_version = 0;
    if (!nano_lance::load_latest_manifest(dataset, latest, latest_version, error)) {
        return fail("load_latest_manifest: " + error);
    }
    std::uint64_t rows = 0;
    for (const auto& fragment : latest.fragments) { rows += fragment.physical_rows; }
    std::cout << "latest version " << latest_version << ": " << latest.fragments.size() << " fragments, " << rows
              << " rows\n";
    if (latest_version != 2 || latest.fragments.size() != 2 || rows != 5) {
        return fail("version 2 should hold 2 fragments and 5 rows");
    }
    return 0;
}
