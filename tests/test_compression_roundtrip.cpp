// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Round-trip test for opt-in zstd compression of variable-width (utf8) columns.
// Writes a repetitive string column with compression enabled and verifies nanolance reads it back
// exactly, and that the compressed data file is smaller than the uncompressed one.
// (Lance interop is covered by the arrowipc2lance --compress smoke / manual checks.)
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
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_compress_" + std::string(suffix));
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
    require(nano_lance_writer_set_compression(&writer, compress) == NANO_LANCE_OK, "set compression");
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
    require(batches.size() == 1U, "one batch");
    // Root struct -> single utf8 child.
    const ArrowArray* col = (batches[0].n_children > 0) ? batches[0].children[0] : &batches[0];
    const auto* offsets = static_cast<const std::int32_t*>(col->buffers[1]);
    const auto* data = static_cast<const char*>(col->buffers[2]);
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < col->length; ++i) {
        out.emplace_back(data + offsets[i], static_cast<std::size_t>(offsets[i + 1] - offsets[i]));
    }
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&batches[0]);
    return out;
}

}  // namespace

int main() {
    std::vector<std::string> vals;
    for (int i = 0; i < 3000; ++i) {
        vals.push_back("s3://my-multimodal-bucket/train/images/capture_" + std::to_string(i % 11) + ".pcapng");
    }

    const auto ds_plain = temp_dataset("plain");
    const auto ds_zstd = temp_dataset("zstd");
    write_strings(ds_plain, /*compress=*/false, vals);
    write_strings(ds_zstd, /*compress=*/true, vals);

    require(read_strings(ds_plain) == vals, "plain strings must round-trip (multi-page)");
    require(read_strings(ds_zstd) == vals, "zstd strings must round-trip (multi-page)");

    const auto plain_bytes = data_dir_bytes(ds_plain);
    const auto zstd_bytes = data_dir_bytes(ds_zstd);
    std::cerr << "compression: plain=" << plain_bytes << "B zstd=" << zstd_bytes << "B\n";
    require(zstd_bytes < plain_bytes, "zstd must shrink a repetitive string column");

    std::error_code ec;
    std::filesystem::remove_all(ds_plain, ec);
    std::filesystem::remove_all(ds_zstd, ec);
    return 0;
}
