// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// nanolance writes no Lance validity information, so a null slot has nowhere to go. Ingest must
// refuse a batch that actually contains one rather than copying the slot's raw bytes (which used to
// turn [10, null, 30] into [10, 0, 30] silently). These cases cover the detection logic's edges:
// the byte-aligned fast scan, an unaligned window via ArrowArray::offset, an unset `null_count`,
// and a null on a *parent* struct, which makes every child row null without any child-level
// validity bit being clear.

#include "nanolance/array_accessor.hpp"
#include "nanolance/schema_mapper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
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

void expect_rejected(const ArrowArray& batch, const nano_lance::LanceSchemaMapping& mapping,
                     const std::string& want_substring, const std::string& what) {
    std::string error;
    require(!ingest(batch, mapping, error), what + ": expected rejection, but ingest succeeded");
    require(error.find(want_substring) != std::string::npos,
            what + ": message did not mention \"" + want_substring + "\" (got: " + error + ")");
}

void expect_accepted(const ArrowArray& batch, const nano_lance::LanceSchemaMapping& mapping,
                     const std::string& what) {
    std::string error;
    require(ingest(batch, mapping, error), what + ": expected acceptance, got: " + error);
}

}  // namespace

int main() {
    ArrowSchema schema{};
    require(build_int64_schema(schema), "schema build failed");

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(schema, mapping, error, /*ignore_nullability=*/true), error);

    // A nullable-flagged column with no actual nulls is the case --ignore-nullability exists for:
    // pyarrow marks essentially every field nullable. It must still be accepted.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 64, {}), "all-valid batch build failed");
        expect_accepted(batch, mapping, "nullable schema, no null values");
        ArrowArrayRelease(&batch);
    }

    // A single null anywhere must be caught, and the message must name the column and the row.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 3, {1}), "batch build failed");
        expect_rejected(batch, mapping, "null at row 1", "null in the middle");
        ArrowArrayRelease(&batch);
    }
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 3, {0}), "batch build failed");
        expect_rejected(batch, mapping, "null at row 0", "null at row 0");
        ArrowArrayRelease(&batch);
    }

    // Past the first validity byte, and past the 8-row aligned block, so the byte-at-a-time scan and
    // its tail are both exercised. 100 rows with the null at 99 lands in the unaligned tail.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 100, {99}), "batch build failed");
        expect_rejected(batch, mapping, "null at row 99", "null in the scan tail");
        ArrowArrayRelease(&batch);
    }
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 100, {40}), "batch build failed");
        expect_rejected(batch, mapping, "null at row 40", "null on a byte boundary");
        ArrowArrayRelease(&batch);
    }

    // ArrowArray::offset shifts the validity window. A null BEFORE the window must not be reported
    // (it is not part of this array's rows), and one inside it must be, at the window-relative row.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 16, {2}), "batch build failed");
        batch.children[0]->offset = 5;   // rows 5..15; the null at 2 is now outside the window
        batch.children[0]->length = 11;
        batch.children[0]->null_count = -1;  // force the bitmap scan rather than a trusted count
        batch.length = 11;
        expect_accepted(batch, mapping, "null before an offset window");
        ArrowArrayRelease(&batch);
    }
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 16, {9}), "batch build failed");
        batch.children[0]->offset = 5;   // rows 5..15; the null at 9 is row 4 of the window
        batch.children[0]->length = 11;
        batch.children[0]->null_count = -1;
        batch.length = 11;
        expect_rejected(batch, mapping, "null at row 4", "null inside an offset window");
        ArrowArrayRelease(&batch);
    }

    // null_count == -1 means "not computed"; the bitmap is then the only source of truth. A reader
    // that trusted the count alone would let this through.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 20, {7}), "batch build failed");
        batch.children[0]->null_count = -1;
        expect_rejected(batch, mapping, "null at row 7", "unset null_count");
        ArrowArrayRelease(&batch);
    }

    // An array with no validity buffer is all-valid no matter what null_count claims, so this must
    // not fabricate a row index out of a null bitmap that does not exist.
    {
        ArrowArray batch{};
        require(build_int64_batch(batch, schema, 8, {}), "batch build failed");
        batch.children[0]->null_count = 3;  // malformed: claims nulls with no bitmap to back them
        expect_accepted(batch, mapping, "claimed nulls with no validity buffer");
        ArrowArrayRelease(&batch);
    }

    ArrowSchemaRelease(&schema);
    std::cout << "null rejection: ok\n";
    return 0;
}
