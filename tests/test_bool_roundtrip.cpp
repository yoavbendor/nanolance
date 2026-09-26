// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Regression test for boolean columns. nanolance stores bool bit-packed on disk (1 bit/value,
// LSB-first), matching stock Lance's own Flat{bits_per_value:1} representation; its internal
// ColumnValues::fixed representation stays one byte per value. A large column previously over-read the
// bit-packed input buffer and crashed, and the bulk read path copied byte-per-value data straight into
// Arrow's bit-packed buffer (decoding to garbage). This verifies a large boolean column round-trips
// exactly and that the on-disk file is close to the 1-bit-per-value size (not 1 byte/value).
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
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_bool_" + std::string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

void write_bools(const std::filesystem::path& ds, bool compress, const std::vector<bool>& vals) {
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetFormat(&schema, "b") == NANOARROW_OK, "format");
    require(ArrowSchemaSetName(&schema, "flag") == NANOARROW_OK, "name");

    ArrowArray source{};
    require(ArrowArrayInitFromSchema(&source, &schema, nullptr) == NANOARROW_OK, "array init");
    require(ArrowArrayStartAppending(&source) == NANOARROW_OK, "append start");
    for (bool v : vals) {
        require(ArrowArrayAppendInt(&source, v ? 1 : 0) == NANOARROW_OK, "append");
    }
    require(ArrowArrayFinishBuildingDefault(&source, nullptr) == NANOARROW_OK, "finish");

    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_ignore_nullability(&writer, true) == NANO_LANCE_OK, "ignore null");
    require(nano_lance_writer_set_compression(&writer, compress) == NANO_LANCE_OK, "set compression");
    require(nano_lance_write_batch(&writer, &source, &schema) == NANO_LANCE_OK, "write");
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");

    ArrowArrayRelease(&source);
    ArrowSchemaRelease(&schema);
}

std::vector<bool> read_bools(const std::filesystem::path& ds) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), error);
    std::vector<bool> out;
    for (auto& batch : batches) {  // several with a parallel read: one per row range
        const ArrowArray* col = (batch.n_children > 0) ? batch.children[0] : &batch;
        const auto* bits = static_cast<const std::uint8_t*>(col->buffers[1]);
        for (std::int64_t i = 0; i < col->length; ++i) {
            const auto idx = static_cast<std::size_t>(i + col->offset);
            out.push_back(((bits[idx >> 3U] >> (idx & 7U)) & 1U) != 0U);
        }
        ArrowArrayRelease(&batch);
    }
    ArrowSchemaRelease(&schema);
    return out;
}

}  // namespace

int main() {
    // >200k values so the write path spans many miniblock chunks (the size that used to crash), with a
    // non-trivial true/false mix so a naive bit/byte confusion would be caught.
    std::vector<bool> vals;
    vals.reserve(200000);
    for (int i = 0; i < 200000; ++i) {
        vals.push_back((i * 7 + 3) % 5 < 2);
    }

    for (bool compress : {false, true}) {
        const auto ds = temp_dataset(compress ? "zstd" : "plain");
        write_bools(ds, compress, vals);
        require(read_bools(ds) == vals, "boolean column must round-trip exactly");

        // Bit-packed (1 bit/value, ~25000 B for 200000 rows plus small chunk headers) must be far
        // smaller than the old 1 byte/value on-disk shape (~200000 B) -- catches an accidental
        // regression back to the byte-per-value flat path.
        std::uintmax_t data_bytes = 0;
        std::error_code size_ec;
        for (const auto& entry : std::filesystem::directory_iterator(ds / "data", size_ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".lance") {
                data_bytes += std::filesystem::file_size(entry.path(), size_ec);
            }
        }
        require(data_bytes < vals.size() / 4U, "bool column must be bit-packed, not byte-per-value");

        std::error_code ec;
        std::filesystem::remove_all(ds, ec);
    }
    std::cerr << "bool round-trip: 200000 values OK (plain + compressed, bit-packed on disk)\n";
    return 0;
}
