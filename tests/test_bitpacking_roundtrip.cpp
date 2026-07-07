// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Round-trip test for opt-in FastLanes bitpacking of fixed-width integer columns.
// Writes a >1024-row int64 column (multi-page) with compression enabled, verifies nanolance reads it
// back exactly, and that the bitpacked data file is smaller. (Lance interop is checked via the
// arrowipc2lance --compress manual/smoke path.)
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
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_bitpack_" + std::string(suffix));
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
    field.flags = 0;  // non-nullable

    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_compression(&writer, compress) == NANO_LANCE_OK, "set compression");
    // Structural encodings (incl. bitpacking) are on by default; the plain baseline must disable them.
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
    require(batches.size() == 1U, "one batch");
    const ArrowArray* col = (batches[0].n_children > 0) ? batches[0].children[0] : &batches[0];
    const auto* v = static_cast<const std::int64_t*>(col->buffers[1]);
    std::vector<std::int64_t> out(v, v + col->length);
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&batches[0]);
    return out;
}

}  // namespace

int main() {
    std::vector<std::int64_t> vals;
    vals.reserve(5000);
    for (int i = 0; i < 5000; ++i) {
        vals.push_back((i * 7) % 1000);  // ~10-bit values, 5000 rows -> multiple 1024 chunks
    }

    const auto ds_plain = temp_dataset("plain");
    const auto ds_bp = temp_dataset("bp");
    write_int64(ds_plain, /*compress=*/false, vals);
    write_int64(ds_bp, /*compress=*/true, vals);

    require(read_int64(ds_plain) == vals, "plain ints must round-trip (multi-page)");
    require(read_int64(ds_bp) == vals, "bitpacked ints must round-trip (multi-page)");

    const auto plain_bytes = data_dir_bytes(ds_plain);
    const auto bp_bytes = data_dir_bytes(ds_bp);
    std::cerr << "bitpacking: plain=" << plain_bytes << "B bitpacked=" << bp_bytes << "B\n";
    require(bp_bytes < plain_bytes, "bitpacking must shrink a small-valued int column");

    std::error_code ec;
    std::filesystem::remove_all(ds_plain, ec);
    std::filesystem::remove_all(ds_bp, ec);
    return 0;
}
