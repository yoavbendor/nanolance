// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// encoding/values/dictionary: a Variable block built by encode_variable decodes back to the same entries (no
// entries, empty strings, one entry, 1000 entries, entries with zero bytes, 32-bit offsets past 64 KiB and 64-bit
// offset words); fixed-width blocks of 1, 2, 4, 8 and 16-byte entries, raw, padded, and inline bit-packed; index
// buffers (u32, bit-packed) round-trip; and the refusals of truncated or malformed blocks with their messages.

#include "encoding/values/bitpack.hpp"
#include "encoding/values/dictionary.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;
using nano_lance::dictionary::Block;
using nano_lance::dictionary::Shape;

Block block_of(Bytes bytes) {
    Block block;
    block.bytes = std::move(bytes);
    return block;
}

/// Entry `i` of a Variable block as a string.
std::string entry_string(const Block& block, std::size_t i) {
    return std::string(reinterpret_cast<const char*>(block.entry(i)), block.entry_size(i));
}

void check_variable(const std::string& what, const std::vector<std::string>& entries, bool large) {
    const std::vector<std::string_view> views(entries.begin(), entries.end());
    Block block = block_of(nano_lance::dictionary::encode_variable(views, large));
    std::string error;
    require(nano_lance::dictionary::decode(block, Shape{}, error), what + ": decode failed: " + error);
    require(!block.is_fixed_width(), what + ": a Variable block is not fixed width");
    require(block.count == entries.size(), what + ": " + std::to_string(block.count) + " entries, want " +
                                               std::to_string(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        require(entry_string(block, i) == entries[i], what + ": entry " + std::to_string(i) + " round-trips");
    }
}

void test_variable_round_trips() {
    for (const bool large : {false, true}) {
        const std::string tag = large ? " (64-bit offsets)" : " (32-bit offsets)";
        check_variable("no entries" + tag, {}, large);
        check_variable("one empty string" + tag, {""}, large);
        check_variable("one entry" + tag, {"a"}, large);
        check_variable("empty strings" + tag, {"", "", "x", "", ""}, large);
        check_variable("entries with zero bytes" + tag,
                       {std::string("a\0b", 3), std::string("\0", 1), std::string(2U, '\0'), std::string("z")}, large);

        std::vector<std::string> thousand;
        for (std::size_t i = 0; i < 1000U; ++i) {
            thousand.push_back(i % 3U == 0U ? std::string() : "entry-" + std::to_string(i));
        }
        check_variable("1000 entries" + tag, thousand, large);

        // Data past 64 KiB: the 32-bit offsets reach past 16 bits, and each entry is a whole buffer's worth.
        std::vector<std::string> big;
        for (std::size_t i = 0; i < 300U; ++i) {
            big.emplace_back(1000U, static_cast<char>('a' + static_cast<int>(i % 26U)));
        }
        check_variable("300 entries of 1000 bytes" + tag, big, large);
    }
}

/// Entry `i` of a fixed-width dictionary of `width` bytes: byte k is (i * 31 + k * 7 + 1) mod 256.
std::string fixed_entry(std::size_t width, std::size_t i) {
    std::string out(width, '\0');
    for (std::size_t k = 0; k < width; ++k) {
        out[k] = static_cast<char>((i * 31U + k * 7U + 1U) & 0xFFU);
    }
    return out;
}

void test_fixed_widths() {
    for (const std::size_t width : {1U, 2U, 4U, 8U, 16U}) {
        const std::string tag = std::to_string(width) + "-byte entries";
        std::vector<std::string> entries;
        for (std::size_t i = 0; i < 5U; ++i) {
            entries.push_back(fixed_entry(width, i));
        }
        const Bytes raw = nano_lance::dictionary::encode_fixed(entries);
        require(raw.size() == 5U * width, tag + ": the entries end to end");

        Shape shape;
        shape.fixed_value_bytes = width;
        shape.count = 5U;
        std::string error;
        Block block = block_of(raw);
        require(nano_lance::dictionary::decode(block, shape, error), tag + ": decode failed: " + error);
        require(block.is_fixed_width() && block.value_bytes == width && block.count == 5U, tag + ": shape");
        for (std::size_t i = 0; i < 5U; ++i) {
            require(std::memcmp(block.entry(i), entries[i].data(), width) == 0, tag + ": entry " + std::to_string(i));
        }

        // Lance pads the block: a longer one is accepted, and the entries are the first `count`.
        Bytes padded = raw;
        padded.resize(padded.size() + 8U, 0xEEU);
        Block longer = block_of(padded);
        require(nano_lance::dictionary::decode(longer, shape, error), tag + ": a padded block is accepted");
        require(std::memcmp(longer.entry(4), entries[4].data(), width) == 0, tag + ": padded block, last entry");
    }
}

void test_inline_bitpacked() {
    // Values below 1000 in 2, 4 and 8-byte entries, 1500 of them: two inline FastLanes blocks (1024 and 476).
    for (const std::size_t width : {2U, 4U, 8U}) {
        const std::size_t n = 1500U;
        Bytes values(n * width, 0U);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t v = (i * 37U) % 1000U;
            std::memcpy(values.data() + i * width, &v, width);
        }
        Bytes packed;
        for (std::size_t start = 0; start < n; start += 1024U) {
            const std::size_t count = std::min<std::size_t>(1024U, n - start);
            Bytes block;
            nano_lance::bitpack::encode_block(values.data() + start * width, count, width, block);
            packed.insert(packed.end(), block.begin(), block.end());
        }
        Shape shape;
        shape.fixed_value_bytes = width;
        shape.count = n;
        shape.inline_bitpacked = true;
        Block block = block_of(packed);
        std::string error;
        require(nano_lance::dictionary::decode(block, shape, error), std::to_string(width) + "-byte inline: " + error);
        require(block.bytes == values, std::to_string(width) + "-byte inline entries unpack to the values");
    }
}

void test_index_round_trip() {
    // Dictionary indices: u32, bit-packed in inline blocks of at most 1024, decoded back block by block.
    const std::size_t n = 2500U;
    Bytes indices(n * 4U);
    for (std::size_t i = 0; i < n; ++i) {
        const auto index = static_cast<std::uint32_t>((i * 13U) % 900U);
        std::memcpy(indices.data() + i * 4U, &index, 4U);
    }
    Bytes back;
    for (std::size_t start = 0; start < n; start += 1024U) {
        const std::size_t count = std::min<std::size_t>(1024U, n - start);
        Bytes block;
        nano_lance::bitpack::encode_block(indices.data() + start * 4U, count, 4U, block);
        std::string error;
        require(nano_lance::bitpack::decode_block(block, count, 4U, back, error), "index block decodes: " + error);
    }
    require(back == indices, "index buffer round-trips");
}

void test_variable_refusals() {
    std::string error;
    Block short7 = block_of(Bytes{32U, 0U, 0U, 0U, 8U, 0U, 0U});
    require(!nano_lance::dictionary::decode(short7, Shape{}, error), "a 7-byte block is refused");
    require(error == "dict block too short", "7-byte message: " + error);

    // 64-bit words in 8 bytes: the header needs 16.
    Block short64 = block_of(Bytes{64U, 0U, 0U, 0U, 0U, 0U, 0U, 0U});
    require(!nano_lance::dictionary::decode(short64, Shape{}, error), "a 64-bit header in 8 bytes is refused");
    require(error == "dict block too short", "short 64-bit header message: " + error);

    const Bytes good = nano_lance::dictionary::encode_variable({std::string_view("ab"), std::string_view("cd")}, false);
    require(good.size() == 24U, "the two-entry test block is 24 bytes");
    {
        Bytes bad = good;
        bad[4] = 8U;  // bytes_start 8 < 3 words
        Block b = block_of(bad);
        require(!nano_lance::dictionary::decode(b, Shape{}, error), "a bytes_start before the offsets is refused");
        require(error == "dict block header invalid", "bytes_start before offsets: " + error);
    }
    {
        Bytes bad = good;
        bad[4] = 40U;  // bytes_start past the end
        Block b = block_of(bad);
        require(!nano_lance::dictionary::decode(b, Shape{}, error), "a bytes_start past the block is refused");
        require(error == "dict block header invalid", "bytes_start past the end: " + error);
    }
    {
        Bytes bad = good;
        bad[16] = 1U;  // offset[2] = 1 < offset[1] = 2
        Block b = block_of(bad);
        require(!nano_lance::dictionary::decode(b, Shape{}, error), "decreasing offsets are refused");
        require(error == "dict offsets out of range", "decreasing offsets: " + error);
    }
    {
        Bytes bad = good;
        bad[12] = 9U;  // offset[1] = 9 runs past the 4 data bytes
        Block b = block_of(bad);
        require(!nano_lance::dictionary::decode(b, Shape{}, error), "an offset past the data is refused");
        require(error == "dict offsets out of range", "offset past the data: " + error);
    }
}

void test_fixed_refusals() {
    std::string error;
    Shape none;
    none.fixed_value_bytes = 4U;
    none.count = 0U;
    Block zero = block_of(Bytes(8U, 0U));
    require(!nano_lance::dictionary::decode(zero, none, error), "a dictionary of 0 entries is refused");
    require(error == "dictionary declares an implausible 0 entries of 4 bytes", "zero entries: " + error);

    Shape three;
    three.fixed_value_bytes = 4U;
    three.count = 3U;
    Block eight = block_of(Bytes(8U, 0U));
    require(!nano_lance::dictionary::decode(eight, three, error), "3 entries of 4 bytes in 8 bytes is refused");
    require(error == "dictionary declares 3 entries of 4 bytes but the block holds 8",
            "short fixed block: " + error);
}

}  // namespace

int main() {
    test_variable_round_trips();
    test_fixed_widths();
    test_inline_bitpacked();
    test_index_round_trip();
    test_variable_refusals();
    test_fixed_refusals();
    std::cout << "encoding/values/dictionary: all checks passed\n";
    return 0;
}
