#include "nano_lance_writer/array_accessor.hpp"
#include "nano_lance_writer/schema_mapper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
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
    require(nano_lance::map_arrow_schema(schema, mapping, error, true), error.c_str());
    const auto physical = nano_lance::lance_physical_fields(mapping);
    require(physical.size() == 2, "expected two physical columns");

    std::vector<nano_lance::ColumnValues> columns;

    ArrowArray batch1{};
    require(build_batch(batch1, schema, {{1, "aa"}, {2, "b"}}), "batch1 build failed");
    require(nano_lance::append_batch_column_values(batch1, mapping, columns, error), error.c_str());
    ArrowArrayRelease(&batch1);

    require(columns[0].kind == nano_lance::ColumnValues::Kind::FixedWidth, "id column not fixed");
    require(columns[0].fixed.size() == 16, "id fixed width size mismatch");
    require(columns[1].kind == nano_lance::ColumnValues::Kind::VariableWidth, "tag column not variable");
    require(columns[1].variable.data.size() == 3, "tag data after batch1");

    ArrowArray batch2{};
    require(build_batch(batch2, schema, {{3, "xyz"}}), "batch2 build failed");
    require(nano_lance::append_batch_column_values(batch2, mapping, columns, error), error.c_str());
    ArrowArrayRelease(&batch2);

    require(columns[0].fixed.size() == 24, "id fixed width after batch2");
    require(columns[1].variable.data.size() == 6, "tag data after batch2");

    ArrowSchemaRelease(&schema);
    return 0;
}
