// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// liblance_c's dataset calls (lance-c's C API, compat/lance-c/include/lance/lance.h), the ones the search and
// stable-row-id tests do not drive: write (create, append, refused create), open at the latest and at a version,
// the metadata calls, version history, data statistics, a scan with a filter, offset and limit, take, the column
// changes (add from SQL, rename, drop), delete, and restore. Errors: an open of a path with no dataset, and the
// error code and message lance-c's thread-local error carries. lance-c's own suite (tools/lance_c_suite.py) runs
// its tests against the same library; this one is a fast ctest of the same calls.

#include <nanoarrow/nanoarrow.h>  // the Arrow C structs, before lance.h

#include "lance/lance.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

/// A stream of one batch: an int64 column "id" holding [first, first + n).
void make_stream(std::int64_t first, std::int64_t n, ArrowSchema& schema, ArrowArrayStream& stream) {
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, 1) == NANOARROW_OK &&
                ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK &&
                ArrowSchemaSetName(schema.children[0], "id") == NANOARROW_OK,
            "schema");
    ArrowArray array{};
    require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array");
    require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start");
    for (std::int64_t i = 0; i < n; ++i) {
        require(ArrowArrayAppendInt(array.children[0], first + i) == NANOARROW_OK, "append");
        require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "element");
    }
    require(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK, "finish");
    ArrowSchema copy{};
    require(ArrowSchemaDeepCopy(&schema, &copy) == NANOARROW_OK, "copy");
    require(ArrowBasicArrayStreamInit(&stream, &copy, 1) == NANOARROW_OK, "stream");
    ArrowBasicArrayStreamSetArray(&stream, 0, &array);
}

std::string last_error() {
    const char* message = lance_last_error_message();
    std::string out = message == nullptr ? "" : message;
    lance_free_string(message);
    return out;
}

/// The int64 values of column `name` in every batch of `stream` (released here).
std::vector<std::int64_t> drain(ArrowArrayStream& stream, const std::string& name) {
    ArrowSchema schema{};
    require(stream.get_schema(&stream, &schema) == 0, "get_schema");
    std::int64_t index = -1;
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (name == schema.children[i]->name) {
            index = i;
        }
    }
    require(index >= 0, "the stream has a column " + name);
    std::vector<std::int64_t> out;
    for (;;) {
        ArrowArray batch{};
        require(stream.get_next(&stream, &batch) == 0, "get_next");
        if (batch.release == nullptr) {
            break;
        }
        const ArrowArray* col = batch.children[index];
        for (std::int64_t r = 0; r < col->length; ++r) {
            out.push_back(static_cast<const std::int64_t*>(col->buffers[1])[col->offset + r]);
        }
        batch.release(&batch);
    }
    schema.release(&schema);
    stream.release(&stream);
    return out;
}

std::vector<std::int64_t> scan(LanceDataset* ds, const char* filter, std::int64_t offset = 0, std::int64_t limit = -1,
                               const std::string& column = "id") {
    LanceScanner* scanner = lance_scanner_new(ds, nullptr, filter);
    require(scanner != nullptr, "scanner: " + last_error());
    if (offset > 0) {
        require(lance_scanner_set_offset(scanner, offset) == 0, "offset");
    }
    if (limit >= 0) {
        require(lance_scanner_set_limit(scanner, limit) == 0, "limit");
    }
    ArrowArrayStream stream{};
    require(lance_scanner_to_arrow_stream(scanner, &stream) == 0, "to_arrow_stream: " + last_error());
    lance_scanner_close(scanner);
    return drain(stream, column);
}

std::vector<std::string> names(LanceDataset* ds) {
    ArrowSchema schema{};
    require(lance_dataset_schema(ds, &schema) == 0, "schema: " + last_error());
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        out.emplace_back(schema.children[i]->name);
    }
    schema.release(&schema);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path root = argc > 1 ? std::filesystem::path(argv[1])
                                                : std::filesystem::temp_directory_path() / "nano_lance_lance_c_dataset";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    const std::string uri = (root / "ds.lance").string();

    // No dataset yet: open fails with an error code and message.
    require(lance_dataset_open(uri.c_str(), nullptr, 0) == nullptr, "open of nothing fails");
    require(lance_last_error_code() != LANCE_OK && !last_error().empty(), "with an error set");

    // Create, then append: versions 1 and 2.
    ArrowSchema schema{};
    ArrowArrayStream stream{};
    make_stream(0, 5, schema, stream);
    LanceDataset* ds = nullptr;
    require(lance_dataset_write(uri.c_str(), &schema, &stream, LANCE_WRITE_CREATE, nullptr, &ds) == 0 && ds != nullptr,
            "create: " + last_error());
    schema.release(&schema);
    require(lance_dataset_version(ds) == 1 && lance_dataset_count_rows(ds) == 5, "version 1, five rows");
    lance_dataset_close(ds);

    make_stream(0, 1, schema, stream);
    require(lance_dataset_write(uri.c_str(), &schema, &stream, LANCE_WRITE_CREATE, nullptr, nullptr) != 0 &&
                lance_last_error_code() == LANCE_ERR_DATASET_ALREADY_EXISTS,
            "create over a dataset is refused");
    schema.release(&schema);
    if (stream.release != nullptr) {
        stream.release(&stream);
    }

    make_stream(5, 5, schema, stream);
    require(lance_dataset_write(uri.c_str(), &schema, &stream, LANCE_WRITE_APPEND, nullptr, nullptr) == 0,
            "append: " + last_error());
    schema.release(&schema);

    ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    require(ds != nullptr, "open: " + last_error());
    require(lance_dataset_version(ds) == 2 && lance_dataset_latest_version(ds) == 2, "latest is version 2");
    require(lance_dataset_count_rows(ds) == 10 && lance_dataset_fragment_count(ds) == 2, "ten rows, two fragments");
    LanceDataset* v1 = lance_dataset_open(uri.c_str(), nullptr, 1);
    require(v1 != nullptr && lance_dataset_count_rows(v1) == 5, "version 1 opens with its five rows");
    lance_dataset_close(v1);

    // Version history.
    LanceVersions* versions = lance_dataset_versions(ds);
    require(versions != nullptr && lance_versions_count(versions) == 2 && lance_versions_id_at(versions, 0) == 1 &&
                lance_versions_id_at(versions, 1) == 2,
            "two versions, oldest first");
    lance_versions_close(versions);

    // Data statistics: one field, some bytes.
    LanceDataStatistics* stats = lance_dataset_calculate_data_stats(ds);
    require(stats != nullptr && lance_data_statistics_count(stats) == 1 &&
                lance_data_statistics_bytes_on_disk_at(stats, 0) > 0,
            "data stats for the one field");
    lance_data_statistics_close(stats);

    // Scans: everything, a filter, offset and limit over the rows that pass.
    require(scan(ds, nullptr) == std::vector<std::int64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, "a full scan");
    require(scan(ds, "id >= 3", 2, 3) == std::vector<std::int64_t>{5, 6, 7}, "filter, offset 2, limit 3");

    // Take: input order and repeats kept.
    {
        const std::uint64_t indices[3] = {7, 2, 7};
        ArrowArrayStream out{};
        require(lance_dataset_take(ds, indices, 3, nullptr, &out) == 0, "take: " + last_error());
        require(drain(out, "id") == std::vector<std::int64_t>{7, 2, 7}, "take keeps order and repeats");
    }

    // Column changes, each moving the handle to its version.
    const LanceSqlColumn added = {"dbl", "id * 2"};
    require(lance_dataset_add_columns_sql(ds, &added, 1, 0) == 0, "add_columns_sql: " + last_error());
    require(names(ds) == std::vector<std::string>{"id", "dbl"} && lance_dataset_version(ds) == 3, "dbl added");
    require(scan(ds, "id = 4", 0, -1, "dbl") == std::vector<std::int64_t>{8}, "dbl is id * 2");
    LanceColumnAlteration rename{};
    rename.path = "dbl";
    rename.rename = "twice";
    rename.nullable_mode = LANCE_COLUMN_NULLABLE_UNCHANGED;
    require(lance_dataset_alter_columns(ds, &rename, 1) == 0, "alter_columns: " + last_error());
    require(names(ds) == std::vector<std::string>{"id", "twice"}, "renamed");
    const char* drop[1] = {"twice"};
    require(lance_dataset_drop_columns(ds, drop, 1) == 0, "drop_columns: " + last_error());
    require(names(ds) == std::vector<std::string>{"id"}, "dropped");

    // Delete, then restore version 2.
    std::uint64_t deleted = 0;
    require(lance_dataset_delete(ds, "id < 4", &deleted) == 0 && deleted == 4, "delete: " + last_error());
    require(lance_dataset_count_rows(ds) == 6, "six rows left");
    const auto before = lance_dataset_version(ds);
    LanceDataset* restored = lance_dataset_restore(ds, 2);
    require(restored != nullptr, "restore: " + last_error());
    require(lance_dataset_version(restored) == before + 1 && lance_dataset_count_rows(restored) == 10,
            "restore commits the next version with version 2's rows");
    require(lance_dataset_restore(ds, 0) == nullptr && lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
            "restore to version 0 is refused");
    lance_dataset_close(restored);
    lance_dataset_close(ds);

    std::filesystem::remove_all(root, ec);
    std::cout << "lance_c dataset: OK\n";
    return 0;
}
