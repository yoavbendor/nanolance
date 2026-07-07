// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Regression test for boolean columns. Arrow stores booleans bit-packed (1 bit/value) but nanolance
// stores one byte per value on disk; a large column previously over-read the bit-packed input buffer
// and crashed, and the bulk read path copied the byte-per-value data straight into Arrow's bit-packed
// buffer (decoding to garbage). This verifies a large boolean column round-trips exactly.
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
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), error.c_str());
    require(batches.size() == 1U, "one batch");
    const ArrowArray* col = (batches[0].n_children > 0) ? batches[0].children[0] : &batches[0];
    const auto* bits = static_cast<const std::uint8_t*>(col->buffers[1]);
    std::vector<bool> out;
    out.reserve(static_cast<std::size_t>(col->length));
    for (std::int64_t i = 0; i < col->length; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        out.push_back(((bits[idx >> 3U] >> (idx & 7U)) & 1U) != 0U);
    }
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&batches[0]);
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
        std::error_code ec;
        std::filesystem::remove_all(ds, ec);
    }
    std::cerr << "bool round-trip: 200000 values OK (plain + compressed)\n";
    return 0;
}
