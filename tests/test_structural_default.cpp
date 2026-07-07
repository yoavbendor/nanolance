// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Regression test for structural encodings being ON BY DEFAULT (independent of zstd/--compress).
// A default writer (no set_compression, no set_structural) must apply the lossless structural
// re-encodings — here RLE on a run-length int column — so the data file is far smaller than the
// explicitly-plain (--no-structural) baseline, while still round-tripping exactly. It also verifies
// that toggling set_structural_encoding(false) reproduces the plain layout.
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
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_structdef_" + std::string(suffix));
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

// structural: -1 = leave default (do not call the setter at all), 0 = off, 1 = on.
void write_int64(const std::filesystem::path& ds, int structural, const std::vector<std::int64_t>& vals) {
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
    // Deliberately never call set_compression: structural encodings must apply without zstd.
    if (structural >= 0) {
        require(nano_lance_writer_set_structural_encoding(&writer, structural != 0) == NANO_LANCE_OK,
                "set structural");
    }
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
    // Long runs of a handful of values -> RLE-friendly.
    std::vector<std::int64_t> vals;
    vals.reserve(40000);
    for (int i = 0; i < 40000; ++i) {
        vals.push_back((i / 5000) % 4);
    }

    const auto ds_default = temp_dataset("default");
    const auto ds_off = temp_dataset("off");
    const auto ds_on = temp_dataset("on");
    write_int64(ds_default, /*structural=*/-1, vals);  // default writer: no setters at all
    write_int64(ds_off, /*structural=*/0, vals);
    write_int64(ds_on, /*structural=*/1, vals);

    require(read_int64(ds_default) == vals, "default write must round-trip");
    require(read_int64(ds_off) == vals, "plain write must round-trip");
    require(read_int64(ds_on) == vals, "structural write must round-trip");

    const auto default_bytes = data_dir_bytes(ds_default);
    const auto off_bytes = data_dir_bytes(ds_off);
    const auto on_bytes = data_dir_bytes(ds_on);
    std::cerr << "structural-default: default=" << default_bytes << "B off=" << off_bytes
              << "B on=" << on_bytes << "B\n";

    // The default writer must apply structural encoding (much smaller than the explicit plain layout)...
    require(default_bytes < off_bytes / 2, "structural encodings must be ON by default (no --compress)");
    // ...and be equivalent to explicitly enabling it.
    require(default_bytes == on_bytes, "default must match explicit structural-on");

    std::error_code ec;
    std::filesystem::remove_all(ds_default, ec);
    std::filesystem::remove_all(ds_off, ec);
    std::filesystem::remove_all(ds_on, ec);
    return 0;
}
