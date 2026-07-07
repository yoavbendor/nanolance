// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Round-trip test for run-length encoding of a low-cardinality fixed-width integer column.
// Writes a column with long runs (incl. runs > 255 to exercise 8-bit sub-run splitting) under
// --compress, verifies nanolance reads it back exactly and the data file is tiny.
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
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
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_rle_" + std::string(suffix));
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

void write_int64(const std::filesystem::path& ds, bool compress, const std::vector<std::int64_t>& vals) {
    ArrowArray batch{};
    batch.length = static_cast<int64_t>(vals.size());
    batch.n_buffers = 2;
    const void* buffers[] = {nullptr, vals.data()};
    batch.buffers = buffers;
    ArrowSchema field{};
    field.format = "l";
    field.name = "v";
    field.flags = 0;
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_compression(&writer, compress) == NANO_LANCE_OK, "set compression");
    // Structural encodings (incl. RLE) are on by default; the plain baseline must disable them.
    require(nano_lance_writer_set_structural_encoding(&writer, compress) == NANO_LANCE_OK, "set structural");
    require(nano_lance_write_batch(&writer, &batch, &field) == NANO_LANCE_OK, "write");
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
}

std::vector<std::int64_t> read_int64(const std::filesystem::path& ds) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), error.c_str());
    const ArrowArray* col = (batches[0].n_children > 0) ? batches[0].children[0] : &batches[0];
    const auto* v = static_cast<const std::int64_t*>(col->buffers[1]);
    std::vector<std::int64_t> out(v, v + col->length);
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&batches[0]);
    return out;
}

}  // namespace

int main() {
    // 50000 rows, 10 distinct values in consecutive runs of 5000 (each run splits into ~20 sub-runs).
    std::vector<std::int64_t> vals;
    vals.reserve(50000);
    for (int i = 0; i < 50000; ++i) {
        vals.push_back((i * 10) / 50000);
    }

    const auto ds_plain = temp_dataset("plain");
    const auto ds_rle = temp_dataset("rle");
    write_int64(ds_plain, /*compress=*/false, vals);
    write_int64(ds_rle, /*compress=*/true, vals);

    require(read_int64(ds_plain) == vals, "plain run-length column must round-trip");
    require(read_int64(ds_rle) == vals, "RLE column must round-trip");

    const auto plain_bytes = data_dir_bytes(ds_plain);
    const auto rle_bytes = data_dir_bytes(ds_rle);
    std::cerr << "rle: plain=" << plain_bytes << "B rle=" << rle_bytes << "B\n";
    require(rle_bytes * 20U < plain_bytes, "RLE must drastically shrink a run-length column");

    std::error_code ec;
    std::filesystem::remove_all(ds_plain, ec);
    std::filesystem::remove_all(ds_rle, ec);
    return 0;
}
