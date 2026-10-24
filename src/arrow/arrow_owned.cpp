// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "arrow/arrow_owned.hpp"

namespace nano_lance::arrow_owned {

bool uint_array(ArrowType type, const void* values, std::int64_t n, std::size_t width, ArrowArray& out,
                std::string& error) {
    if (ArrowArrayInitFromType(&out, type) != NANOARROW_OK ||
        ArrowBufferAppend(ArrowArrayBuffer(&out, 1), values, n * static_cast<std::int64_t>(width)) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    out.length = n;
    out.null_count = 0;
    return ArrowArrayFinishBuildingDefault(&out, nullptr) == NANOARROW_OK || (error = "bad array", false);
}

/// A struct schema of `children` (name, type to copy, nullable).
bool struct_schema(const std::vector<std::tuple<const char*, const ArrowSchema*, bool>>& children, ArrowSchema& out,
                   std::string& error) {
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetTypeStruct(&out, static_cast<std::int64_t>(children.size())) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        ArrowSchema* child = out.children[i];
        child->release(child);
        if (ArrowSchemaDeepCopy(std::get<1>(children[i]), child) != NANOARROW_OK ||
            ArrowSchemaSetName(child, std::get<0>(children[i])) != NANOARROW_OK ||
            ArrowSchemaSetMetadata(child, nullptr) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
        child->flags = std::get<2>(children[i]) ? ARROW_FLAG_NULLABLE : 0;
    }
    return true;
}

bool type_schema(ArrowType type, ArrowSchema& out) {
    ArrowSchemaInit(&out);
    return ArrowSchemaSetType(&out, type) == NANOARROW_OK;
}

bool struct_batch(std::vector<ArrowArray*> children, std::int64_t length, ArrowArray& out, std::string& error) {
    if (ArrowArrayInitFromType(&out, NANOARROW_TYPE_STRUCT) != NANOARROW_OK ||
        ArrowArrayAllocateChildren(&out, static_cast<std::int64_t>(children.size())) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        ArrowArrayMove(children[i], out.children[i]);
    }
    out.length = length;
    out.null_count = 0;
    return true;
}

}  // namespace nano_lance::arrow_owned
