// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// A column's values between Arrow and the encoders (nanolance/column_values.hpp): needs_nested_pages() is true
// only for a column with a list layer or a struct layer that has a null (a leaf under never-null structs keeps
// the flat encodings), and fixed_data() / fixed_size() read the borrowed caller buffer when one is set and the
// owned `fixed` bytes otherwise. append_batch_column_values (tests/arrow/test_array_accessor.cpp) fills these.

#include "nanolance/column_values.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

}  // namespace

int main() {
    using nano_lance::ColumnValues;

    // The defaults: a fixed-width column with no validity and no layers.
    {
        ColumnValues c;
        require(c.kind == ColumnValues::Kind::FixedWidth, "fixed width by default");
        require(c.validity.empty() && c.null_count == 0 && c.rows == 0, "no validity, no rows");
        require(!c.needs_nested_pages(), "no layers: flat pages");
        require(c.fixed_size() == 0, "no bytes");
        require(c.dictionary == nullptr, "no dictionary");
    }

    // needs_nested_pages.
    {
        ColumnValues c;
        ColumnValues::NestedLayer never_null_struct;
        never_null_struct.is_list = false;
        never_null_struct.length = 4;
        c.layers.push_back(never_null_struct);
        require(!c.needs_nested_pages(), "a struct that is never null keeps flat pages");

        c.layers.front().null_count = 1;
        c.layers.front().validity = {0x0E};
        require(c.needs_nested_pages(), "a struct with a null needs nested pages");

        ColumnValues l;
        ColumnValues::NestedLayer list;
        list.offsets = {0, 2, 2, 5};
        list.length = 3;
        l.layers.push_back(list);
        require(l.needs_nested_pages(), "a list always needs nested pages, even without nulls");

        ColumnValues deep;
        deep.layers.push_back(never_null_struct);
        deep.layers.push_back(list);
        require(deep.needs_nested_pages(), "a list under a never-null struct needs them too");
    }

    // fixed_data / fixed_size: owned bytes, then a borrowed buffer, which takes precedence.
    {
        ColumnValues c;
        c.fixed = {1, 2, 3, 4};
        require(c.fixed_data() == c.fixed.data() && c.fixed_size() == 4, "owned bytes");
        const std::uint8_t borrowed[8] = {9, 9, 9, 9, 9, 9, 9, 9};
        c.fixed_borrowed = borrowed;
        c.fixed_borrowed_size = sizeof borrowed;
        require(c.fixed_data() == borrowed && c.fixed_size() == 8, "a borrowed buffer is read instead");
        c.fixed_borrowed = nullptr;
        require(c.fixed_data() == c.fixed.data() && c.fixed_size() == 4, "and the owned bytes once it is cleared");
    }

    std::cout << "column_values: all checks passed\n";
    return 0;
}
