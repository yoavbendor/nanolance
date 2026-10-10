// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Stable row ids through lance-c's C API: lance_dataset_write_with_params(enable_stable_row_ids),
// the `_rowid` column surviving delete, update and compaction, and lance_dataset_take_rows /
// lance_dataset_take_blobs-style lookups by row id (an id that names no row is left out).

#include <nanoarrow/nanoarrow.h>

#include "lance/lance.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void make_batch(int64_t n, ArrowSchema& schema, ArrowArray& array) {
    ArrowSchemaInit(&schema);
    ArrowSchemaSetTypeStruct(&schema, 2);
    ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64);
    ArrowSchemaSetName(schema.children[0], "id");
    ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_INT64);
    ArrowSchemaSetName(schema.children[1], "v");
    ArrowArrayInitFromSchema(&array, &schema, nullptr);
    ArrowArrayStartAppending(&array);
    for (int64_t i = 0; i < n; ++i) {
        ArrowArrayAppendInt(array.children[0], i);
        ArrowArrayAppendInt(array.children[1], i * 10);
        ArrowArrayFinishElement(&array);
    }
    ArrowArrayFinishBuildingDefault(&array, nullptr);
}

/// id -> (_rowid, v) of every row, by a scan with row ids.
std::map<int64_t, std::pair<uint64_t, int64_t>> rows_of(LanceDataset* ds) {
    std::map<int64_t, std::pair<uint64_t, int64_t>> out;
    LanceScanner* scanner = lance_scanner_new(ds, nullptr, nullptr);
    lance_scanner_with_row_id(scanner, true);
    ArrowArrayStream stream{};
    if (lance_scanner_to_arrow_stream(scanner, &stream) != 0) {
        check(false, "scan");
        lance_scanner_close(scanner);
        return out;
    }
    ArrowSchema schema;
    stream.get_schema(&stream, &schema);
    for (;;) {
        ArrowArray batch{};
        stream.get_next(&stream, &batch);
        if (batch.release == nullptr) {
            break;
        }
        ArrowArrayView view;
        ArrowArrayViewInitFromSchema(&view, &schema, nullptr);
        ArrowArrayViewSetArray(&view, &batch, nullptr);
        for (int64_t r = 0; r < batch.length; ++r) {
            out[ArrowArrayViewGetIntUnsafe(view.children[0], r)] = {
                ArrowArrayViewGetUIntUnsafe(view.children[2], r), ArrowArrayViewGetIntUnsafe(view.children[1], r)};
        }
        ArrowArrayViewReset(&view);
        batch.release(&batch);
    }
    schema.release(&schema);
    stream.release(&stream);
    lance_scanner_close(scanner);
    return out;
}

std::vector<int64_t> take_ids(LanceDataset* ds, const std::vector<uint64_t>& row_ids) {
    std::vector<int64_t> out;
    const char* columns[] = {"id", nullptr};
    ArrowArrayStream stream{};
    if (lance_dataset_take_rows(ds, row_ids.data(), row_ids.size(), columns, &stream) != 0) {
        check(false, "take_rows");
        return out;
    }
    ArrowSchema schema;
    stream.get_schema(&stream, &schema);
    for (;;) {
        ArrowArray batch{};
        stream.get_next(&stream, &batch);
        if (batch.release == nullptr) {
            break;
        }
        ArrowArrayView view;
        ArrowArrayViewInitFromSchema(&view, &schema, nullptr);
        ArrowArrayViewSetArray(&view, &batch, nullptr);
        for (int64_t r = 0; r < batch.length; ++r) {
            out.push_back(ArrowArrayViewGetIntUnsafe(view.children[0], r));
        }
        ArrowArrayViewReset(&view);
        batch.release(&batch);
    }
    schema.release(&schema);
    stream.release(&stream);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string uri = (argc > 1 ? std::string(argv[1]) : std::string("lance_c_stable_tmp")) + "/d.lance";
    std::filesystem::remove_all(uri);
    std::filesystem::create_directories(std::filesystem::path(uri).parent_path());

    ArrowSchema schema;
    ArrowArray array;
    make_batch(12, schema, array);
    ArrowArrayStream stream;
    ArrowBasicArrayStreamInit(&stream, &schema, 1);
    ArrowBasicArrayStreamSetArray(&stream, 0, &array);
    ArrowSchema schema2;
    ArrowSchemaDeepCopy(&schema, &schema2);
    LanceWriteParams params{};
    params.max_rows_per_file = 5;
    params.enable_stable_row_ids = true;
    LanceDataset* ds = nullptr;
    const int32_t rc = lance_dataset_write_with_params(uri.c_str(), &schema2, &stream, LANCE_WRITE_CREATE, &params,
                                                       nullptr, &ds);
    schema2.release(&schema2);
    check(rc == 0 && ds != nullptr, "write with stable row ids");
    if (ds == nullptr) {
        return 1;
    }

    const auto before = rows_of(ds);
    check(before.size() == 12U, "12 rows");
    for (const auto& [id, row] : before) {
        check(row.first == static_cast<uint64_t>(id), "ids start as 0..11 in row order");
    }

    uint64_t deleted = 0;
    check(lance_dataset_delete(ds, "id = 3", &deleted) == 0 && deleted == 1U, "delete");
    const char* cols[] = {"v"};
    const char* vals[] = {"-1"};
    uint64_t updated = 0;
    check(lance_dataset_update(ds, "id = 7", cols, vals, 1, &updated) == 0 && updated == 1U, "update");
    LanceCompactionOptions options{};
    options.target_rows_per_fragment = 100;
    LanceCompactionMetrics metrics{};
    check(lance_dataset_compact_files(ds, &options, &metrics) == 0, "compact");

    const auto after = rows_of(ds);
    check(after.size() == 11U && after.count(3) == 0U, "the deleted row is gone");
    for (const auto& [id, row] : after) {
        check(before.at(id).first == row.first, "row " + std::to_string(id) + " keeps its row id");
    }
    check(after.at(7).second == -1, "the update shows");

    // Lookups by row id: the order asked for, repeats kept, ids of no row (or a deleted one) left out.
    const auto taken = take_ids(ds, {before.at(10).first, 99999U, before.at(3).first, before.at(7).first,
                                     before.at(10).first});
    check((taken == std::vector<int64_t>{10, 7, 10}), "take_rows by stable row id");

    lance_dataset_close(ds);
    std::filesystem::remove_all(std::filesystem::path(uri).parent_path());
    return failures == 0 ? 0 : 1;
}
