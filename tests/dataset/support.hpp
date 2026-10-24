// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// What the dataset layer's tests share: a small dataset of int64 ids written with the C writer (one commit,
// one fragment, per list of ids), and a scan of one column back. Header-only; each test includes it once.

#pragma once

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace dataset_test {

inline void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

/// A fresh, empty directory under the temp directory.
inline std::filesystem::path scratch(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

/// One int64 column named "id" holding `values`, as an owned nanoarrow batch and its schema.
inline void make_ids(const std::vector<std::int64_t>& values, ArrowSchema& schema, ArrowArray& array) {
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, 1) == NANOARROW_OK, "struct schema");
    require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK, "int64 child");
    require(ArrowSchemaSetName(schema.children[0], "id") == NANOARROW_OK, "child name");
    schema.children[0]->flags = 0;  // not nullable
    require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array from schema");
    require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start appending");
    for (const auto v : values) {
        require(ArrowArrayAppendInt(array.children[0], v) == NANOARROW_OK, "append");
        require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "finish element");
    }
    require(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK, "finish building");
}

inline void write_ids(NanoLanceWriter* writer, const std::vector<std::int64_t>& values) {
    ArrowSchema schema{};
    ArrowArray array{};
    make_ids(values, schema, array);
    require(nano_lance_write_batch(writer, &array, &schema) == NANO_LANCE_OK,
            std::string("write_batch: ") + nano_lance_writer_last_error(writer));
    if (array.release != nullptr) {
        ArrowArrayRelease(&array);
    }
    ArrowSchemaRelease(&schema);
}

/// A dataset with one fragment per entry of `fragments`, each its own commit (version 1, 2, ...).
inline void write_dataset(const std::filesystem::path& path, const std::vector<std::vector<std::int64_t>>& fragments) {
    for (std::size_t i = 0; i < fragments.size(); ++i) {
        NanoLanceWriter w{};
        const bool append = i != 0;
        const int rc = append ? nano_lance_writer_init_append(&w, path.string().c_str(), 0)
                              : nano_lance_writer_init(&w, path.string().c_str(), 0);
        require(rc == NANO_LANCE_OK, "writer init");
        write_ids(&w, fragments[i]);
        require(nano_lance_writer_commit(&w, append) == NANO_LANCE_OK,
                std::string("commit: ") + nano_lance_writer_last_error(&w));
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
    }
}

/// The int64 values of column `name` over a scan of the latest version (fragment order).
inline std::vector<std::int64_t> read_column(const std::filesystem::path& path, const std::string& name,
                                             const std::string* filter = nullptr) {
    nano_lance::LanceScanRequest request;
    request.filter = filter;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_dataset_scan(path, request, schema, batches, error), "scan: " + error);
    std::int64_t index = -1;
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (name == schema.children[i]->name) {
            index = i;
        }
    }
    require(index >= 0, "the scan has a column named " + name);
    std::vector<std::int64_t> out;
    for (auto& batch : batches) {
        const ArrowArray* col = batch.children[index];
        for (std::int64_t r = 0; r < col->length; ++r) {
            out.push_back(static_cast<const std::int64_t*>(col->buffers[1])[col->offset + r]);
        }
        ArrowArrayRelease(&batch);
    }
    ArrowSchemaRelease(&schema);
    return out;
}

/// The top-level column names of the latest version.
inline std::vector<std::string> column_names(const std::filesystem::path& path) {
    ArrowSchema schema{};
    std::string error;
    require(nano_lance::lance_table_read_schema(path, schema, error), "schema: " + error);
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        out.emplace_back(schema.children[i]->name);
    }
    ArrowSchemaRelease(&schema);
    return out;
}

}  // namespace dataset_test
