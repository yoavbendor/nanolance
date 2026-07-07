// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Verifies that changing the schema between batches in one writer session is rejected with an
// informative error (names the kind of change and the offending column), not a generic message.
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

void require_contains(const std::string& haystack, const char* needle) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << "FAIL: expected error to contain '" << needle << "' but got: " << haystack << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_schema_change_" + std::string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

// Writes one 1-row batch for a single fixed-width column of the given Arrow format.
template <typename T>
int write_one(NanoLanceWriter& writer, const char* name, const char* format, T value) {
    ArrowArray batch{};
    batch.length = 1;
    batch.n_buffers = 2;
    const void* buffers[] = {nullptr, &value};
    batch.buffers = buffers;
    ArrowSchema field{};
    field.format = format;
    field.name = name;
    field.flags = 0;
    return nano_lance_write_batch(&writer, &batch, &field);
}

}  // namespace

int main() {
    // 1. Type change: id:int64 -> id:int32.
    {
        const auto ds = temp_dataset("type");
        NanoLanceWriter writer{};
        require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
        require(write_one<std::int64_t>(writer, "id", "l", 1) == NANO_LANCE_OK, "first batch");
        const int rc = write_one<std::int32_t>(writer, "id", "i", 2);
        require(rc == NANO_LANCE_UNSUPPORTED, "type change must be rejected");
        const std::string err = nano_lance_writer_last_error(&writer);
        require_contains(err, "type mismatch at column 0");
        require_contains(err, "int64");
        require_contains(err, "int32");
        nano_lance_writer_close(&writer);
        std::error_code ec;
        std::filesystem::remove_all(ds, ec);
    }

    // 2. Column-name/order change: id -> other.
    {
        const auto ds = temp_dataset("name");
        NanoLanceWriter writer{};
        require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
        require(write_one<std::int64_t>(writer, "id", "l", 1) == NANO_LANCE_OK, "first batch");
        const int rc = write_one<std::int64_t>(writer, "other", "l", 2);
        require(rc == NANO_LANCE_UNSUPPORTED, "name change must be rejected");
        const std::string err = nano_lance_writer_last_error(&writer);
        require_contains(err, "field name/order mismatch at column 0");
        require_contains(err, "'id'");
        require_contains(err, "'other'");
        nano_lance_writer_close(&writer);
        std::error_code ec;
        std::filesystem::remove_all(ds, ec);
    }

    std::cerr << "schema-change errors are informative\n";
    return 0;
}
