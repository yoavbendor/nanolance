// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/array_accessor.hpp"
#include "nanolance/schema_mapper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

// NB: `require(f(error), error)` is a trap. The two arguments are evaluated in unspecified
// order, so c_str() can capture a pointer into the EMPTY string before f() runs; when f() then fails
// and assigns a long message, the string reallocates and that pointer dangles. Real failures printed
// a stray letter instead of their message. Pass the string itself.
void require(bool ok, const std::string& message) {
    require(ok, message.c_str());
}

bool build_two_column_schema(ArrowSchema& schema) {
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, 2) != NANOARROW_OK) {
        return false;
    }
    if (ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_UINT64) != NANOARROW_OK ||
        ArrowSchemaSetName(schema.children[0], "id") != NANOARROW_OK) {
        return false;
    }
    if (ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_STRING) != NANOARROW_OK ||
        ArrowSchemaSetName(schema.children[1], "tag") != NANOARROW_OK) {
        return false;
    }
    schema.flags = 0;
    schema.children[0]->flags = 0;
    schema.children[1]->flags = 0;
    return true;
}

bool append_row(ArrowArray& root, std::uint64_t id, const char* tag, std::int64_t tag_len) {
    auto* id_col = root.children[0];
    auto* tag_col = root.children[1];
    if (tag == nullptr) {
        if (ArrowArrayAppendNull(id_col, 1) != NANOARROW_OK || ArrowArrayAppendNull(tag_col, 1) != NANOARROW_OK) {
            return false;
        }
    } else {
        if (ArrowArrayAppendUInt(id_col, id) != NANOARROW_OK) {
            return false;
        }
        ArrowStringView sv{tag, tag_len};
        if (ArrowArrayAppendString(tag_col, sv) != NANOARROW_OK) {
            return false;
        }
    }
    return ArrowArrayFinishElement(&root) == NANOARROW_OK;
}

// A pyarrow `Table.to_batches()` batch is a VIEW: its columns share one contiguous buffer and address
// their rows through ArrowArray::offset. Model that here without copying -- shallow struct copies
// that borrow the source batch's buffers. The copies must NOT be released; `source` owns everything.
struct SlicedBatch {
    ArrowArray root{};
    ArrowArray children[2]{};
    ArrowArray* child_ptrs[2]{};
};

void slice_batch(const ArrowArray& source, std::int64_t offset, std::int64_t length, SlicedBatch& out) {
    out.root = source;
    for (int i = 0; i < 2; ++i) {
        out.children[i] = *source.children[i];
        out.children[i].offset += offset;
        out.children[i].length = length;
        out.child_ptrs[i] = &out.children[i];
    }
    out.root.children = out.child_ptrs;
    out.root.length = length;
}

std::uint64_t offset_at(const nano_lance::ColumnValues& column, std::size_t index) {
    std::int32_t value = 0;
    std::memcpy(&value, column.variable.offsets.data() + index * sizeof(std::int32_t), sizeof(value));
    return static_cast<std::uint64_t>(value);
}

bool build_batch(ArrowArray& array, const ArrowSchema& schema, const std::vector<std::pair<std::uint64_t, const char*>>& rows) {
    if (ArrowArrayInitFromSchema(&array, &schema, nullptr) != NANOARROW_OK) {
        return false;
    }
    if (ArrowArrayStartAppending(&array) != NANOARROW_OK) {
        ArrowArrayRelease(&array);
        return false;
    }
    for (const auto& [id, tag] : rows) {
        const auto len = tag == nullptr ? 0 : static_cast<std::int64_t>(std::strlen(tag));
        if (!append_row(array, id, tag, len)) {
            ArrowArrayRelease(&array);
            return false;
        }
    }
    return ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK;
}

}  // namespace

int main() {
    std::string error;
    ArrowSchema schema{};
    require(build_two_column_schema(schema), "schema init failed");

    nano_lance::LanceSchemaMapping mapping;
    require(nano_lance::map_arrow_schema(schema, mapping, error, true), error);
    const auto physical = nano_lance::lance_physical_fields(mapping);
    require(physical.size() == 2, "expected two physical columns");

    std::vector<nano_lance::ColumnValues> columns;

    ArrowArray batch1{};
    require(build_batch(batch1, schema, {{1, "aa"}, {2, "b"}}), "batch1 build failed");
    require(nano_lance::append_batch_column_values(batch1, mapping, columns, error), error);
    ArrowArrayRelease(&batch1);

    require(columns[0].kind == nano_lance::ColumnValues::Kind::FixedWidth, "id column not fixed");
    require(columns[0].fixed.size() == 16, "id fixed width size mismatch");
    require(columns[1].kind == nano_lance::ColumnValues::Kind::VariableWidth, "tag column not variable");
    require(columns[1].variable.data.size() == 3, "tag data after batch1");

    ArrowArray batch2{};
    require(build_batch(batch2, schema, {{3, "xyz"}}), "batch2 build failed");
    require(nano_lance::append_batch_column_values(batch2, mapping, columns, error), error);
    ArrowArrayRelease(&batch2);

    require(columns[0].fixed.size() == 24, "id fixed width after batch2");
    require(columns[1].variable.data.size() == 6, "tag data after batch2");

    // --- Sliced batches -------------------------------------------------------------------------
    // The variable-width path used to ignore ArrowArray::offset while the fixed-width path applied
    // it, so every batch after the first re-ingested the FIRST batch's offsets and data. That is
    // exactly what `Table.to_batches()` produces, so multi-batch utf8/binary columns were silently
    // corrupted from row `chunksize` on -- and stock Lance read back the same wrong bytes, because
    // the file on disk was wrong. These assertions are the core-library guard for that.
    ArrowArray source{};
    require(build_batch(source, schema, {{1, "aa"}, {2, "b"}, {3, "xyz"}, {4, "dddd"}}),
            "source build failed");

    SlicedBatch tail_rows;
    slice_batch(source, 2, 2, tail_rows);

    // First append: a slice whose offset is non-zero has to be rebased to row 0, not copied verbatim.
    std::vector<nano_lance::ColumnValues> sliced;
    require(nano_lance::append_batch_column_values(tail_rows.root, mapping, sliced, error), error);
    require(sliced[1].variable.data.size() == 7, "sliced first batch took the wrong data range");
    require(std::memcmp(sliced[1].variable.data.data(), "xyzdddd", 7) == 0,
            "sliced first batch ingested the wrong rows");
    require(sliced[1].variable.offsets.size() == 3 * sizeof(std::int32_t), "sliced offsets count");
    require(offset_at(sliced[1], 0) == 0, "sliced offsets must start at 0");
    require(offset_at(sliced[1], 1) == 3 && offset_at(sliced[1], 2) == 7, "sliced offsets not rebased");
    require(sliced[0].fixed.size() == 16, "sliced fixed column took the wrong row range");

    // Second append of the same slice: the accumulating path must rebase onto what is already held.
    require(nano_lance::append_batch_column_values(tail_rows.root, mapping, sliced, error), error);
    require(sliced[1].variable.data.size() == 14, "accumulated sliced data size");
    require(std::memcmp(sliced[1].variable.data.data(), "xyzddddxyzdddd", 14) == 0,
            "accumulated sliced data contents");
    require(sliced[1].variable.offsets.size() == 5 * sizeof(std::int32_t), "accumulated offsets count");
    require(offset_at(sliced[1], 3) == 10 && offset_at(sliced[1], 4) == 14,
            "accumulated offsets not rebased onto the existing data");

    ArrowArrayRelease(&source);
    ArrowSchemaRelease(&schema);
    return 0;
}
