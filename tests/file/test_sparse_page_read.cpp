// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Lance 2.3 sparse page layout (PageLayout field 5) with the shapes nanolance read last: a map, whose key column's page
// carries an all-valid validity layer for the non-nullable key; a fixed-size list of structs, whose page carries a
// fixed-size-list layer; and a list of fixed-size lists of structs, whose page counts its fixed-size list's elements in
// its rows. The golden files in tests/golden/sparse are written by Lance's own encoder tests
// (test_explicit_sparse_map_and_fixed_size_list_roundtrip and test_explicit_sparse_list_fixed_size_list_struct_roundtrip;
// NANOLANCE_RUST_KEEP_PASS keeps them). The values are the tests' own:
//   map_keys_values.lance              Map<utf8 non-null, int32> of four rows: [a:1], null, [], [b:null, c:3].
//   fixed_size_list_struct.lance       FixedSizeList<2 x Struct<value: int32>> of six rows, with null structs and null rows.
//   list_fixed_size_list_struct.lance  List<FixedSizeList<2 x Struct<value: int32>>> of four rows: one fixed-size list, a
//                                      null row, a null and a valid fixed-size list, an empty row.
// Each file is read whole, by a row range and by a take, and every row is rendered and compared with its text.
// Rust is not needed.

#include "golden_render.hpp"
#include "nanolance/lance_table_reader.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef NANO_LANCE_SPARSE_GOLDEN_DIR
#error "NANO_LANCE_SPARSE_GOLDEN_DIR must name tests/golden/sparse"
#endif

namespace {

using golden::release;
using golden::render_rows;
using golden::require;

void check_file(const std::filesystem::path& path, const std::vector<std::string>& expected) {
    const std::string name = path.filename().string();
    const std::uint64_t rows = expected.size();
    std::string error;

    {
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_read(path, request, schema, batches, error), name + " whole: " + error);
        const auto got = render_rows(schema, batches);
        require(got.size() == rows, name + " whole: " + std::to_string(got.size()) + " rows");
        for (std::uint64_t r = 0; r < rows; ++r) {
            require(got[r] == expected[r], name + " whole: row " + std::to_string(r) + " is " + got[r]);
        }
        release(schema, batches);
    }

    {
        // Rows 1 to 3: a null, an empty and a value.
        nano_lance::LanceScanRequest request;
        request.range = {1, 3};
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_read(path, request, schema, batches, error), name + " range: " + error);
        const auto got = render_rows(schema, batches);
        require(got.size() == 3, name + " range: " + std::to_string(got.size()) + " rows");
        for (std::uint64_t k = 0; k < 3; ++k) {
            require(got[k] == expected[1 + k], name + " range: row " + std::to_string(1 + k) + " is " + got[k]);
        }
        release(schema, batches);
    }

    {
        const std::vector<std::uint64_t> wanted = {0, rows - 1};
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_take(path, request, wanted, schema, batches, error), name + " take: " + error);
        const auto got = render_rows(schema, batches);
        require(got.size() == wanted.size(), name + " take: " + std::to_string(got.size()) + " rows");
        for (std::size_t k = 0; k < wanted.size(); ++k) {
            require(got[k] == expected[wanted[k]], name + " take: row " + std::to_string(wanted[k]) + " is " + got[k]);
        }
        release(schema, batches);
    }
}

}  // namespace

int main() {
    const std::filesystem::path dir = NANO_LANCE_SPARSE_GOLDEN_DIR;
    check_file(dir / "map_keys_values.lance",
               {"[{keys:\"a\",values:1}]", "null", "[]", "[{keys:\"b\",values:null},{keys:\"c\",values:3}]"});
    check_file(dir / "fixed_size_list_struct.lance",
               {"[{value:0},{value:1}]", "null", "[{value:4},{value:5}]", "[{value:null},null]", "null",
                "[{value:10},{value:null}]"});
    check_file(dir / "list_fixed_size_list_struct.lance",
               {"[[{value:0},{value:null}]]", "null", "[null,[{value:null},{value:5}]]", "[]"});
    std::cout << "sparse map, fixed-size-list and list-of-fixed-size-list pages read back: 3 files, whole, by range and by take\n";
    return 0;
}
