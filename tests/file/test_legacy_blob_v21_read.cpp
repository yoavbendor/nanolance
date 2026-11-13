// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// A legacy blob column (`lance-encoding:blob` on a large_binary field) in a format 2.1 file: Lance 2.1 stores it as a
// BlobLayout over a packed (position, size) descriptor, and nanolance reads the bytes back. The golden file in
// tests/golden/legacy_blob_v21 is written by tests/generate_golden_legacy_blob.py; its values are the formula in that
// script: a null on every row i % 7 == 3, an empty value on i % 5 == 1, a 70000-byte value on i % 100 == 0, and
// otherwise (i * 17) % 61 + 1 bytes of (i * 13 + k * 7) & 255. Read whole, by a row range and by a take, and each row
// is checked against the formula. Rust is not needed.

#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifndef NANO_LANCE_LEGACY_BLOB_GOLDEN
#error "NANO_LANCE_LEGACY_BLOB_GOLDEN must name tests/golden/legacy_blob_v21/v21_legacy_blob.lance"
#endif

namespace {

constexpr std::uint64_t kRows = 300;

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

/// The formula the script writes: nullopt for a null, an empty value, or the bytes of row i.
std::optional<std::vector<std::uint8_t>> expected_value(std::uint64_t i) {
    if (i % 7 == 3) {
        return std::nullopt;
    }
    if (i % 5 == 1) {
        return std::vector<std::uint8_t>{};
    }
    const std::uint64_t n = i % 100 == 0 ? 70000U : (i * 17U) % 61U + 1U;
    std::vector<std::uint8_t> bytes(n);
    for (std::uint64_t k = 0; k < n; ++k) {
        bytes[k] = static_cast<std::uint8_t>((i * 13U + k * 7U) & 255U);
    }
    return bytes;
}

/// The rows of one read: each row's id and its bytes (nullopt for a null).
struct Rows {
    std::vector<std::int64_t> id;
    std::vector<std::optional<std::vector<std::uint8_t>>> blob;
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

/// Appends one batch's rows: id (int64) and b (large_binary, the bytes of each row).
void append_batch(const ArrowSchema& schema, const ArrowArray& batch, Rows& rows) {
    const int id_at = child_index(schema, "id");
    const int b_at = child_index(schema, "b");
    require(id_at >= 0 && b_at >= 0, "both columns are read");
    require(std::string(schema.children[b_at]->format) == "Z", "b is read as large_binary");

    const ArrowArray& ids = *batch.children[id_at];
    const ArrowArray& b = *batch.children[b_at];
    const auto* offsets = static_cast<const std::int64_t*>(b.buffers[1]);
    const auto* data = static_cast<const std::uint8_t*>(b.buffers[2]);
    for (std::int64_t r = 0; r < batch.length; ++r) {
        rows.id.push_back(static_cast<const std::int64_t*>(ids.buffers[1])[ids.offset + r]);
        const std::int64_t at = b.offset + r;
        if (!bit(b.buffers[0], at)) {
            rows.blob.emplace_back(std::nullopt);
            continue;
        }
        const std::int64_t begin = offsets[at];
        const std::int64_t end = offsets[at + 1];
        rows.blob.emplace_back(std::vector<std::uint8_t>(data + begin, data + end));
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

/// Row `i` (of the file) is what the formula says; `r` is its position in the read.
void check_row(const Rows& rows, std::size_t r, std::uint64_t i, const std::string& what) {
    const std::string where = what + ": row " + std::to_string(i);
    require(rows.id[r] == static_cast<std::int64_t>(i), where + " has the wrong id");
    const auto want = expected_value(i);
    const auto& got = rows.blob[r];
    require(want.has_value() == got.has_value(), where + " has the wrong null");
    if (!want) {
        return;
    }
    require(got->size() == want->size(),
            where + " has " + std::to_string(got->size()) + " bytes, expected " + std::to_string(want->size()));
    require(*got == *want, where + " has the wrong bytes");
}

void check_file(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
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
            check_row(rows, i, i, name + " whole");
        }
        release(schema, batches);
    }

    // A row range that crosses the 70000-byte value at row 100: rows [95, 140).
    {
        nano_lance::LanceScanRequest request;
        request.range = {95, 45};
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_read(path, request, schema, batches, error), name + " range: " + error);
        const Rows rows = read_rows(schema, batches);
        require(rows.id.size() == 45, name + " range: " + std::to_string(rows.id.size()) + " rows");
        for (std::uint64_t k = 0; k < 45; ++k) {
            check_row(rows, k, 95 + k, name + " range");
        }
        release(schema, batches);
    }

    // A take of rows across the file, ascending as lance_file_take takes them.
    {
        const std::vector<std::uint64_t> wanted = {0, 3, 6, 7, 100, 101, 200, 299};
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_take(path, request, wanted, schema, batches, error), name + " take: " + error);
        const Rows rows = read_rows(schema, batches);
        require(rows.id.size() == wanted.size(), name + " take: " + std::to_string(rows.id.size()) + " rows");
        for (std::size_t k = 0; k < wanted.size(); ++k) {
            check_row(rows, k, wanted[k], name + " take");
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

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path path = argc > 1 ? std::filesystem::path(argv[1])
                                                : std::filesystem::path(NANO_LANCE_LEGACY_BLOB_GOLDEN);
    check_file(path);
    std::cout << "legacy blob (2.1) read back: " << path.filename().string() << ", " << kRows
              << " rows whole, by range and by take\n";
    return 0;
}
