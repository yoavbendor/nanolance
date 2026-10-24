// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Shared by the index layer's tests: a require() that fails the test, a scratch directory, and a small
// dataset written with the C writer API (one fragment per call), as the other index tests write theirs.

#pragma once

#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace index_test {

inline void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

/// A fresh, empty directory below the system's temporary directory.
inline std::filesystem::path scratch(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

/// A column of the test dataset: int64, utf8, list<utf8> or a fixed-size list of `dim` float32.
struct Column {
    enum class Kind { Int64, String, StringList, Vector };
    std::string name;
    Kind kind = Kind::Int64;
    int dim = 0;
};

/// One row's value of column `c`, appended to `array` (that column's child array).
using Fill = std::function<void(ArrowArray* array, std::size_t c, std::int64_t row)>;

inline void build_schema(const std::vector<Column>& columns, ArrowSchema& schema) {
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, static_cast<std::int64_t>(columns.size())) == NANOARROW_OK, "struct");
    for (std::size_t c = 0; c < columns.size(); ++c) {
        ArrowSchema* child = schema.children[c];
        switch (columns[c].kind) {
            case Column::Kind::Int64:
                require(ArrowSchemaSetType(child, NANOARROW_TYPE_INT64) == NANOARROW_OK, "int64");
                break;
            case Column::Kind::String:
                require(ArrowSchemaSetType(child, NANOARROW_TYPE_STRING) == NANOARROW_OK, "string");
                break;
            case Column::Kind::StringList:
                require(ArrowSchemaSetType(child, NANOARROW_TYPE_LIST) == NANOARROW_OK, "list");
                require(ArrowSchemaSetType(child->children[0], NANOARROW_TYPE_STRING) == NANOARROW_OK, "item");
                break;
            case Column::Kind::Vector:
                require(ArrowSchemaSetTypeFixedSize(child, NANOARROW_TYPE_FIXED_SIZE_LIST, columns[c].dim) ==
                            NANOARROW_OK,
                        "fixed-size list");
                require(ArrowSchemaSetType(child->children[0], NANOARROW_TYPE_FLOAT) == NANOARROW_OK, "float");
                break;
        }
        require(ArrowSchemaSetName(child, columns[c].name.c_str()) == NANOARROW_OK, "name");
    }
}

/// Rows [first, first + count) as one fragment: a new dataset when `append` is false.
inline void write_fragment(const std::filesystem::path& path, bool append, const std::vector<Column>& columns,
                           std::int64_t first, std::int64_t count, const Fill& fill) {
    ArrowSchema schema{};
    build_schema(columns, schema);
    ArrowArray array{};
    ArrowError err{};
    require(ArrowArrayInitFromSchema(&array, &schema, &err) == NANOARROW_OK, std::string("array: ") + err.message);
    require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start appending");
    for (std::int64_t r = first; r < first + count; ++r) {
        for (std::size_t c = 0; c < columns.size(); ++c) {
            fill(array.children[c], c, r);
        }
    }
    array.length = count;
    require(ArrowArrayFinishBuildingDefault(&array, &err) == NANOARROW_OK, std::string("finish: ") + err.message);

    NanoLanceWriter w{};
    const auto opened = append ? nano_lance_writer_init_append(&w, path.string().c_str(), 0)
                               : nano_lance_writer_init(&w, path.string().c_str(), 0);
    require(opened == NANO_LANCE_OK, "open the writer");
    require(nano_lance_write_batch(&w, &array, &schema) == NANO_LANCE_OK,
            std::string("write_batch: ") + nano_lance_writer_last_error(&w));
    require(nano_lance_writer_commit(&w, append) == NANO_LANCE_OK, "commit the fragment");
    require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close the writer");
    if (array.release != nullptr) {
        ArrowArrayRelease(&array);
    }
    if (schema.release != nullptr) {
        ArrowSchemaRelease(&schema);
    }
}

inline void append_string(ArrowArray* a, const std::string& s) {
    ArrowStringView v{s.data(), static_cast<std::int64_t>(s.size())};
    require(ArrowArrayAppendString(a, v) == NANOARROW_OK, "append a string");
}

inline void append_vector(ArrowArray* a, const std::vector<float>& v) {
    for (const float x : v) {
        require(ArrowArrayAppendDouble(a->children[0], x) == NANOARROW_OK, "append a float");
    }
    require(ArrowArrayFinishElement(a) == NANOARROW_OK, "finish a vector");
}

}  // namespace index_test
