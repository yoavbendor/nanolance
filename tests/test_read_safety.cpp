// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Negative-corpus / unit tests for the read-path memory-safety substrate. These assert that hostile
// on-disk inputs are rejected *cleanly* (an error return, no crash / no runaway allocation). Run under
// ASan+UBSan in CI, they turn "the reader is safe against malformed files" into an enforced property.

#include "nanolance/data_file_reader.hpp"
#include "nanolance/path_safety.hpp"
#include "nanolance/read_safety.hpp"

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++g_failures;
    }
}

void put_le(std::vector<std::uint8_t>& b, std::size_t off, std::uint64_t v, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        b[off + i] = static_cast<std::uint8_t>((v >> (8U * i)) & 0xFFU);
    }
}

// Builds a minimal 64-byte Lance data-file footer that parses far enough to reach the descriptor-bounds
// and column-count checks. Layout (front pad 8B so file_size==64 >= the reader's 64B minimum):
//   [0,8)   front padding (zeros)
//   [8,48)  five LE u64: global_buffer_offset, descriptor_size, column_metadata_start,
//           column_offsets_start, global_offsets_start
//   [48,52) u32 const1 (== 1)
//   [52,56) u32 num_columns
//   [56,60) u16 minor(=2) + u16 major(=2)
//   [60,64) "LANC"
std::vector<std::uint8_t> make_footer(std::uint64_t global_buffer_offset, std::uint64_t descriptor_size,
                                      std::uint32_t num_columns) {
    std::vector<std::uint8_t> b(64, 0U);
    const std::uint64_t u64_block = 8;      // == magic_idx(60) - 52
    put_le(b, 8, global_buffer_offset, 8);  // u64[0]
    put_le(b, 16, descriptor_size, 8);      // u64[1]
    put_le(b, 24, 0, 8);                    // column_metadata_start
    put_le(b, 32, 0, 8);                    // column_offsets_start
    put_le(b, 40, u64_block, 8);            // global_offsets_start must == abs u64 block (tail_base 0 + 8)
    put_le(b, 48, 1, 4);                    // const1
    put_le(b, 52, num_columns, 4);          // num_columns
    put_le(b, 56, 2, 2);                    // minor
    put_le(b, 58, 2, 2);                    // major
    b[60] = 'L';
    b[61] = 'A';
    b[62] = 'N';
    b[63] = 'C';
    return b;
}

std::filesystem::path write_temp(const std::vector<std::uint8_t>& bytes, const char* tag) {
    auto p = std::filesystem::temp_directory_path() /
             ("nano_lance_read_safety_" + std::string(tag) + ".lance");
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return p;
}

void test_checked_math() {
    std::uint64_t out = 0;
    const auto max = std::numeric_limits<std::uint64_t>::max();
    check(nano_lance::checked_add(2, 3, out) && out == 5, "checked_add basic");
    check(!nano_lance::checked_add(max, 1, out), "checked_add detects overflow");
    check(nano_lance::checked_mul(1000, 1000, out) && out == 1000000, "checked_mul basic");
    check(!nano_lance::checked_mul(max, 2, out), "checked_mul detects overflow");
    check(nano_lance::range_in_bounds(0, 10, 10), "range_in_bounds exact fit");
    check(!nano_lance::range_in_bounds(5, 10, 10), "range_in_bounds past end rejected");
    check(!nano_lance::range_in_bounds(1, max, 10), "range_in_bounds overflow rejected");
    check(nano_lance::load_le<std::uint32_t>(
              reinterpret_cast<const std::uint8_t*>("\x01\x02\x03\x04")) == 0x04030201U,
          "load_le little-endian");
}

void test_footer_column_cap() {
    // num_columns beyond the safety cap, but otherwise well-formed / in-bounds descriptor.
    const auto path = write_temp(make_footer(/*gbo=*/0, /*desc=*/0, /*num_columns=*/0xFFFFFFFFU), "cols");
    nano_lance::pb::FileDescriptor desc;
    nano_lance::LanceDataFileFooterLayout layout;
    std::string error;
    const bool ok = nano_lance::read_lance_data_file_footer_and_descriptor(path, desc, layout, error);
    check(!ok, "huge num_columns must be rejected");
    check(error.find("column count") != std::string::npos, "column-count cap message");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void test_footer_descriptor_overflow() {
    // descriptor_size near UINT64_MAX with a non-zero offset: offset+size wraps -> must be rejected by
    // the overflow-safe range check, not silently pass.
    const auto path = write_temp(
        make_footer(/*gbo=*/8, /*desc=*/std::numeric_limits<std::uint64_t>::max(), /*num_columns=*/1),
        "ovf");
    nano_lance::pb::FileDescriptor desc;
    nano_lance::LanceDataFileFooterLayout layout;
    std::string error;
    const bool ok = nano_lance::read_lance_data_file_footer_and_descriptor(path, desc, layout, error);
    check(!ok, "overflowing descriptor bounds must be rejected");
    check(error.find("descriptor bounds") != std::string::npos, "descriptor-bounds message");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void test_garbage_protobuf_no_crash() {
    // Feeding arbitrary bytes to the protobuf decoders must never crash / over-allocate (ASan-checked);
    // a false/empty return is fine. Exercise a few adversarial shapes.
    const std::vector<std::vector<std::uint8_t>> corpus = {
        {},
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        // field 1, wire type 2 (length-delimited), length 0x7FFFFFFF but no payload -> must not allocate
        {0x0A, 0xFF, 0xFF, 0xFF, 0xFF, 0x07},
        std::vector<std::uint8_t>(256, 0x08),  // repeated varint tags
    };
    for (const auto& bytes : corpus) {
        nano_lance::pb::Manifest m;
        (void)nano_lance::pb::decode_manifest(bytes, m);
        nano_lance::pb::FileDescriptor fd;
        (void)nano_lance::pb::decode_file_descriptor(bytes, fd);
        nano_lance::pb::ColumnMetadata cm;
        (void)nano_lance::pb::decode_column_metadata(bytes, cm);
    }
    check(true, "protobuf decoders survive garbage (ASan enforces no OOB)");
}

void test_trusted_mode_skips_budget_cap_only() {
    // Same footer as test_footer_column_cap: num_columns beyond the safety cap, otherwise well-formed
    // (empty descriptor). Under default limits it's rejected by the column-count budget check.
    const auto path = write_temp(make_footer(/*gbo=*/0, /*desc=*/0, /*num_columns=*/0xFFFFFFFFU), "trusted_cols");
    {
        nano_lance::pb::FileDescriptor desc;
        nano_lance::LanceDataFileFooterLayout layout;
        std::string error;
        const bool ok = nano_lance::read_lance_data_file_footer_and_descriptor(path, desc, layout, error);
        check(!ok && error.find("column count") != std::string::npos,
              "default limits still reject the oversized column count");
    }
    {
        // trusted_input's ONLY effect: the four DoS-budget comparisons no longer trigger. With the cap
        // out of the way this footer is otherwise well-formed (empty descriptor), so the read succeeds.
        nano_lance::ScopedReadLimits trusted(nano_lance::trusted_read_limits());
        nano_lance::pb::FileDescriptor desc;
        nano_lance::LanceDataFileFooterLayout layout;
        std::string error;
        const bool ok = nano_lance::read_lance_data_file_footer_and_descriptor(path, desc, layout, error);
        check(ok, "trusted mode skips the column-count budget check");
        check(layout.num_columns == 0xFFFFFFFFU, "trusted mode preserves the declared column count");
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void test_trusted_mode_still_bounds_checked() {
    // Same footer as test_footer_descriptor_overflow: offset+size wraps. This is a BOUNDS violation, not
    // a budget cap, so trusted mode must still reject it via the overflow-safe range check.
    const auto path = write_temp(
        make_footer(/*gbo=*/8, /*desc=*/std::numeric_limits<std::uint64_t>::max(), /*num_columns=*/1),
        "trusted_ovf");
    nano_lance::ScopedReadLimits trusted(nano_lance::trusted_read_limits());
    nano_lance::pb::FileDescriptor desc;
    nano_lance::LanceDataFileFooterLayout layout;
    std::string error;
    const bool ok = nano_lance::read_lance_data_file_footer_and_descriptor(path, desc, layout, error);
    check(!ok, "trusted mode still rejects overflowing descriptor bounds");
    check(error.find("descriptor bounds") != std::string::npos,
          "trusted mode still reports the descriptor-bounds error, not a silent pass");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void test_scoped_read_limits_restores_previous() {
    const auto& before = nano_lance::active_read_limits();
    const auto before_columns = before.max_columns;
    {
        nano_lance::ScopedReadLimits trusted(nano_lance::trusted_read_limits());
        check(nano_lance::active_read_limits().max_columns == std::numeric_limits<std::uint32_t>::max(),
              "ScopedReadLimits applies the trusted limits while in scope");
    }
    check(nano_lance::active_read_limits().max_columns == before_columns,
          "ScopedReadLimits restores the previous limits on scope exit");
}

void test_path_jail() {
    namespace fs = std::filesystem;
    const fs::path base = fs::path("/dataset") / "data";

    // Legitimate: a bare filename (exactly what the writer stores) resolves under base.
    const auto ok = nano_lance::safe_join_under(base, fs::path("fragment-0.lance"));
    check(ok.has_value() && *ok == base / "fragment-0.lance", "bare filename is allowed");

    // A nested-but-contained path is still fine.
    check(nano_lance::safe_join_under(base, fs::path("0/fragment-0.lance")).has_value(),
          "contained subpath is allowed");

    // Hostile: traversal, absolute paths, and root escapes must all be rejected.
    check(!nano_lance::safe_join_under(base, fs::path("../../etc/passwd")).has_value(),
          "parent traversal rejected");
    check(!nano_lance::safe_join_under(base, fs::path("a/../../b")).has_value(),
          "embedded traversal rejected");
    check(!nano_lance::safe_join_under(base, fs::path("/etc/passwd")).has_value(),
          "absolute path rejected");
    check(!nano_lance::safe_join_under(base, fs::path("")).has_value(), "empty path rejected");
}

}  // namespace

int main() {
    test_checked_math();
    test_footer_column_cap();
    test_footer_descriptor_overflow();
    test_garbage_protobuf_no_crash();
    test_trusted_mode_skips_budget_cap_only();
    test_trusted_mode_still_bounds_checked();
    test_scoped_read_limits_restores_previous();
    test_path_jail();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d read-safety checks failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "read-safety checks passed\n");
    return 0;
}
