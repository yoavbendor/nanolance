// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The streaming read path, driven through the C ABI the Python bindings use. The Python tests cover
// the same ground, but they do not run under ASan/UBSan -- and this hands an ArrowArrayStream's
// ownership across a C boundary, which is exactly what sanitizers are for.

#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

void require(bool ok, const std::string& msg) {
    require(ok, msg.c_str());
}

constexpr std::int64_t kRowsPerFragment = 500;
constexpr int kFragments = 4;

// Two columns so projection has something to drop, and a string column because the variable-width
// path is the one with the interesting state.
bool build_schema(ArrowSchema& schema) {
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, 2) != NANOARROW_OK) {
        return false;
    }
    return ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK &&
           ArrowSchemaSetName(schema.children[0], "id") == NANOARROW_OK &&
           ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_STRING) == NANOARROW_OK &&
           ArrowSchemaSetName(schema.children[1], "tag") == NANOARROW_OK;
}

bool build_batch(ArrowArray& array, const ArrowSchema& schema, std::int64_t first, std::int64_t count) {
    if (ArrowArrayInitFromSchema(&array, &schema, nullptr) != NANOARROW_OK) {
        return false;
    }
    if (ArrowArrayStartAppending(&array) != NANOARROW_OK) {
        ArrowArrayRelease(&array);
        return false;
    }
    for (std::int64_t i = first; i < first + count; ++i) {
        const std::string tag = "tag-" + std::to_string(i);
        ArrowStringView sv{tag.c_str(), static_cast<std::int64_t>(tag.size())};
        if (ArrowArrayAppendInt(array.children[0], i) != NANOARROW_OK ||
            ArrowArrayAppendString(array.children[1], sv) != NANOARROW_OK ||
            ArrowArrayFinishElement(&array) != NANOARROW_OK) {
            ArrowArrayRelease(&array);
            return false;
        }
    }
    return ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK;
}

// One fragment per commit, so the stream has several batches to hand back.
void write_dataset(const std::filesystem::path& path, const ArrowSchema& schema) {
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, path.string().c_str(), 0) == NANO_LANCE_OK,
            "writer init failed");
    for (int fragment = 0; fragment < kFragments; ++fragment) {
        ArrowArray batch{};
        require(build_batch(batch, schema, fragment * kRowsPerFragment, kRowsPerFragment),
                "batch build failed");
        ArrowSchema batch_schema{};
        require(ArrowSchemaDeepCopy(&schema, &batch_schema) == NANOARROW_OK, "schema copy failed");
        const int rc = nano_lance_write_batch(&writer, &batch, &batch_schema);
        require(rc == NANO_LANCE_OK, nano_lance_writer_last_error(&writer));
        ArrowArrayRelease(&batch);
        ArrowSchemaRelease(&batch_schema);
        require(nano_lance_writer_commit(&writer, fragment != 0) == NANO_LANCE_OK,
                nano_lance_writer_last_error(&writer));
    }
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "writer close failed");
}

std::int64_t drain(ArrowArrayStream& stream, std::int64_t* batches, std::int64_t first_id) {
    std::int64_t rows = 0;
    *batches = 0;
    for (;;) {
        ArrowArray batch{};
        require(stream.get_next(&stream, &batch) == 0, "get_next failed");
        if (batch.release == nullptr) {
            break;
        }
        // Check the values, not just the count: a stream that hands back the same fragment every
        // time would otherwise pass.
        const auto* ids = static_cast<const std::int64_t*>(batch.children[0]->buffers[1]) +
                          batch.children[0]->offset;
        require(ids[0] == first_id + rows, "batch delivered out of order or repeated");
        rows += batch.length;
        ++*batches;
        ArrowArrayRelease(&batch);
    }
    return rows;
}

}  // namespace

int main() {
    ArrowSchema schema{};
    require(build_schema(schema), "schema build failed");

    const auto path = std::filesystem::temp_directory_path() / "nano_lance_table_stream_test.lance";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    write_dataset(path, schema);

    char error[512] = {0};

    // Full read: one batch per fragment, in order, every row present.
    {
        ArrowArrayStream stream{};
        require(nano_lance_table_open_stream(path.string().c_str(), nullptr, 0, 1, &stream, error,
                                             sizeof(error)) == NANO_LANCE_OK,
                error);
        ArrowSchema out{};
        require(stream.get_schema(&stream, &out) == 0, "get_schema failed");
        require(out.n_children == 2, "stream schema lost a column");
        ArrowSchemaRelease(&out);
        // The C data interface permits get_schema more than once; each result is the caller's to
        // release, so the stream must hand out a fresh copy rather than the one it holds.
        require(stream.get_schema(&stream, &out) == 0, "second get_schema failed");
        ArrowSchemaRelease(&out);

        std::int64_t batches = 0;
        const auto rows = drain(stream, &batches, 0);
        require(batches == kFragments, "expected one batch per fragment");
        require(rows == kFragments * kRowsPerFragment, "stream lost rows");
        stream.release(&stream);
        require(stream.release == nullptr, "release must null itself");
    }

    // Projection reaches the stream too.
    {
        const char* columns[] = {"tag"};
        ArrowArrayStream stream{};
        require(nano_lance_table_open_stream(path.string().c_str(), columns, 1, 1, &stream, error,
                                             sizeof(error)) == NANO_LANCE_OK,
                error);
        ArrowSchema out{};
        require(stream.get_schema(&stream, &out) == 0, "projected get_schema failed");
        require(out.n_children == 1, "projection did not drop a column");
        require(std::strcmp(out.children[0]->name, "tag") == 0, "projection kept the wrong column");
        ArrowSchemaRelease(&out);
        std::int64_t rows = 0;
        for (;;) {
            ArrowArray batch{};
            require(stream.get_next(&stream, &batch) == 0, "projected get_next failed");
            if (batch.release == nullptr) {
                break;
            }
            rows += batch.length;
            ArrowArrayRelease(&batch);
        }
        require(rows == kFragments * kRowsPerFragment, "projected stream lost rows");
        stream.release(&stream);
    }

    // Abandoning a stream part-way must not leak: release after a single pull.
    {
        ArrowArrayStream stream{};
        require(nano_lance_table_open_stream(path.string().c_str(), nullptr, 0, 1, &stream, error,
                                             sizeof(error)) == NANO_LANCE_OK,
                error);
        ArrowArray batch{};
        require(stream.get_next(&stream, &batch) == 0, "partial get_next failed");
        require(batch.release != nullptr, "expected a first batch");
        ArrowArrayRelease(&batch);
        stream.release(&stream);
    }

    // A missing dataset reports through the error buffer rather than crashing or succeeding empty.
    {
        ArrowArrayStream stream{};
        const auto missing = path.string() + "-does-not-exist";
        require(nano_lance_table_open_stream(missing.c_str(), nullptr, 0, 1, &stream, error,
                                             sizeof(error)) != NANO_LANCE_OK,
                "opening a missing dataset must fail");
        require(error[0] != '\0', "a failed open must fill the error buffer");
        require(stream.release == nullptr, "a failed open must leave out_stream zeroed");
    }

    // An unknown projected column is refused by name, not silently dropped.
    {
        const char* columns[] = {"nope"};
        ArrowArrayStream stream{};
        require(nano_lance_table_open_stream(path.string().c_str(), columns, 1, 1, &stream, error,
                                             sizeof(error)) != NANO_LANCE_OK,
                "an unknown column must be refused");
        require(stream.release == nullptr, "a refused projection must leave out_stream zeroed");
    }

    ArrowSchemaRelease(&schema);
    std::filesystem::remove_all(path, ec);
    std::cout << "table stream ok\n";
    return 0;
}
