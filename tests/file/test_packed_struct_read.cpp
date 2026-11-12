// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Lance 2.1 and 2.2 packed structs (`lance-encoding:packed`): a struct whose fixed-width children pylance wrote
// as one column, their bytes interleaved row by row. The golden files in tests/golden/packed_struct are written by
// tests/generate_golden_packed_struct.py, and their values are a formula: id = i, s.a = 3 i, s.b = i + 7, with
// s null on every row i % 7 == 3 in the nullable file. Each file is read whole, by a row range and by a take, and
// each row is checked against the formula. Rust is not needed.

#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef NANO_LANCE_PACKED_GOLDEN_DIR
#error "NANO_LANCE_PACKED_GOLDEN_DIR must name tests/golden/packed_struct"
#endif

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

/// The rows of one read, as the test checks them.
struct Rows {
    std::vector<std::int64_t> id;
    std::vector<std::uint64_t> a;
    std::vector<std::uint32_t> b;
    std::vector<bool> valid;  // the struct s is not null
};

bool bit(const void* validity, std::int64_t at) {
    if (validity == nullptr) {
        return true;
    }
    const auto* bits = static_cast<const std::uint8_t*>(validity);
    return ((bits[at >> 3] >> (at & 7)) & 1U) != 0U;
}

int child_index(const ArrowSchema& schema, const std::string& name) {
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (name == schema.children[i]->name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// Appends one batch's rows: a struct array of (id, s), s of (a, b).
void append_batch(const ArrowSchema& schema, const ArrowArray& batch, Rows& rows) {
    const int id_at = child_index(schema, "id");
    const int s_at = child_index(schema, "s");
    require(id_at >= 0 && s_at >= 0, "both columns are read");
    require(schema.children[s_at]->n_children == 2, "s has two children, a and b");
    const int a_at = child_index(*schema.children[s_at], "a");
    const int b_at = child_index(*schema.children[s_at], "b");
    require(a_at == 0 && b_at == 1, "s has a then b");

    const ArrowArray& ids = *batch.children[id_at];
    const ArrowArray& s = *batch.children[s_at];
    const ArrowArray& a = *s.children[a_at];
    const ArrowArray& b = *s.children[b_at];
    const auto n = batch.length;
    for (std::int64_t r = 0; r < n; ++r) {
        rows.id.push_back(static_cast<const std::int64_t*>(ids.buffers[1])[ids.offset + r]);
        const bool valid = bit(s.buffers[0], s.offset + r);
        rows.valid.push_back(valid);
        rows.a.push_back(static_cast<const std::uint64_t*>(a.buffers[1])[a.offset + r]);
        rows.b.push_back(static_cast<const std::uint32_t*>(b.buffers[1])[b.offset + r]);
    }
}

Rows read_rows(const ArrowSchema& schema, const std::vector<ArrowArray>& batches) {
    Rows rows;
    for (const auto& batch : batches) {
        append_batch(schema, batch, rows);
    }
    return rows;
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

/// Row `i` (of the file) is what the formula says it is; `r` is its position in the read.
void check_row(const Rows& rows, std::size_t r, std::uint64_t i, bool nullable, const std::string& what) {
    const auto row = static_cast<std::int64_t>(i);
    require(rows.id[r] == row, what + ": row " + std::to_string(i) + " has the wrong id");
    const bool expect_null = nullable && i % 7 == 3;
    require(rows.valid[r] == !expect_null, what + ": row " + std::to_string(i) + " has the wrong struct validity");
    if (expect_null) {
        return;
    }
    require(rows.a[r] == 3U * i, what + ": row " + std::to_string(i) + " has the wrong a");
    require(rows.b[r] == static_cast<std::uint32_t>(i + 7U), what + ": row " + std::to_string(i) + " has the wrong b");
}

void check_file(const std::filesystem::path& path, bool nullable) {
    const std::string name = path.filename().string();
    constexpr std::uint64_t kRows = 1300;
    std::string error;

    // Whole: every row, in order.
    {
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_read(path, request, schema, batches, error), name + " whole: " + error);
        const Rows rows = read_rows(schema, batches);
        require(rows.id.size() == kRows, name + " whole: " + std::to_string(rows.id.size()) + " rows");
        for (std::uint64_t i = 0; i < kRows; ++i) {
            check_row(rows, i, i, nullable, name + " whole");
        }
        release(schema, batches);
    }

    // A row range inside a page: rows [100, 150).
    {
        nano_lance::LanceScanRequest request;
        request.range = {100, 50};
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_read(path, request, schema, batches, error), name + " range: " + error);
        const Rows rows = read_rows(schema, batches);
        require(rows.id.size() == 50, name + " range: " + std::to_string(rows.id.size()) + " rows");
        for (std::uint64_t k = 0; k < 50; ++k) {
            check_row(rows, k, 100 + k, nullable, name + " range");
        }
        release(schema, batches);
    }

    // A take of rows across the file, ascending as lance_file_take takes them.
    {
        const std::vector<std::uint64_t> wanted = {0, 3, 6, 7, 500, 1000, 1299};
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_take(path, request, wanted, schema, batches, error), name + " take: " + error);
        const Rows rows = read_rows(schema, batches);
        require(rows.id.size() == wanted.size(), name + " take: " + std::to_string(rows.id.size()) + " rows");
        for (std::size_t k = 0; k < wanted.size(); ++k) {
            check_row(rows, k, wanted[k], nullable, name + " take");
        }
        release(schema, batches);

        // A row past the end is refused, with the reason.
        ArrowSchema bad_schema{};
        std::vector<ArrowArray> bad;
        require(!nano_lance::lance_file_take(path, request, {kRows}, bad_schema, bad, error),
                name + " take: a row past the end is refused");
        require(!error.empty(), name + " take: the refusal says why");
        release(bad_schema, bad);
    }
}

/// The same file as a dataset (its manifest, then its one fragment): whole, by range and by take, through the
/// dataset's own readers, which map the packed struct from the manifest's fields.
void check_dataset(const std::filesystem::path& dataset, bool nullable) {
    const std::string name = dataset.filename().string();
    constexpr std::uint64_t kRows = 1300;
    std::string error;
    {
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_table_read_dataset_range(dataset, nullptr, nano_lance::LanceRowRange{0, kRows},
                                                           schema, batches, error),
                name + " whole: " + error);
        const Rows rows = read_rows(schema, batches);
        require(rows.id.size() == kRows, name + " whole: " + std::to_string(rows.id.size()) + " rows");
        for (std::uint64_t i = 0; i < kRows; ++i) {
            check_row(rows, i, i, nullable, name + " whole");
        }
        release(schema, batches);
    }
    {
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_table_read_dataset_range(dataset, nullptr, nano_lance::LanceRowRange{100, 50},
                                                           schema, batches, error),
                name + " range: " + error);
        const Rows rows = read_rows(schema, batches);
        require(rows.id.size() == 50, name + " range: " + std::to_string(rows.id.size()) + " rows");
        for (std::uint64_t k = 0; k < 50; ++k) {
            check_row(rows, k, 100 + k, nullable, name + " range");
        }
        release(schema, batches);
    }
    {
        const std::vector<std::uint64_t> wanted = {0, 3, 6, 7, 500, 1000, 1299};
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_table_take(dataset, nullptr, wanted, schema, batches, error), name + " take: " + error);
        const Rows rows = read_rows(schema, batches);
        require(rows.id.size() == wanted.size(), name + " take: " + std::to_string(rows.id.size()) + " rows");
        for (std::size_t k = 0; k < wanted.size(); ++k) {
            check_row(rows, k, wanted[k], nullable, name + " take");
        }
        release(schema, batches);
    }
}

/// A 2.2 list<int32> whose values are all 7, with every third row null (a constant page with levels, and no struct
/// anywhere). It must read as the list it is, not be taken for a packed struct: the rows are checked one by one.
void check_const_list(const std::filesystem::path& path) {
    constexpr std::uint64_t kRows = 1300;
    std::string error;
    nano_lance::LanceScanRequest request;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    require(nano_lance::lance_file_read(path, request, schema, batches, error), "const list: " + error);
    require(schema.n_children == 1, "const list: one column");
    std::uint64_t row = 0;
    for (const auto& batch : batches) {
        const ArrowArray& list = *batch.children[0];
        const ArrowArray& items = *list.children[0];
        const auto* offsets = static_cast<const std::int32_t*>(list.buffers[1]);
        const auto* values = static_cast<const std::int32_t*>(items.buffers[1]);
        for (std::int64_t r = 0; r < batch.length; ++r, ++row) {
            const bool valid = bit(list.buffers[0], list.offset + r);
            require(valid == (row % 3 != 0), "const list: row " + std::to_string(row) + " has the wrong validity");
            if (!valid) {
                continue;
            }
            const auto begin = offsets[list.offset + r];
            const auto end = offsets[list.offset + r + 1];
            require(end - begin == 1, "const list: row " + std::to_string(row) + " is one item long");
            require(values[items.offset + begin] == 7, "const list: row " + std::to_string(row) + " holds 7");
        }
    }
    require(row == kRows, "const list: " + std::to_string(row) + " rows");
    release(schema, batches);
}

}  // namespace

int main() {
    const std::filesystem::path dir = NANO_LANCE_PACKED_GOLDEN_DIR;
    check_file(dir / "v21_packed.lance", false);
    check_file(dir / "v22_packed.lance", false);
    check_file(dir / "v21_packed_nullable.lance", true);
    check_dataset(dir / "v21_dataset", false);
    check_const_list(dir / "v22_const_list.lance");
    std::cout << "packed struct reads: 2.1, 2.2 and a nullable struct, whole, by range and by take; "
                 "and a 2.1 dataset\n";
    return 0;
}
