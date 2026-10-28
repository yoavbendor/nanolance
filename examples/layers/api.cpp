// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer api: the public C APIs, as an outside user calls them. It includes no layer's internals. It writes a small
// dataset with the C writer (nano_lance_writer_*) and reads it back with the C reader (nano_lance_table_*). The
// Arrow data is built with nanoarrow, as a C or C++ caller does. (lance-c, compat/lance-c, is the other C API.)
//
// Run it with: ctest --test-dir build -R nano_lance_example_api --output-on-failure

#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

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

// The Arrow batch nanoarrow builds, released when the example ends.
struct Owned {
    ArrowSchema schema{};
    ArrowArray array{};
    ~Owned() { if (array.release) { array.release(&array); } if (schema.release) { schema.release(&schema); } }
};

// The batches of a read. The reader frees them on failure itself, so they are freed here only once filled.
struct ReadResult {
    ArrowSchema schema{};
    ArrowArray* batches = nullptr;
    size_t batch_count = 0;
    bool filled = false;
    ~ReadResult() { if (filled) { nano_lance_table_read_result_free(&schema, batches, batch_count); } }
};

}  // namespace

int main() {
    const ScratchDir scratch{"nano_lance_example_api"};
    const std::string path = (scratch.path / "ids.lance").string();
    const std::vector<std::int64_t> ids = {10, 20, 30, 40};
    char error[512] = {};

    // Step 1: an Arrow batch, struct<id: int64>, with four rows, built with nanoarrow's builders.
    Owned owned;
    ArrowSchemaInit(&owned.schema);
    if (ArrowSchemaSetTypeStruct(&owned.schema, 1) != NANOARROW_OK) {
        return fail("ArrowSchemaSetTypeStruct");
    }
    ArrowSchemaSetType(owned.schema.children[0], NANOARROW_TYPE_INT64);
    ArrowSchemaSetName(owned.schema.children[0], "id");
    if (ArrowArrayInitFromSchema(&owned.array, &owned.schema, nullptr) != NANOARROW_OK ||
        ArrowArrayStartAppending(&owned.array) != NANOARROW_OK) {
        return fail("nanoarrow could not start the batch");
    }
    for (const std::int64_t id : ids) {
        ArrowArrayAppendInt(owned.array.children[0], id);
        ArrowArrayFinishElement(&owned.array);
    }
    if (ArrowArrayFinishBuildingDefault(&owned.array, nullptr) != NANOARROW_OK) {
        return fail("ArrowArrayFinishBuildingDefault");
    }

    // Step 2: write the batch and publish it as version 1. The zeroed options are the defaults, but stage_fragments
    // keeps the batch staged until nano_lance_writer_finish publishes it. The writer is closed on every path.
    NanoLanceWriteOptions options = {};
    options.stage_fragments = true;
    NanoLanceWriter writer = {};
    uint64_t version = 0;
    int status = nano_lance_writer_open(&writer, path.c_str(), &options);
    if (status == NANO_LANCE_OK) {
        status = nano_lance_write_batch(&writer, &owned.array, &owned.schema);
    }
    if (status == NANO_LANCE_OK) {
        status = nano_lance_writer_finish(&writer, NANO_LANCE_COMMIT_CREATE, &version);
    }
    const std::string why = status == NANO_LANCE_OK ? std::string() : nano_lance_writer_last_error(&writer);
    nano_lance_writer_close(&writer);
    if (status != NANO_LANCE_OK || version != 1) {
        return fail("writing the dataset: " + why);
    }

    // Step 3: count the rows from the manifest alone, then read every row back.
    uint64_t rows = 0;
    ReadResult read;
    if (nano_lance_table_count_rows(path.c_str(), &rows, error, sizeof error) != NANO_LANCE_READER_OK ||
        nano_lance_table_read_dataset(path.c_str(), &read.schema, &read.batches, &read.batch_count, error,
                                      sizeof error) != NANO_LANCE_READER_OK) {
        return fail(std::string("reading the dataset: ") + error);
    }
    read.filled = true;
    std::vector<std::int64_t> got;
    for (size_t b = 0; b < read.batch_count; ++b) {
        const ArrowArray& column = *read.batches[b].children[0];
        const auto* data = static_cast<const std::int64_t*>(column.buffers[1]) + column.offset;
        got.insert(got.end(), data, data + read.batches[b].length);
    }
    std::cout << "read back " << got.size() << " of " << rows << " rows\n";
    if (rows != ids.size() || got != ids) {
        return fail("the rows read back differ from the rows written");
    }
    return 0;
}
