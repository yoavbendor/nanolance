// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer arrow: the boundary between Arrow (the C data interface, through nanoarrow) and Lance's schema. An Arrow schema
// maps to a LanceSchemaMapping: one entry per field, with its Lance logical type and the physical column it is stored
// in. This example builds a two-column Arrow batch with nanoarrow, maps its schema and turns the batch into the column
// values the file writers take.
//
//   - map_arrow_schema: Arrow schema -> LanceSchemaMapping. A type Lance cannot store is refused by name.
//   - lance_arrow_format_for_logical_type and lance_on_disk_logical_type: the names Lance uses for a type.
//   - append_batch_column_values: a batch's physical columns -> ColumnValues.
//
// Run it with: ctest --test-dir build -R nano_lance_example_arrow --output-on-failure

#include "nanolance/array_accessor.hpp"
#include "nanolance/column_values.hpp"
#include "nanolance/schema_mapper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const std::string& message) { std::cerr << "FAIL: " << message << "\n"; return 1; }

// The schema and batch built below. nanoarrow allocates them, so they are released when the example ends.
struct Owned {
    ArrowSchema schema{};
    ArrowArray array{};
    Owned() = default;
    ~Owned() {
        if (array.release != nullptr) { array.release(&array); }
        if (schema.release != nullptr) { schema.release(&schema); }
    }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
};

}  // namespace

int main() {
    std::string error;
    Owned owned;

    // Step 1: the Arrow schema, struct<id: int64, name: utf8>, the shape of a record batch.
    ArrowSchemaInit(&owned.schema);
    if (ArrowSchemaSetTypeStruct(&owned.schema, 2) != NANOARROW_OK) {
        return fail("ArrowSchemaSetTypeStruct");
    }
    ArrowSchemaSetType(owned.schema.children[0], NANOARROW_TYPE_INT64);
    ArrowSchemaSetName(owned.schema.children[0], "id");
    ArrowSchemaSetType(owned.schema.children[1], NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(owned.schema.children[1], "name");

    // Step 2: three rows, built with nanoarrow's builders.
    const std::int64_t ids[] = {7, 8, 9};
    const char* names[] = {"ada", "grace", "linus"};
    if (ArrowArrayInitFromSchema(&owned.array, &owned.schema, nullptr) != NANOARROW_OK ||
        ArrowArrayStartAppending(&owned.array) != NANOARROW_OK) {
        return fail("nanoarrow could not start the batch");
    }
    for (int i = 0; i < 3; ++i) {
        ArrowArrayAppendInt(owned.array.children[0], ids[i]);
        ArrowArrayAppendString(owned.array.children[1], ArrowCharView(names[i]));
        ArrowArrayFinishElement(&owned.array);
    }
    if (ArrowArrayFinishBuildingDefault(&owned.array, nullptr) != NANOARROW_OK) {
        return fail("ArrowArrayFinishBuildingDefault");
    }

    // Step 3: map the schema. Each field gets a Lance logical type and, if it is stored, a column index.
    nano_lance::LanceSchemaMapping mapping;
    if (!nano_lance::map_arrow_schema(owned.schema, mapping, error) || mapping.fields.size() != 2) {
        return fail("map_arrow_schema: " + error);
    }
    const auto& id = mapping.fields[0];
    const auto& name = mapping.fields[1];
    std::cout << "id: " << id.logical_type << " in column " << id.column_index << "; name: " << name.logical_type
              << ", written as " << nano_lance::lance_on_disk_logical_type(name.logical_type) << " in column "
              << name.column_index << "\n";
    if (id.name != "id" || id.column_index != 0 || name.name != "name" || name.column_index != 1) {
        return fail("field names or column indices are not in schema order");
    }

    // Step 4: the Arrow format string of a Lance type ("l" is int64 in the Arrow C format).
    std::string format;
    if (!nano_lance::lance_arrow_format_for_logical_type(id.logical_type, format, error) || format != "l") {
        return fail("int64 should have the Arrow format \"l\": " + error);
    }

    // Step 5: the batch's physical columns as ColumnValues, the form the file writers take. The fixed-width column
    // holds the ids as little-endian int64 bytes.
    std::vector<nano_lance::ColumnValues> columns(nano_lance::lance_physical_fields(mapping).size());
    if (!nano_lance::append_batch_column_values(owned.array, mapping, columns, error)) {
        return fail("append_batch_column_values: " + error);
    }
    std::int64_t got[3] = {};
    if (columns[0].fixed.size() != sizeof(got)) {
        return fail("the id column holds " + std::to_string(columns[0].fixed.size()) + " bytes, want 24");
    }
    std::memcpy(got, columns[0].fixed.data(), sizeof(got));
    std::cout << "id column: " << got[0] << ", " << got[1] << ", " << got[2] << "\n";
    if (got[0] != ids[0] || got[1] != ids[1] || got[2] != ids[2]) {
        return fail("the id column does not hold the ids of the batch");
    }
    return 0;
}
