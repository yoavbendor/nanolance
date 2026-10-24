// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "encoding/values/dictionary.hpp"

#include "encoding/values/bitpack.hpp"
#include "nanolance/read_safety.hpp"

#include <cstring>
#include <string>
#include <utility>

namespace nano_lance::dictionary {

namespace {

/// `v` as a little-endian word of `large ? 8 : 4` bytes (32-bit words keep the low 32 bits).
void append_word(std::vector<std::uint8_t>& out, std::uint64_t v, bool large) {
    const std::size_t width = large ? 8U : 4U;
    for (std::size_t i = 0; i < width; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8U * i)) & 0xFFU));
    }
}

}  // namespace

// Lance's VariableEncoder block format. For large_utf8 / large_binary every one of those words is a u64 and
// bits_per_offset is 64: Lance decodes the dictionary straight into the column's Arrow type and refuses 32-bit
// offsets for a large one ("expected 64-bit offsets but got 32-bit offsets"). Its descriptor says Flat(64) to
// match.
std::vector<std::uint8_t> encode_variable(const std::vector<std::string_view>& entries, bool large) {
    const std::size_t n = entries.size();
    const std::size_t word = large ? 8U : 4U;
    std::vector<std::uint8_t> out;
    append_word(out, large ? 64U : 32U, large);            // bits_per_offset
    append_word(out, 2U * word + (n + 1U) * word, large);  // where the data bytes start
    std::uint64_t cum = 0;
    append_word(out, 0U, large);  // offset[0]
    for (const auto& s : entries) {
        cum += s.size();
        append_word(out, cum, large);
    }
    for (const auto& s : entries) {
        out.insert(out.end(), s.begin(), s.end());
    }
    return out;
}

std::vector<std::uint8_t> encode_fixed(const std::vector<std::string>& entries) {
    std::vector<std::uint8_t> out;
    for (const auto& e : entries) {
        out.insert(out.end(), e.begin(), e.end());
    }
    return out;
}

bool decode(Block& block, const Shape& shape, std::string& error) {
    block.ranges.clear();
    if (shape.fixed_value_bytes != 0U) {
        // A headerless block states neither its entry width nor its count, so `count` is the only statement of how
        // many there are. Lance pads the block, so it may be longer than the entries need -- it must never be shorter.
        block.value_bytes = shape.fixed_value_bytes;
        block.count = static_cast<std::size_t>(shape.count);
        std::uint64_t needed = 0;
        if (block.count == 0U ||
            !checked_mul(shape.count, static_cast<std::uint64_t>(block.value_bytes), needed) ||
            !fits_size_t(needed) || needed > default_read_limits().max_uncompressed_bytes) {
            error = "dictionary declares an implausible " + std::to_string(shape.count) + " entries of " +
                    std::to_string(block.value_bytes) + " bytes";
            return false;
        }
        if (shape.out_of_line_bitpacked) {
            std::vector<std::uint8_t> unpacked(static_cast<std::size_t>(needed));
            if (!bitpack::decode_out_of_line(block.bytes, block.count, shape.packed_width, block.value_bytes,
                                             unpacked.data(), "dictionary block", error)) {
                return false;
            }
            block.bytes = std::move(unpacked);
            return true;
        }
        if (shape.inline_bitpacked) {
            // The count is what bounds the walk, and the buffer is what bounds the count: a series of packed blocks
            // is far smaller than the values it yields, so the size check below cannot be applied to the stored
            // bytes. `bitpack::decode_series` refuses a count the buffer cannot actually supply, which is the same
            // guarantee from the other direction.
            std::vector<std::uint8_t> unpacked;
            if (!bitpack::decode_series(block.bytes, block.count, block.value_bytes, unpacked, error)) {
                return false;
            }
            block.bytes = std::move(unpacked);
            return true;
        }
        if (static_cast<std::size_t>(needed) > block.bytes.size()) {
            error = "dictionary declares " + std::to_string(shape.count) + " entries of " +
                    std::to_string(block.value_bytes) + " bytes but the block holds " +
                    std::to_string(block.bytes.size());
            return false;
        }
        return true;
    }

    block.value_bytes = 0;
    // [bits_per_offset][bytes_start][offsets (count + 1)][data], every word as wide as bits_per_offset says: 32, or 64
    // for a large_utf8 / large_binary dictionary.
    if (block.bytes.size() < 8U) {
        error = "dict block too short";
        return false;
    }
    std::uint32_t bits_per_offset = 0;
    std::memcpy(&bits_per_offset, block.bytes.data(), 4U);
    const std::size_t word = bits_per_offset == 64U ? 8U : 4U;
    const auto read_word = [&](std::size_t at) {
        std::uint64_t v = 0;
        std::memcpy(&v, block.bytes.data() + at, word);  // little-endian; the upper bytes stay 0 for u32
        return v;
    };
    if (block.bytes.size() < 2U * word) {
        error = "dict block too short";
        return false;
    }
    const std::uint64_t bytes_start = read_word(word);
    if (bytes_start < 3U * word || bytes_start > block.bytes.size() || (bytes_start - 2U * word) % word != 0U) {
        error = "dict block header invalid";
        return false;
    }
    block.count = static_cast<std::size_t>((bytes_start - 2U * word) / word - 1U);
    block.ranges.resize(block.count);
    for (std::size_t d = 0; d < block.count; ++d) {
        const std::uint64_t a = read_word(2U * word + d * word);
        const std::uint64_t b = read_word(2U * word + (d + 1U) * word);
        // Compared without adding: `bytes_start + b` in u32 once wrapped for an offset near 2^32 and handed the copy
        // a ~4 GiB entry length (found by fuzz_column_decode); with u64 words the sum can wrap too.
        if (b < a || b > block.bytes.size() - bytes_start) {
            error = "dict offsets out of range";
            return false;
        }
        block.ranges[d] = {static_cast<std::size_t>(bytes_start + a), static_cast<std::size_t>(b - a)};
    }
    return true;
}

}  // namespace nano_lance::dictionary
