// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// libFuzzer harness over PAGE DECODING: decode_lance_physical_column, the code that turns a page's
// descriptor and buffers into values. Dictionaries (four block shapes), RLE and dict+RLE, FastLanes
// bit-unpacking inline and out of line, definition levels, constant pages, FSST and LZ4 values,
// byte-stream-split, FullZip -- all of it runs here and nowhere else.
//
// Why it exists: fuzz_decode stops at the data file's footer and column metadata, and the other
// targets each own one sub-format. Nothing fed a real descriptor and real buffers to the decoder
// itself, so every page decoder written against pylance files was bounds-checked by construction and
// never by a fuzzer. README advertised a fuzzer "over the full decode chain"; this makes that true.
//
// Input, one page of one column, in a form `nlance-pagelayout --dump-fuzz-pages` writes from real
// datasets so the corpus starts from pages Lance actually produced:
//
//   [u8  n][n bytes]   the column's Lance logical type ("int64", "string", "fixed_size_binary:300")
//   [u32 rows]         the page's row count (clamped below, so a small input cannot demand gigabytes)
//   [u32 n][n bytes]   ColumnPage.encoding -- the wrapped PageLayout descriptor
//   [u8  count]        number of page buffers, then per buffer [u32 n][n bytes]
//
// All integers little-endian. A malformed input is truncated where it stops parsing, never refused:
// the decoder is what is under test, not this framing.
//
// Contracts asserted, beyond "no crash":
//   * a refusal says why;
//   * decoding is deterministic.
//
// Build with -DNANOLANCE_BUILD_FUZZERS=ON on a Clang toolchain.

#include "nanolance/column_values.hpp"
#include "nanolance/lance_column_decoder.hpp"
#include "nanolance/schema_mapper.hpp"

#include "lance_minimal.pb.hpp"

#include <unistd.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Rows are clamped so the decoder's legitimate row-proportional allocations (a constant page
// expands its one value to every row) stay far below the sanitizer's RSS limit. A real page at this
// size exercises every loop boundary that a larger one would.
constexpr std::uint32_t kMaxRows = 1U << 15U;
constexpr std::size_t kMaxBuffers = 4;

struct Reader {
    const std::uint8_t* p;
    std::size_t left;

    bool u8(std::uint8_t& v) {
        if (left < 1U) return false;
        v = *p++;
        --left;
        return true;
    }
    bool u32(std::uint32_t& v) {
        if (left < 4U) return false;
        std::memcpy(&v, p, 4U);
        p += 4U;
        left -= 4U;
        return true;
    }
    // Takes up to `n` bytes -- fewer if the input runs out, so a truncated input still decodes.
    std::vector<std::uint8_t> bytes(std::size_t n) {
        n = n < left ? n : left;
        std::vector<std::uint8_t> out(p, p + n);
        p += n;
        left -= n;
        return out;
    }
};

std::filesystem::path temp_path() {
    static const auto dir = std::filesystem::temp_directory_path();
    return dir / ("nanolance_fuzz_column_" + std::to_string(static_cast<unsigned long>(::getpid())) + ".bin");
}

bool decode_once(const std::filesystem::path& file, const nano_lance::pb::Field& field,
                 const nano_lance::pb::ColumnMetadata& column, nano_lance::ColumnValues& out, std::string& error) {
    return nano_lance::decode_lance_physical_column(file, field, column, out, error);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    Reader in{data, size};

    std::uint8_t type_len = 0;
    if (!in.u8(type_len)) {
        return 0;
    }
    const auto type_bytes = in.bytes(type_len);
    std::uint32_t rows = 0;
    std::uint32_t desc_len = 0;
    if (!in.u32(rows) || !in.u32(desc_len)) {
        return 0;
    }
    rows = rows > kMaxRows ? kMaxRows : rows;
    const auto descriptor = in.bytes(desc_len);
    std::uint8_t buffer_count = 0;
    (void)in.u8(buffer_count);
    std::vector<std::vector<std::uint8_t>> buffers;
    for (std::size_t b = 0; b < buffer_count && b < kMaxBuffers; ++b) {
        std::uint32_t n = 0;
        if (!in.u32(n)) {
            break;
        }
        buffers.push_back(in.bytes(n));
    }

    // Lay the buffers out in a file the way a data file would: 64-byte aligned, in order. A fresh
    // inode every time -- unlink first -- so the reader's open-file cache can never hand one input
    // another input's bytes.
    const auto file = temp_path();
    std::error_code ec;
    std::filesystem::remove(file, ec);
    nano_lance::pb::ColumnMetadata column;
    nano_lance::pb::ColumnPage page;
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        std::uint64_t at = 0;
        for (const auto& buffer : buffers) {
            const std::uint64_t pad = (64U - (at % 64U)) % 64U;
            static const char zeros[64] = {};
            out.write(zeros, static_cast<std::streamsize>(pad));
            at += pad;
            page.buffer_offsets.push_back(at);
            page.buffer_sizes.push_back(buffer.size());
            out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            at += buffer.size();
        }
    }
    page.length = rows;
    page.encoding = descriptor;
    column.pages.push_back(page);

    nano_lance::pb::Field field;
    field.name = "c";
    field.logical_type.assign(type_bytes.begin(), type_bytes.end());
    field.encoding = nano_lance::lance_on_disk_field_encoding(field.logical_type);
    field.nullable = true;

    nano_lance::ColumnValues first;
    std::string first_error;
    const bool ok = decode_once(file, field, column, first, first_error);
    if (!ok) {
        assert(!first_error.empty() && "a refused page must say why");
    }

    nano_lance::ColumnValues second;
    std::string second_error;
    const bool ok_again = decode_once(file, field, column, second, second_error);
    assert(ok == ok_again && "decoding must be deterministic");
    if (ok) {
        assert(first.fixed == second.fixed && first.variable.data == second.variable.data &&
               first.variable.offsets == second.variable.offsets && first.validity == second.validity &&
               first.null_count == second.null_count && "decoding must be deterministic");
    }
    (void)ok_again;

    std::filesystem::remove(file, ec);
    return 0;
}
