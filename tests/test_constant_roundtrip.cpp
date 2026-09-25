// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Round-trip test for ConstantLayout: a constant fixed-width column under --compress is stored as a
// single inline value (zero data buffers) and read back identically.
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

// NB: `require(f(error), error)` is a trap. The two arguments are evaluated in unspecified
// order, so c_str() can capture a pointer into the EMPTY string before f() runs; when f() then fails
// and assigns a long message, the string reallocates and that pointer dangles. Real failures printed
// a stray letter instead of their message. Pass the string itself.
void require(bool ok, const std::string& msg) {
    require(ok, msg.c_str());
}

std::filesystem::path temp_dataset(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_const_" + std::string(suffix));
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
    // Structural encodings (incl. ConstantLayout) are on by default; the plain baseline must disable them.
    require(nano_lance_writer_set_structural_encoding(&writer, compress) == NANO_LANCE_OK, "set structural");
    require(nano_lance_write_batch(&writer, &batch, &field) == NANO_LANCE_OK, "write");
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
}

std::vector<std::int64_t> read_int64(const std::filesystem::path& ds) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), error);
    require(batches.size() == 1U, "one batch");
    const ArrowArray* col = (batches[0].n_children > 0) ? batches[0].children[0] : &batches[0];
    const auto* v = static_cast<const std::int64_t*>(col->buffers[1]);
    std::vector<std::int64_t> out(v, v + col->length);
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&batches[0]);
    return out;
}

}  // namespace

void write_string_const(const std::filesystem::path& ds, const std::string& value, int rows) {
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetFormat(&schema, "u") == NANOARROW_OK, "format");
    require(ArrowSchemaSetName(&schema, "u") == NANOARROW_OK, "name");
    ArrowArray source{};
    require(ArrowArrayInitFromSchema(&source, &schema, nullptr) == NANOARROW_OK, "array init");
    require(ArrowArrayStartAppending(&source) == NANOARROW_OK, "append start");
    for (int i = 0; i < rows; ++i) {
        require(ArrowArrayAppendString(&source, {value.data(), static_cast<int64_t>(value.size())}) == NANOARROW_OK,
                "append");
    }
    require(ArrowArrayFinishBuildingDefault(&source, nullptr) == NANOARROW_OK, "finish");
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_ignore_nullability(&writer, true) == NANO_LANCE_OK, "ignore null");
    require(nano_lance_writer_set_compression(&writer, true) == NANO_LANCE_OK, "compress");
    require(nano_lance_write_batch(&writer, &source, &schema) == NANO_LANCE_OK, "write");
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    ArrowArrayRelease(&source);
    ArrowSchemaRelease(&schema);
}

std::vector<std::string> read_string(const std::filesystem::path& ds) {
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

int main() {
    const std::vector<std::int64_t> vals(20000, 1500);  // all identical -> ConstantLayout

    const auto ds_plain = temp_dataset("plain");
    const auto ds_const = temp_dataset("const");
    write_int64(ds_plain, /*compress=*/false, vals);
    write_int64(ds_const, /*compress=*/true, vals);

    require(read_int64(ds_plain) == vals, "plain constant column must round-trip");
    require(read_int64(ds_const) == vals, "ConstantLayout column must round-trip");

    const auto plain_bytes = data_dir_bytes(ds_plain);
    const auto const_bytes = data_dir_bytes(ds_const);
    std::cerr << "constant int: plain=" << plain_bytes << "B constant=" << const_bytes << "B\n";
    // A constant column should be tiny regardless of row count (value stored once in the descriptor).
    require(const_bytes < 2048U, "ConstantLayout column must be near-zero on disk");

    // Constant string column -> ConstantLayout with a single-value scalar buffer.
    const auto ds_str = temp_dataset("str");
    const std::string uri = "s3://my-multimodal-bucket/captures/run_001.pcapng";
    write_string_const(ds_str, uri, 20000);
    const auto got = read_string(ds_str);
    require(got.size() == 20000U, "constant string row count");
    require(got.front() == uri && got.back() == uri, "constant string must round-trip");
    require(data_dir_bytes(ds_str) < 2048U, "constant string column must be near-zero on disk");

    std::error_code ec;
    std::filesystem::remove_all(ds_plain, ec);
    std::filesystem::remove_all(ds_const, ec);
    std::filesystem::remove_all(ds_str, ec);
    return 0;
}
