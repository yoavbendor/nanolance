// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Round-trip test for structural dictionary encoding of scattered low-cardinality strings
// (e.g. row-{i%500}). Verifies nanolance reads its own dict output back exactly and the data file
// is much smaller than plain variable-width encoding.
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_dict_" + std::string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

std::uintmax_t data_dir_bytes(const std::filesystem::path& ds) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(ds / "data", ec)) {
        if (e.is_regular_file() && e.path().extension() == ".lance") {
            total += std::filesystem::file_size(e.path(), ec);
        }
    }
    return total;
}

void write_strings(const std::filesystem::path& ds, bool compress, const std::vector<std::string>& vals) {
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetFormat(&schema, "u") == NANOARROW_OK, "format");
    require(ArrowSchemaSetName(&schema, "u") == NANOARROW_OK, "name");
    ArrowArray source{};
    require(ArrowArrayInitFromSchema(&source, &schema, nullptr) == NANOARROW_OK, "array init");
    require(ArrowArrayStartAppending(&source) == NANOARROW_OK, "append start");
    for (const auto& v : vals) {
        require(ArrowArrayAppendString(&source, {v.data(), static_cast<int64_t>(v.size())}) == NANOARROW_OK, "append");
    }
    require(ArrowArrayFinishBuildingDefault(&source, nullptr) == NANOARROW_OK, "finish");
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_ignore_nullability(&writer, true) == NANO_LANCE_OK, "ignore null");
    require(nano_lance_writer_set_compression(&writer, compress) == NANO_LANCE_OK, "compress");
    // Structural encodings (incl. dictionary) are on by default; the plain baseline must disable them.
    require(nano_lance_writer_set_structural_encoding(&writer, compress) == NANO_LANCE_OK, "set structural");
    require(nano_lance_write_batch(&writer, &source, &schema) == NANO_LANCE_OK, "write");
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    ArrowArrayRelease(&source);
    ArrowSchemaRelease(&schema);
}

std::vector<std::string> read_strings(const std::filesystem::path& ds) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), error.c_str());
    const ArrowArray* col = (batches[0].n_children > 0) ? batches[0].children[0] : &batches[0];
    const auto* off = static_cast<const std::int32_t*>(col->buffers[1]);
    const auto* data = static_cast<const char*>(col->buffers[2]);
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < col->length; ++i) {
        out.emplace_back(data + off[i], static_cast<std::size_t>(off[i + 1] - off[i]));
    }
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&batches[0]);
    return out;
}

}  // namespace

int main() {
    // 50000 rows cycling through 500 distinct strings (scattered, not run-length).
    std::vector<std::string> vals;
    vals.reserve(50000);
    for (int i = 0; i < 50000; ++i) {
        vals.push_back("row-" + std::to_string(i % 500));
    }

    const auto ds_plain = temp_dataset("plain");
    const auto ds_dict = temp_dataset("dict");
    write_strings(ds_plain, /*compress=*/false, vals);
    write_strings(ds_dict, /*compress=*/true, vals);

    require(read_strings(ds_plain) == vals, "plain cycling string column must round-trip");
    require(read_strings(ds_dict) == vals, "dict string column must round-trip");

    const auto plain_bytes = data_dir_bytes(ds_plain);
    const auto dict_bytes = data_dir_bytes(ds_dict);
    std::cerr << "dict: plain=" << plain_bytes << "B dict=" << dict_bytes << "B\n";
    require(dict_bytes * 5U < plain_bytes, "dict must drastically shrink a cycling string column");

    std::error_code ec;
    std::filesystem::remove_all(ds_plain, ec);
    std::filesystem::remove_all(ds_dict, ec);
    return 0;
}
