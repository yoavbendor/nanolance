// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Round-trip test for dictionary + RLE encoding of a low-cardinality run-length string column
// (the per-minute URI case). Verifies nanolance reads its own dict+RLE output back exactly and the
// data file is tiny. Lance interop is exercised separately via arrowipc2lance --compress.
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

// NB: `require(f(error), error)` is a trap. The two arguments are evaluated in unspecified
// order, so c_str() can capture a pointer into the EMPTY string before f() runs; when f() then fails
// and assigns a long message, the string reallocates and that pointer dangles. Real failures printed
// a stray letter instead of their message. Pass the string itself.
void require(bool ok, const std::string& msg) {
    require(ok, msg.c_str());
}

std::filesystem::path temp_dataset(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_dictrle_" + std::string(suffix));
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
    // Structural encodings (incl. dict+RLE) are on by default; the plain baseline must disable them.
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
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), error);
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
    // 50000 rows, 10 distinct URIs in consecutive runs of 5000 (runs split into <=255 sub-runs).
    std::vector<std::string> vals;
    vals.reserve(50000);
    const int minutes = 10;
    for (int i = 0; i < 50000; ++i) {
        const int m = (i * minutes) / 50000;
        vals.push_back("s3://my-bucket/captures/file_" + std::to_string(m) + ".pcapng");
    }

    const auto ds_plain = temp_dataset("plain");
    const auto ds_dr = temp_dataset("dr");
    write_strings(ds_plain, /*compress=*/false, vals);
    write_strings(ds_dr, /*compress=*/true, vals);

    require(read_strings(ds_plain) == vals, "plain run-length string column must round-trip");
    require(read_strings(ds_dr) == vals, "dict+RLE string column must round-trip");

    const auto plain_bytes = data_dir_bytes(ds_plain);
    const auto dr_bytes = data_dir_bytes(ds_dr);
    std::cerr << "dict-rle: plain=" << plain_bytes << "B dict-rle=" << dr_bytes << "B\n";
    require(dr_bytes * 50U < plain_bytes, "dict+RLE must drastically shrink a run-length string column");

    std::error_code ec;
    std::filesystem::remove_all(ds_plain, ec);
    std::filesystem::remove_all(ds_dr, ec);
    return 0;
}
