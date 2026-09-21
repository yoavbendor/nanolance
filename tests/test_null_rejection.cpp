// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Nulls are now STORED, not refused: ingest captures a validity bitmap per column. These cases cover
// the capture logic's edges, which are the same ones the old rejection had to get right -- the
// byte-aligned fast scan, an unaligned window via ArrowArray::offset, an unset `null_count`, and a
// null on a *parent* struct, which makes every child row null without any child-level validity bit
// being clear -- plus the ones only accumulation introduces: a bitmap that must materialize lazily
// when the first null arrives in a later batch, and bits that must land at the right absolute row.

#include "nanolance/array_accessor.hpp"
#include "nanolance/schema_mapper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

/// One nullable int64 column named "v". `nulls_at` lists the rows to append as null.
bool build_int64_batch(ArrowArray& batch, ArrowSchema& schema, std::int64_t rows,
                       const std::vector<std::int64_t>& nulls_at) {
    if (ArrowArrayInitFromSchema(&batch, &schema, nullptr) != NANOARROW_OK ||
        ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
        return false;
    }
    for (std::int64_t i = 0; i < rows; ++i) {
        bool is_null = false;
        for (const auto n : nulls_at) {
            is_null = is_null || n == i;
        }
        auto* col = batch.children[0];
        const auto rc = is_null ? ArrowArrayAppendNull(col, 1) : ArrowArrayAppendInt(col, i);
        if (rc != NANOARROW_OK || ArrowArrayFinishElement(&batch) != NANOARROW_OK) {
            return false;
        }
    }
    return ArrowArrayFinishBuildingDefault(&batch, nullptr) == NANOARROW_OK;
}

bool build_int64_schema(ArrowSchema& schema) {
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, 1) != NANOARROW_OK ||
        ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) != NANOARROW_OK ||
        ArrowSchemaSetName(schema.children[0], "v") != NANOARROW_OK) {
        return false;
    }
    schema.flags = 0;
    schema.children[0]->flags = ARROW_FLAG_NULLABLE;
    return true;
}

/// Ingest `batch` and report whether it was accepted, capturing the error text.
bool ingest(const ArrowArray& batch, const nano_lance::LanceSchemaMapping& mapping, std::string& error) {
    std::vector<nano_lance::ColumnValues> columns;
    return nano_lance::append_batch_column_values(batch, mapping, columns, error);
}

/// Ingest and return the resulting column, so tests can inspect the captured validity.
nano_lance::ColumnValues ingest_column(const ArrowArray& batch,
                                       const nano_lance::LanceSchemaMapping& mapping,
                                       const std::string& what) {
    std::vector<nano_lance::ColumnValues> columns;
    std::string error;
    require(nano_lance::append_batch_column_values(batch, mapping, columns, error), what + ": " + error);
    require(columns.size() == 1U, what + ": expected exactly one column");
    return std::move(columns[0]);
}

bool row_valid(const nano_lance::ColumnValues& col, std::uint64_t row) {
    if (col.validity.empty()) {
        return true;  // no bitmap means every row is valid
    }
    return ((col.validity[static_cast<std::size_t>(row >> 3U)] >> (row & 7U)) & 1U) != 0U;
}

/// Require exactly the rows in `null_rows` to be null, and nothing else.
void expect_nulls_at(const nano_lance::ColumnValues& col, std::uint64_t rows,
                     const std::vector<std::uint64_t>& null_rows, const std::string& what) {
    require(col.null_count == null_rows.size(),
            what + ": null_count is " + std::to_string(col.null_count) + ", expected " +
                std::to_string(null_rows.size()));
    for (std::uint64_t row = 0; row < rows; ++row) {
        const bool should_be_null =
            std::find(null_rows.begin(), null_rows.end(), row) != null_rows.end();
        require(row_valid(col, row) == !should_be_null,
                what + ": row " + std::to_string(row) + " should be " +
                    (should_be_null ? "null" : "valid"));
    }
}

}  // namespace

int main() {
    ArrowSchema schema{};
    require(build_int64_schema(schema), "schema build failed");

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(schema, mapping, error, /*ignore_nullability=*/true), error);

    // A column with no nulls carries NO bitmap at all -- the common case must stay free.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 64, {}), "all-valid batch build failed");
        const auto col = ingest_column(batch, mapping, "no nulls");
        require(col.validity.empty(), "an all-valid column should carry no validity bitmap");
        require(col.null_count == 0U, "an all-valid column should report no nulls");
        require(col.rows == 64U, "row count mismatch");
        ArrowArrayRelease(&batch);
    }

    // A single null anywhere is captured at the right row.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 3, {1}), "batch build failed");
        expect_nulls_at(ingest_column(batch, mapping, "null in the middle"), 3, {1}, "null in the middle");
        ArrowArrayRelease(&batch);
    }
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 3, {0}), "batch build failed");
        expect_nulls_at(ingest_column(batch, mapping, "null at row 0"), 3, {0}, "null at row 0");
        ArrowArrayRelease(&batch);
    }

    // Past the first validity byte and past an 8-row boundary, so the scan's aligned body and its
    // tail are both exercised.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 100, {99}), "batch build failed");
        expect_nulls_at(ingest_column(batch, mapping, "null in the tail"), 100, {99}, "null in the tail");
        ArrowArrayRelease(&batch);
    }
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 100, {40}), "batch build failed");
        expect_nulls_at(ingest_column(batch, mapping, "null on a byte boundary"), 100, {40},
                        "null on a byte boundary");
        ArrowArrayRelease(&batch);
    }

    // ArrowArray::offset shifts the validity window: a null BEFORE the window is not this array's
    // row, and one inside it lands at the window-relative index.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 16, {2}), "batch build failed");
        batch.children[0]->offset = 5;
        batch.children[0]->length = 11;
        batch.children[0]->null_count = -1;
        batch.length = 11;
        const auto col = ingest_column(batch, mapping, "null before an offset window");
        require(col.null_count == 0U, "a null outside the offset window must not be captured");
        ArrowArrayRelease(&batch);
    }
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 16, {9}), "batch build failed");
        batch.children[0]->offset = 5;   // rows 5..15; the null at 9 is row 4 of the window
        batch.children[0]->length = 11;
        batch.children[0]->null_count = -1;
        batch.length = 11;
        expect_nulls_at(ingest_column(batch, mapping, "null inside an offset window"), 11, {4},
                        "null inside an offset window");
        ArrowArrayRelease(&batch);
    }

    // null_count == -1 means "not computed"; the bitmap is then the only source of truth.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 20, {7}), "batch build failed");
        batch.children[0]->null_count = -1;
        expect_nulls_at(ingest_column(batch, mapping, "unset null_count"), 20, {7}, "unset null_count");
        ArrowArrayRelease(&batch);
    }

    // An array with no validity buffer is all-valid no matter what null_count claims.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 8, {}), "batch build failed");
        batch.children[0]->null_count = 3;  // malformed: claims nulls with no bitmap to back them
        const auto col = ingest_column(batch, mapping, "claimed nulls with no validity buffer");
        require(col.null_count == 0U, "a missing validity buffer means every row is valid");
        ArrowArrayRelease(&batch);
    }

    // Accumulation across batches: the bitmap must materialize when the FIRST null appears in a
    // later batch, back-filling the earlier rows as valid, and land the new bits at absolute rows.
    {
        std::vector<nano_lance::ColumnValues> columns;
        ArrowArray first{};
        require(build_int64_batch(first, schema, 10, {}), "first batch build failed");
        require(nano_lance::append_batch_column_values(first, mapping, columns, error), error);
        ArrowArrayRelease(&first);
        require(columns[0].validity.empty(), "no bitmap should exist while every row is valid");

        ArrowArray second{};
        require(build_int64_batch(second, schema, 10, {3}), "second batch build failed");
        require(nano_lance::append_batch_column_values(second, mapping, columns, error), error);
        ArrowArrayRelease(&second);

        require(columns[0].rows == 20U, "rows should accumulate across batches");
        expect_nulls_at(columns[0], 20, {13}, "null in a later batch");
    }

    ArrowSchemaRelease(&schema);
    std::cout << "null capture: ok\n";
    return 0;
}
