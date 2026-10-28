// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer dataset: user operations on a dataset (delete, update, merge_insert, compaction, index lifecycle). Each one
// commits a new version. This example writes two fragments (with the table layer's commit, as the dataset layer's
// writers sit above this layer), deletes a row with dataset_delete and reads the latest version back.
//
// Run it with: ctest --test-dir build -R nano_lance_example_dataset --output-on-failure

#include "nanolance/array_accessor.hpp"
#include "nanolance/column_values.hpp"
#include "nanolance/data_file_writer.hpp"
#include "nanolance/dataset_ops.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/schema_mapper.hpp"

#include <algorithm>
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
// the latest version.
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
    const ScratchDir scratch{"nano_lance_example_dataset"};
    const auto dataset = scratch.path / "ids.lance";
    std::string error;
    std::uint64_t version = 0;
    // Step 1: two fragments, version 2.
    if (!commit_ids(dataset, "first.lance", {1, 2, 3, 4}, nano_lance::CommitMode::Create, version, error) ||
        !commit_ids(dataset, "second.lance", {5, 6}, nano_lance::CommitMode::Append, version, error) || version != 2) {
        return fail("writing the dataset: " + error);
    }

    // Step 2: delete the row with id 2. That is version 3, and the rows of the fragments are otherwise untouched.
    std::uint64_t deleted = 0;
    if (!nano_lance::dataset_delete(dataset, "id = 2", deleted, version, error) || deleted != 1 || version != 3) {
        return fail("dataset_delete should delete 1 row and commit version 3: " + error);
    }

    // Step 3: read the latest version back. Row 2 is gone.
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!nano_lance::lance_table_read_dataset(dataset, schema, batches, error)) {
        return fail("read back: " + error);
    }
    std::vector<std::int64_t> ids;
    for (auto& batch : batches) {
        const ArrowArray& column = *batch.children[0];
        const auto* data = static_cast<const std::int64_t*>(column.buffers[1]) + column.offset;
        ids.insert(ids.end(), data, data + batch.length);
        batch.release(&batch);
    }
    schema.release(&schema);
    std::sort(ids.begin(), ids.end());
    std::cout << "latest version " << version << ": " << ids.size() << " rows\n";
    if (ids != std::vector<std::int64_t>{1, 3, 4, 5, 6}) {
        return fail("the latest version does not hold the expected rows");
    }
    return 0;
}
