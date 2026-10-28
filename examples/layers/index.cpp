// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer index: each index kind, built, looked up and described in Lance's format. This example builds a BTree scalar
// index on id (dataset_create_scalar_index), lists it (dataset_list_indices) and counts the rows of a filter from it
// (dataset_count_from_index). The dataset is written with the table layer's commit, because the dataset layer's
// writers sit above this layer.
//
// Run it with: ctest --test-dir build -R nano_lance_example_index --output-on-failure

#include "nanolance/arrow_slice.hpp"  // completes the Arrow C structs (through nanoarrow), see IdBatch
#include "nanolance/array_accessor.hpp"
#include "nanolance/column_values.hpp"
#include "nanolance/data_file_writer.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/scalar_index.hpp"
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
    nano_lance::NewFragment fragment{.data_file = {}, .rows = ids.size(), .base_id = {}};
    if (!nano_lance::write_lance_data_file(dataset, file_name, mapping, columns, ids.size(), 0, false,
                                           fragment.data_file, error)) {
        return false;
    }
    return nano_lance::commit_dataset_version(dataset, mapping, {fragment}, mode, version, error);
}
}  // namespace

int main() {
    const ScratchDir scratch{"nano_lance_example_index"};
    const auto dataset = scratch.path / "ids.lance";
    std::string error;
    std::uint64_t version = 0;

    // Step 1: a dataset of two fragments and five rows, at version 2.
    if (!commit_ids(dataset, "first.lance", {1, 2, 3}, nano_lance::CommitMode::Create, version, error) ||
        !commit_ids(dataset, "second.lance", {4, 5}, nano_lance::CommitMode::Append, version, error)) {
        return fail("writing the dataset: " + error);
    }

    // Step 2: build a BTree index on id. The default options give it a name and replace one of that name.
    std::uint64_t indexed_version = 0;
    if (!nano_lance::dataset_create_scalar_index(dataset, "id", nano_lance::ScalarIndexType::BTree, {},
                                                 indexed_version, error) ||
        indexed_version != version + 1) {
        return fail("dataset_create_scalar_index: " + error);
    }
    std::cout << "built a BTree index, committed as version " << indexed_version << "\n";

    // Step 3: the index is listed for the latest version, as one BTree index on id.
    std::vector<nano_lance::IndexInfo> indices;
    if (!nano_lance::dataset_list_indices(dataset, false, 0, indices, error) || indices.size() != 1 ||
        indices[0].type != "BTree" || indices[0].fields != std::vector<std::string>{"id"}) {
        return fail("expected one BTree index on id " + error);
    }

    // Step 4: count the rows with 3 <= id <= 5. The index answers the whole filter, so no data file is read.
    nano_lance::IndexCount count;
    if (!nano_lance::dataset_count_from_index(dataset, false, 0, "id >= 3 AND id <= 5", count, error) ||
        count.count != 3 || !count.exact) {
        return fail("the index should count 3 rows exactly: " + error);
    }
    std::cout << "rows with 3 <= id <= 5: " << count.count << ", exact from the index\n";
    return 0;
}
