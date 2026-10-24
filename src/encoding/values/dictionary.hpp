// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Lance's dictionary block, both directions: the distinct values of a column (or of a page) in one buffer,
/// and the rows as indices into it (the indices are a separate buffer, bit-packed by values/bitpack). Two
/// shapes, which the descriptor says apart:
///  - Variable: the VariableEncoder block, [u32 or u64 LE bits_per_offset][u32 or u64 LE bytes_start]
///    [N+1 offsets, relative to the data, the first 0][data]. Every header word is as wide as bits_per_offset
///    (32, or 64 for a large_utf8 / large_binary column).
///  - Fixed width: N entries of `width` bytes end to end, no header. The count is the descriptor's
///    num_dictionary_items, and the block may be longer than the entries need (Lance pads it). Its entries
///    may be FastLanes bit-packed, inline (a series of blocks) or out of line (at the descriptor's width).
/// The block's buffer compression (zstd, lz4) is the caller's, and so are the page's index chunks.
namespace nano_lance::dictionary {

/// A dictionary block as decoded: `bytes` holds the entries (for a Variable block, the header and the data
/// in place; `ranges` gives each entry's start and length within it).
struct Block {
    std::vector<std::uint8_t> bytes;
    /// Variable-width: (start, length) per entry. Empty for a fixed-width dictionary.
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    /// Fixed-width: bytes per entry. Zero when the dictionary is variable-width.
    std::size_t value_bytes = 0;
    std::size_t count = 0;

    bool is_fixed_width() const { return value_bytes != 0U; }
    const std::uint8_t* entry(std::size_t index) const {
        return bytes.data() + (is_fixed_width() ? index * value_bytes : ranges[index].first);
    }
    std::size_t entry_size(std::size_t index) const { return is_fixed_width() ? value_bytes : ranges[index].second; }
};

/// What the descriptor says of a dictionary block's shape: a fixed-width one (`fixed_value_bytes` > 0, with
/// `count` entries, possibly bit-packed as stated) or a Variable one (`fixed_value_bytes` 0).
struct Shape {
    std::size_t fixed_value_bytes = 0;
    /// Fixed-width: num_dictionary_items. A headerless block states neither its width nor its count.
    std::uint64_t count = 0;
    bool inline_bitpacked = false;
    bool out_of_line_bitpacked = false;
    /// Out-of-line packing: the bit width the entries are packed at.
    std::uint32_t packed_width = 0;
};

/// The Variable block of `entries` (32-bit offsets, or 64-bit words when `large`).
std::vector<std::uint8_t> encode_variable(const std::vector<std::string_view>& entries, bool large);

/// The fixed-width block of `entries`, which all have the same width: the entries end to end.
std::vector<std::uint8_t> encode_fixed(const std::vector<std::string>& entries);

/// Parses the block in `block.bytes` (already decompressed) as `shape` says, in place: a Variable block gets its
/// ranges, a fixed-width one is unpacked when its entries are bit-packed. False, with `error`, for a block
/// that is too short, has a header or offsets that do not hold together, or does not hold the entries
/// `shape` declares.
bool decode(Block& block, const Shape& shape, std::string& error);

}  // namespace nano_lance::dictionary
