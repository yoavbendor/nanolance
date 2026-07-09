// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// nlbench_gendata: write a synthetic dataset exercising a mix of column shapes (plain fixed-width,
// low-cardinality utf8, high-cardinality utf8, and a constant column) at a chosen row count, so
// bench/read_parity_bench.sh can measure nanolance's own read throughput with default (fully-checked)
// vs trusted_input=true reads. Encoding choice (bitpack/RLE/dict-RLE/constant) is the writer's own
// automatic structural-encoding heuristic — this tool just shapes the data to exercise it, matching
// the same patterns tests/test_*_roundtrip.cpp use.
// Usage: nlbench_gendata <out_dataset.lance> <rows>
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void die(const char* msg) {
    std::cerr << "nlbench_gendata: " << msg << '\n';
    std::exit(1);
}

// Fixed-width int64 column, no external buffers owned beyond `values` (caller keeps it alive).
void fill_int64_array(ArrowArray& array, const std::vector<std::int64_t>& values, const void** buffers) {
    buffers[0] = nullptr;
    buffers[1] = values.data();
    array = ArrowArray{};
    array.length = static_cast<std::int64_t>(values.size());
    array.n_buffers = 2;
    array.buffers = buffers;
}

// utf8 column built from `strings`: owns int32 offsets + concatenated char data via the out-params so
// the caller can keep them alive for the duration of the write call.
void fill_utf8_array(ArrowArray& array, const std::vector<std::string>& strings, std::vector<std::int32_t>& offsets,
                     std::string& data, const void** buffers) {
    offsets.assign(strings.size() + 1U, 0);
    data.clear();
    for (std::size_t i = 0; i < strings.size(); ++i) {
        data += strings[i];
        offsets[i + 1U] = static_cast<std::int32_t>(data.size());
    }
    buffers[0] = nullptr;
    buffers[1] = offsets.data();
    buffers[2] = data.data();
    array = ArrowArray{};
    array.length = static_cast<std::int64_t>(strings.size());
    array.n_buffers = 3;
    array.buffers = buffers;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: nlbench_gendata <out_dataset.lance> <rows>\n";
        return 2;
    }
    const std::string out_path = argv[1];
    const std::int64_t rows = std::atoll(argv[2]);
    if (rows <= 0) {
        die("rows must be positive");
    }

    // id: sequential int64 (plain fixed-width; the writer may bitpack it once structural encoding sees
    // its narrow value range).
    std::vector<std::int64_t> id_values(static_cast<std::size_t>(rows));
    for (std::int64_t i = 0; i < rows; ++i) {
        id_values[static_cast<std::size_t>(i)] = i;
    }

    // category: 4 distinct values cycling -> low-cardinality, a natural dict-RLE/RLE candidate.
    static const char* kCategories[] = {"pending", "active", "archived", "deleted"};
    std::vector<std::string> category_values(static_cast<std::size_t>(rows));
    for (std::int64_t i = 0; i < rows; ++i) {
        category_values[static_cast<std::size_t>(i)] = kCategories[i % 4];
    }

    // label: near-unique strings -> plain variable-width, no repetition to exploit.
    std::vector<std::string> label_values(static_cast<std::size_t>(rows));
    for (std::int64_t i = 0; i < rows; ++i) {
        label_values[static_cast<std::size_t>(i)] = "row-" + std::to_string(i);
    }

    // flag: single repeated value across every row -> a natural constant-encoding candidate.
    std::vector<std::int64_t> flag_values(static_cast<std::size_t>(rows), 1);

    ArrowSchema id_schema{};
    id_schema.format = "l";
    id_schema.name = "id";
    id_schema.flags = 0;

    ArrowSchema category_schema{};
    category_schema.format = "u";
    category_schema.name = "category";
    category_schema.flags = 0;

    ArrowSchema label_schema{};
    label_schema.format = "u";
    label_schema.name = "label";
    label_schema.flags = 0;

    ArrowSchema flag_schema{};
    flag_schema.format = "l";
    flag_schema.name = "flag";
    flag_schema.flags = 0;

    ArrowSchema* children[] = {&id_schema, &category_schema, &label_schema, &flag_schema};
    ArrowSchema root_schema{};
    root_schema.format = "+s";
    root_schema.name = "";
    root_schema.flags = 0;
    root_schema.n_children = 4;
    root_schema.children = children;

    const void* id_buffers[2];
    ArrowArray id_array;
    fill_int64_array(id_array, id_values, id_buffers);

    std::vector<std::int32_t> category_offsets;
    std::string category_data;
    const void* category_buffers[3];
    ArrowArray category_array;
    fill_utf8_array(category_array, category_values, category_offsets, category_data, category_buffers);

    std::vector<std::int32_t> label_offsets;
    std::string label_data;
    const void* label_buffers[3];
    ArrowArray label_array;
    fill_utf8_array(label_array, label_values, label_offsets, label_data, label_buffers);

    const void* flag_buffers[2];
    ArrowArray flag_array;
    fill_int64_array(flag_array, flag_values, flag_buffers);

    ArrowArray* array_children[] = {&id_array, &category_array, &label_array, &flag_array};
    const void* root_buffers[] = {nullptr};
    ArrowArray root_array{};
    root_array.length = rows;
    root_array.n_buffers = 1;
    root_array.buffers = root_buffers;
    root_array.n_children = 4;
    root_array.children = array_children;

    NanoLanceWriter writer{};
    if (nano_lance_writer_init(&writer, out_path.c_str(), 3) != NANO_LANCE_OK) {
        die("writer init failed");
    }
    if (nano_lance_writer_set_compression(&writer, true) != NANO_LANCE_OK) {
        die("set_compression failed");
    }
    if (nano_lance_writer_set_structural_encoding(&writer, true) != NANO_LANCE_OK) {
        die("set_structural_encoding failed");
    }
    if (nano_lance_write_batch(&writer, &root_array, &root_schema) != NANO_LANCE_OK) {
        std::cerr << "write_batch failed: " << nano_lance_writer_last_error(&writer) << '\n';
        return 1;
    }
    if (nano_lance_writer_commit(&writer, false) != NANO_LANCE_OK) {
        std::cerr << "commit failed: " << nano_lance_writer_last_error(&writer) << '\n';
        return 1;
    }
    if (nano_lance_writer_close(&writer) != NANO_LANCE_OK) {
        die("close failed");
    }

    std::cout << "wrote " << rows << " rows to " << out_path << '\n';
    return 0;
}
