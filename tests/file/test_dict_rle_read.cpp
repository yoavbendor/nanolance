// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// A dictionary whose indices are run-length encoded, as Lance's writer makes them (the mini-block value encoding
// Rle{values, lengths} under a dictionary): the run values are dictionary indices, and each run's length has the
// width the page declares. tests/golden/dict_rle holds two files Lance's encoder test (test_miniblock_bitpack) wrote,
// kept by NANOLANCE_RUST_KEEP_PASS: 7178 rows of int16/int32/int64 that are the same values in each, runs of 1024, and
// their run lengths are u8 in one file and u16 in the other (a run longer than 255 needs the wide length). Row i is
//   3 for i < 1024, 8 below 2048, 16 below 3072, 100 below 4096, 512 below 5120, 1000 below 6144, 2000 below 7168,
//   and -1 for the last ten rows.
// Each file is read whole, by a range that crosses a run boundary, and by a take. Rust is not needed.

#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef NANO_LANCE_DICT_RLE_GOLDEN_DIR
#error "NANO_LANCE_DICT_RLE_GOLDEN_DIR must name tests/golden/dict_rle"
#endif

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

constexpr std::uint64_t kRows = 7178;

std::int64_t expected(std::uint64_t row) {
    if (row < 1024) {
        return 3;
    }
    if (row < 2048) {
        return 8;
    }
    if (row < 3072) {
        return 16;
    }
    if (row < 4096) {
        return 100;
    }
    if (row < 5120) {
        return 512;
    }
    if (row < 6144) {
        return 1000;
    }
    if (row < 7168) {
        return 2000;
    }
    return -1;
}

/// The values of one read, as int64, from the column's Arrow format (int16, int32 or int64; the file holds one of
/// them). Nulls are refused: the golden column has none.
void append_values(const ArrowSchema& schema, const std::vector<ArrowArray>& batches, std::vector<std::int64_t>& out) {
    require(schema.n_children == 1, "one column");
    const std::string format = schema.children[0]->format;
    for (const auto& batch : batches) {
        const ArrowArray& column = *batch.children[0];
        require(column.null_count == 0, "no nulls in the golden column");
        for (std::int64_t i = 0; i < batch.length; ++i) {
            const std::int64_t at = column.offset + i;
            if (format == "s") {
                std::int16_t v = 0;
                std::memcpy(&v, static_cast<const std::uint8_t*>(column.buffers[1]) + at * 2, 2);
                out.push_back(v);
            } else if (format == "i") {
                std::int32_t v = 0;
                std::memcpy(&v, static_cast<const std::uint8_t*>(column.buffers[1]) + at * 4, 4);
                out.push_back(v);
            } else {
                require(format == "l", "the golden column is int16, int32 or int64, not '" + format + "'");
                std::int64_t v = 0;
                std::memcpy(&v, static_cast<const std::uint8_t*>(column.buffers[1]) + at * 8, 8);
                out.push_back(v);
            }
        }
    }
}

void release(ArrowSchema& schema, std::vector<ArrowArray>& batches) {
    for (auto& batch : batches) {
        if (batch.release != nullptr) {
            ArrowArrayRelease(&batch);
        }
    }
    if (schema.release != nullptr) {
        ArrowSchemaRelease(&schema);
    }
}

void check_file(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    std::string error;

    {
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_read(path, request, schema, batches, error), name + " whole: " + error);
        std::vector<std::int64_t> got;
        append_values(schema, batches, got);
        require(got.size() == kRows, name + " whole: " + std::to_string(got.size()) + " rows");
        for (std::uint64_t r = 0; r < kRows; ++r) {
            require(got[r] == expected(r), name + " whole: row " + std::to_string(r) + " is " + std::to_string(got[r]));
        }
        release(schema, batches);
    }

    {
        // A range across the boundary of the 1024-row runs, and past them into the last run.
        constexpr std::uint64_t kFirst = 1020;
        constexpr std::uint64_t kCount = 2060;
        nano_lance::LanceScanRequest request;
        request.range = {kFirst, kCount};
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_read(path, request, schema, batches, error), name + " range: " + error);
        std::vector<std::int64_t> got;
        append_values(schema, batches, got);
        require(got.size() == kCount, name + " range: " + std::to_string(got.size()) + " rows");
        for (std::uint64_t k = 0; k < kCount; ++k) {
            require(got[k] == expected(kFirst + k), name + " range: row " + std::to_string(kFirst + k) + " is wrong");
        }
        release(schema, batches);
    }

    {
        const std::vector<std::uint64_t> wanted = {0, 1023, 1024, 4096, 6143, 7167, 7177};
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_take(path, request, wanted, schema, batches, error), name + " take: " + error);
        std::vector<std::int64_t> got;
        append_values(schema, batches, got);
        require(got.size() == wanted.size(), name + " take: " + std::to_string(got.size()) + " rows");
        for (std::size_t k = 0; k < wanted.size(); ++k) {
            require(got[k] == expected(wanted[k]), name + " take: row " + std::to_string(wanted[k]) + " is wrong");
        }
        release(schema, batches);
    }
}

}  // namespace

int main() {
    const std::filesystem::path dir = NANO_LANCE_DICT_RLE_GOLDEN_DIR;
    check_file(dir / "u8_lengths.lance");
    check_file(dir / "u16_lengths.lance");
    std::cout << "dictionary run-length indices read back: 2 files, whole, by range and by take\n";
    return 0;
}
