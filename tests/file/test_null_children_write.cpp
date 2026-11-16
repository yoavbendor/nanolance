// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Null-typed children of lists and structs (W1k5): a list<null> with an empty list, a list of nulls, and a null list,
// and a struct<a: null, b: int64> whose null child is stored as its definition levels only. Written with
// nano_lance::Writer, read back with the C table reader, and checked level by level: the list's offsets and validity,
// the null child's length, and the int64 sibling.

#include "nanolance/nano_lance_reader.h"
#include "nanolance/writer.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& what) {
    if (!ok) {
        throw std::runtime_error(what);
    }
}

/// A fresh directory, removed when the test ends however it ends.
class ScratchDir {
  public:
    ScratchDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("nano_lance_null_children_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        require(std::filesystem::create_directory(path_), "scratch directory already exists");
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    [[nodiscard]] std::filesystem::path path() const { return path_; }

  private:
    std::filesystem::path path_;
};

constexpr std::int64_t kRows = 4;

/// Root struct of three children: "l" list<null>, "s" struct<a: null, b: int64>, "id" int64.
/// Rows: l = [], [null], null, [null, null]; s = {a: null, b: 1..4}; id = 0..3.
struct Batch {
    ArrowSchema schema{};
    ArrowArray array{};
    Batch() {
        ArrowSchemaInit(&schema);
        require(ArrowSchemaSetTypeStruct(&schema, 3) == NANOARROW_OK, "root struct");
        ArrowSchema* list = schema.children[0];
        require(ArrowSchemaInitFromType(list, NANOARROW_TYPE_LIST) == NANOARROW_OK, "list type");
        require(ArrowSchemaSetName(list, "l") == NANOARROW_OK, "list name");
        require(ArrowSchemaSetType(list->children[0], NANOARROW_TYPE_NA) == NANOARROW_OK, "null item type");
        require(ArrowSchemaSetName(list->children[0], "item") == NANOARROW_OK, "item name");

        ArrowSchema* st = schema.children[1];
        require(ArrowSchemaSetTypeStruct(st, 2) == NANOARROW_OK, "struct type");
        require(ArrowSchemaSetName(st, "s") == NANOARROW_OK, "struct name");
        require(ArrowSchemaSetType(st->children[0], NANOARROW_TYPE_NA) == NANOARROW_OK, "null field type");
        require(ArrowSchemaSetName(st->children[0], "a") == NANOARROW_OK, "null field name");
        require(ArrowSchemaSetType(st->children[1], NANOARROW_TYPE_INT64) == NANOARROW_OK, "b type");
        require(ArrowSchemaSetName(st->children[1], "b") == NANOARROW_OK, "b name");

        require(ArrowSchemaSetType(schema.children[2], NANOARROW_TYPE_INT64) == NANOARROW_OK, "id type");
        require(ArrowSchemaSetName(schema.children[2], "id") == NANOARROW_OK, "id name");

        require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array init");
        require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start appending");

        // Row 0: an empty list. Row 1: a list of one null. Row 2: a null list. Row 3: a list of two nulls.
        ArrowArray* l = array.children[0];
        ArrowArray* item = l->children[0];
        ArrowArray* s = array.children[1];
        for (std::int64_t i = 0; i < kRows; ++i) {
            if (i == 0) {
                require(ArrowArrayAppendEmpty(l, 1) == NANOARROW_OK, "row 0 list");
            } else if (i == 2) {
                require(ArrowArrayAppendNull(l, 1) == NANOARROW_OK, "row 2 null list");
            } else {
                require(ArrowArrayAppendNull(item, i == 1 ? 1 : 2) == NANOARROW_OK, "list items");
                require(ArrowArrayFinishElement(l) == NANOARROW_OK, "list element");
            }
            // The struct: its null child holds no values, b counts 1..4.
            require(ArrowArrayAppendNull(s->children[0], 1) == NANOARROW_OK, "null field");
            require(ArrowArrayAppendInt(s->children[1], 1 + i) == NANOARROW_OK, "b value");
            require(ArrowArrayFinishElement(s) == NANOARROW_OK, "struct element");
            require(ArrowArrayAppendInt(array.children[2], i) == NANOARROW_OK, "id value");
            require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "root element");
        }
        require(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK, "finish array");
    }
    ~Batch() {
        ArrowArrayRelease(&array);
        ArrowSchemaRelease(&schema);
    }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
};

bool bit(const void* bitmap, std::int64_t i) {
    return (static_cast<const std::uint8_t*>(bitmap)[i >> 3] >> (i & 7)) & 1U;
}

void check_read_back(const std::filesystem::path& dataset) {
    ArrowSchema schema{};
    ArrowArray* batches = nullptr;
    size_t count = 0;
    char err[512]{};
    const int rc = nano_lance_table_read_dataset(dataset.string().c_str(), &schema, &batches, &count, err, sizeof err);
    require(rc == NANO_LANCE_READER_OK, std::string("read back: ") + err);

    std::int64_t rows = 0;
    for (size_t b = 0; b < count; ++b) {
        rows += batches[b].length;
    }
    require(rows == kRows, "row count " + std::to_string(rows));
    require(count == 1, "a single fragment is expected for this batch");

    const ArrowArray& root = batches[0];
    const ArrowArray& l = *root.children[0];
    const ArrowArray& item = *l.children[0];
    require(l.length == kRows, "list length");
    require(item.length == 3, "the null items are 0 + 1 + 0 + 2 = 3 entries");

    // Offsets: [], [null], null, [null, null].
    const auto* offsets = static_cast<const std::int32_t*>(l.buffers[1]) + l.offset;
    const std::int32_t expected_offsets[] = {0, 0, 1, 1, 3};
    for (int i = 0; i < 5; ++i) {
        require(offsets[i] == expected_offsets[i], "list offset " + std::to_string(i));
    }
    require(l.buffers[0] != nullptr && bit(l.buffers[0], 0) && !bit(l.buffers[0], 2), "row 2 is a null list");

    // The struct's null child holds no values and the int64 beside it counts 1..4.
    const ArrowArray& s = *root.children[1];
    require(s.length == kRows, "struct length");
    require(s.children[0]->length == kRows, "null child length");
    const auto* b = static_cast<const std::int64_t*>(s.children[1]->buffers[1]) + s.children[1]->offset;
    for (std::int64_t i = 0; i < kRows; ++i) {
        require(b[i] == 1 + i, "b value " + std::to_string(i));
    }
    const auto* id = static_cast<const std::int64_t*>(root.children[2]->buffers[1]) + root.children[2]->offset;
    for (std::int64_t i = 0; i < kRows; ++i) {
        require(id[i] == i, "id value " + std::to_string(i));
    }

    nano_lance_table_read_result_free(&schema, batches, count);
}

void test_round_trip(const std::filesystem::path& dir) {
    Batch batch;
    nano_lance::Writer writer;
    const auto dataset = dir / "nulls.lance";
    require(writer.open(dataset), std::string("open: ") + writer.error());
    ArrowSchema schema_copy{};
    require(ArrowSchemaDeepCopy(&batch.schema, &schema_copy) == NANOARROW_OK, "schema copy");
    const bool written = writer.write_batch(&batch.array, &schema_copy);
    ArrowSchemaRelease(&schema_copy);
    require(written, std::string("write_batch: ") + writer.error());
    require(writer.commit(false), std::string("commit: ") + writer.error());
    require(writer.close(), "close");
    check_read_back(dataset);
}

}  // namespace

int main() {
    ScratchDir scratch;
    test_round_trip(scratch.path());
    std::cout << "null_children_write: ok\n";
    return 0;
}
