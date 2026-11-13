// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// fixed_size_list<fixed_size_list<int32, 2>, 2> is written (two validity levels over the leaves, as pylance 12 writes
// it at format 2.2) and reads back with its nulls in place: a null row, a null leaf, and a null inner list.
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

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

bool bit(const std::uint8_t* bits, std::int64_t k) { return ((bits[k >> 3] >> (k & 7)) & 1U) != 0U; }

}  // namespace

int main() {
    const auto ds = std::filesystem::temp_directory_path() / "nano_lance_fsl_of_fsl_write";
    std::error_code ec;
    std::filesystem::remove_all(ds, ec);

    // Four rows of fixed_size_list<fixed_size_list<int32, 2>, 2>: 2 inner lists a row, 2 leaves an inner list.
    //   row 0: [[1, 2], [3, 4]]
    //   row 1: null (its inner lists and leaves are null too)
    //   row 2: [[5, null], [7, 8]]
    //   row 3: [[9, 10], null]
    const std::int32_t leaf_values[16] = {1, 2, 3, 4, 0, 0, 0, 0, 5, 0, 7, 8, 9, 10, 0, 0};
    const std::uint8_t leaf_valid[2] = {0x0FU, 0x3DU};   // idx 0-3, 8, 10, 11, 12, 13 valid
    const std::uint8_t inner_valid[1] = {0x73U};         // m0, m1, m4, m5, m6 valid
    const std::uint8_t outer_valid[1] = {0x0DU};         // rows 0, 2, 3 valid
    const void* leaf_buffers[] = {leaf_valid, leaf_values};
    const void* inner_buffers[] = {inner_valid};
    const void* outer_buffers[] = {outer_valid};

    ArrowArray leaf{};
    leaf.length = 16;
    leaf.null_count = 7;
    leaf.n_buffers = 2;
    leaf.buffers = leaf_buffers;
    ArrowArray* inner_children[] = {&leaf};
    ArrowArray inner{};
    inner.length = 8;
    inner.null_count = 3;
    inner.n_buffers = 1;
    inner.buffers = inner_buffers;
    inner.n_children = 1;
    inner.children = inner_children;
    ArrowArray* outer_children[] = {&inner};
    ArrowArray batch{};
    batch.length = 4;
    batch.null_count = 1;
    batch.n_buffers = 1;
    batch.buffers = outer_buffers;
    batch.n_children = 1;
    batch.children = outer_children;

    ArrowSchema leaf_schema{};
    leaf_schema.format = "i";
    leaf_schema.name = "item";
    ArrowSchema* inner_schema_children[] = {&leaf_schema};
    ArrowSchema inner_schema{};
    inner_schema.format = "+w:2";
    inner_schema.name = "item";
    inner_schema.n_children = 1;
    inner_schema.children = inner_schema_children;
    ArrowSchema* outer_schema_children[] = {&inner_schema};
    ArrowSchema field{};
    field.format = "+w:2";
    field.name = "c";
    field.n_children = 1;
    field.children = outer_schema_children;

    // The batch is a record batch of one column, as pyarrow passes it: a struct whose child is the column.
    ArrowArray* record_children[] = {&batch};
    const void* record_buffers[] = {nullptr};
    ArrowArray record{};
    record.length = 4;
    record.n_buffers = 1;
    record.buffers = record_buffers;
    record.n_children = 1;
    record.children = record_children;
    ArrowSchema* top_children[] = {&field};
    ArrowSchema top{};
    top.format = "+s";
    top.name = "";
    top.n_children = 1;
    top.children = top_children;

    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    const int rc = nano_lance_write_batch(&writer, &record, &top);
    require(rc == NANO_LANCE_OK, "a fixed_size_list of fixed_size_list must be written: " +
                                     std::string(nano_lance_writer_last_error(&writer)));
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    nano_lance_writer_close(&writer);

    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), "read: " + error);
    // Arrow's offsets: element (r, j, i) of the outer array is at raw index outer.offset + r, the inner list at
    // mid.offset + (outer.offset + r) * 2 + j, and its leaf at leaves.offset + (mid.offset + that) * 2 + i.
    std::int64_t row = 0;
    for (auto& b : batches) {  // one per fragment or row range
        require(b.n_children == 1, "one column");
        const ArrowArray& outer = *b.children[0];
        const ArrowArray& mid = *outer.children[0];
        const ArrowArray& leaves = *mid.children[0];
        const auto* outer_bits = static_cast<const std::uint8_t*>(outer.buffers[0]);
        const auto* mid_bits = static_cast<const std::uint8_t*>(mid.buffers[0]);
        const auto* leaf_bits = static_cast<const std::uint8_t*>(leaves.buffers[0]);
        const auto* leaf_data = static_cast<const std::int32_t*>(leaves.buffers[1]);
        for (std::int64_t r = 0; r < outer.length; ++r, ++row) {
            const bool row_valid = outer.null_count == 0 || bit(outer_bits, outer.offset + r);
            require(row_valid == (row != 1), "row validity at row " + std::to_string(row));
            if (!row_valid) {
                continue;
            }
            for (std::int64_t j = 0; j < 2; ++j) {
                const std::int64_t m = mid.offset + (outer.offset + r) * 2 + j;
                const bool inner_ok = mid.null_count == 0 || bit(mid_bits, m);
                require(inner_ok == !(row == 3 && j == 1), "inner validity at row " + std::to_string(row));
                if (!inner_ok) {
                    continue;
                }
                for (std::int64_t i = 0; i < 2; ++i) {
                    const std::int64_t k = leaves.offset + m * 2 + i;
                    const bool leaf_ok = leaves.null_count == 0 || bit(leaf_bits, k);
                    require(leaf_ok == !(row == 2 && j == 0 && i == 1), "leaf validity at row " + std::to_string(row));
                    if (leaf_ok) {
                        require(leaf_data[k] == leaf_values[row * 4 + j * 2 + i],
                                "leaf value at row " + std::to_string(row));
                    }
                }
            }
        }
    }
    require(row == 4, "four rows read back, got " + std::to_string(row));
    std::filesystem::remove_all(ds, ec);
    std::cerr << "fixed_size_list of fixed_size_list writes and reads back with its nulls\n";
    return 0;
}
