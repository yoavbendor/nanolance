// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer scan: reading rows of fragments, given a row selection (a LanceScanRequest: columns, row range, filter).
// This example writes a standalone .lance file of ten rows, then reads it by row range (lance_file_read) and by row
// numbers (lance_file_take). The dataset reads (lance_dataset_scan, lance_table_take) take the same request.
//
// Run it with: ctest --test-dir build -R nano_lance_example_scan --output-on-failure

#include "nanolance/data_file_writer.hpp"
#include "nanolance/lance_file_writer.hpp"
#include "nanolance/lance_table_reader.hpp"

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
// The schema and batches of a read, released when this goes out of scope.
struct ReadResult {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    ~ReadResult() {
        for (auto& batch : batches) { if (batch.release) { batch.release(&batch); } }
        if (schema.release) { schema.release(&schema); }
    }
    ReadResult() = default;
    ReadResult(const ReadResult&) = delete;
    ReadResult& operator=(const ReadResult&) = delete;
};

// The id values of every batch of a read, in order.
std::vector<std::int64_t> ids_of(const ReadResult& result) {
    std::vector<std::int64_t> out;
    for (const auto& batch : result.batches) {
        const ArrowArray& column = *batch.children[0];
        const auto* data = static_cast<const std::int64_t*>(column.buffers[1]) + column.offset;
        out.insert(out.end(), data, data + batch.length);
    }
    return out;
}

}  // namespace

int main() {
    const ScratchDir scratch{"nano_lance_example_scan"};
    const auto path = scratch.path / "ids.lance";
    const std::vector<std::int64_t> ids = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::string error;

    // Step 1: write the ten rows as one standalone .lance file.
    IdBatch batch(ids);
    nano_lance::LanceFileExtras extras;
    extras.path = path;
    std::uint64_t file_size = 0;
    if (!nano_lance::write_lance_file(batch.schema, {&batch.array}, extras, file_size, error)) {
        return fail("write_lance_file: " + error);
    }

    // Step 2: a row range, three rows from row 2. Only the rows in the range come back.
    nano_lance::LanceScanRequest range_request;
    range_request.range.offset = 2;
    range_request.range.length = 3;
    ReadResult by_range;
    if (!nano_lance::lance_file_read(path, range_request, by_range.schema, by_range.batches, error)) {
        return fail("lance_file_read: " + error);
    }
    std::cout << "rows 2 to 4: " << ids_of(by_range).size() << " values\n";
    if (ids_of(by_range) != std::vector<std::int64_t>{2, 3, 4}) {
        return fail("the row range returned the wrong rows");
    }

    // Step 3: a row selection. lance_file_take reads the rows at these row numbers, in ascending order.
    ReadResult by_take;
    if (!nano_lance::lance_file_take(path, nano_lance::LanceScanRequest{}, {1, 4, 8}, by_take.schema,
                                     by_take.batches, error)) {
        return fail("lance_file_take: " + error);
    }
    std::cout << "rows 1, 4 and 8: " << ids_of(by_take).size() << " values\n";
    if (ids_of(by_take) != std::vector<std::int64_t>{1, 4, 8}) {
        return fail("the row selection returned the wrong rows");
    }
    return 0;
}
