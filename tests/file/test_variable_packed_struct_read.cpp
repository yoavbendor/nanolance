// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Lance 2.2 packed structs with variable-width children (`lance-encoding:packed`, a VariablePackedStruct page): one
// column whose rows hold the children back to back, a variable child as a length prefix and its bytes. The golden
// files in tests/golden/variable_packed are the small files Lance's own encoder tests write (NANOLANCE_RUST_KEEP_PASS
// keeps them), and their values are the ones those tests encode:
//   utf8_struct.lance          Struct<id: u32, uri: utf8, long_text: large_utf8>, three rows, not nested.
//   sparse_list_struct.lance   List<packed Struct<value: utf8>> over 70000 rows, one item on every 2000th row.
//   list_of_list_struct.lance  List<List<packed Struct<value: utf8>>>, one outer row of 70000 inner lists.
// Each file is read whole, by a row range and by a take, and every row is rendered as text and compared with the
// text the values give. Rust is not needed.

#include "golden_render.hpp"
#include "nanolance/lance_table_reader.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef NANO_LANCE_VARIABLE_PACKED_GOLDEN_DIR
#error "NANO_LANCE_VARIABLE_PACKED_GOLDEN_DIR must name tests/golden/variable_packed"
#endif

namespace {

using golden::release;
using golden::render_rows;
using golden::require;

/// Expected row text for each row of a file.
using RowText = std::string (*)(std::uint64_t row);

std::string utf8_row(std::uint64_t row) {
    static const char* uri[] = {"\"a\"", "\"b\"", "\"/tmp/x\""};
    static const char* long_text[] = {"\"alpha\"", "\"a considerably longer payload for testing\"", "\"mid\""};
    return "{id:" + std::to_string(row + 1) + ",uri:" + uri[row] + ",long_text:" + long_text[row] + "}";
}

/// Row r of the sparse file: one item {value: "value-k"} on every 2000th row (row 1999 + 2000k) and on the last row,
/// and an empty list everywhere else.
std::string sparse_row(std::uint64_t row) {
    constexpr std::uint64_t kStride = 2000;
    constexpr std::uint64_t kRows = 70000;
    if ((row + 1) % kStride == 0 || row + 1 == kRows) {
        return "[{value:\"value-" + std::to_string((row + 1) / kStride - 1) + "\"}]";
    }
    return "[]";
}

/// Row 0 of the list of lists: 70000 inner lists, all empty but the last two, which hold {value: "value-0"} and
/// {value: "value-1"}.
std::string list_of_list_row(std::uint64_t row) {
    constexpr std::uint64_t kInner = 70000;
    std::string out = "[";
    for (std::uint64_t j = 0; j < kInner; ++j) {
        if (j > 0) {
            out += ",";
        }
        if (j == kInner - 2) {
            out += "[{value:\"value-0\"}]";
        } else if (j == kInner - 1) {
            out += "[{value:\"value-1\"}]";
        } else {
            out += "[]";
        }
    }
    (void)row;
    return out + "]";
}

/// Reads `path` whole, by the row range [first, first + count), and by the rows `wanted` (ascending), and compares each
/// read row with `expected(row)`. The points are chosen per file so that they hold rows with values, not only empties.
void check_file(const std::filesystem::path& path, std::uint64_t rows, RowText expected, std::uint64_t first,
                std::uint64_t count, const std::vector<std::uint64_t>& wanted) {
    const std::string name = path.filename().string();
    std::string error;

    {
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_read(path, request, schema, batches, error), name + " whole: " + error);
        const auto got = render_rows(schema, batches);
        require(got.size() == rows, name + " whole: " + std::to_string(got.size()) + " rows");
        for (std::uint64_t r = 0; r < rows; ++r) {
            require(got[r] == expected(r), name + " whole: row " + std::to_string(r) + " is " + got[r]);
        }
        release(schema, batches);
    }

    {
        nano_lance::LanceScanRequest request;
        request.range = {first, count};
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_read(path, request, schema, batches, error), name + " range: " + error);
        const auto got = render_rows(schema, batches);
        require(got.size() == count, name + " range: " + std::to_string(got.size()) + " rows");
        for (std::uint64_t k = 0; k < count; ++k) {
            require(got[k] == expected(first + k), name + " range: row " + std::to_string(first + k) + " is " + got[k]);
        }
        release(schema, batches);
    }

    {
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_take(path, request, wanted, schema, batches, error), name + " take: " + error);
        const auto got = render_rows(schema, batches);
        require(got.size() == wanted.size(), name + " take: " + std::to_string(got.size()) + " rows");
        for (std::size_t k = 0; k < wanted.size(); ++k) {
            require(got[k] == expected(wanted[k]), name + " take: row " + std::to_string(wanted[k]) + " is " + got[k]);
        }
        release(schema, batches);
    }
}

}  // namespace

int main() {
    const std::filesystem::path dir = NANO_LANCE_VARIABLE_PACKED_GOLDEN_DIR;
    // utf8: every row; sparse: a range holding the first value (row 1999) and a take of the first and last values.
    check_file(dir / "utf8_struct.lance", 3, utf8_row, 1, 2, {0, 1, 2});
    check_file(dir / "sparse_list_struct.lance", 70000, sparse_row, 1998, 3, {0, 1999, 69999});
    check_file(dir / "list_of_list_struct.lance", 1, list_of_list_row, 0, 1, {0});
    std::cout << "variable-width packed struct golden files read back: 3 files, whole, by range and by take\n";
    return 0;
}
