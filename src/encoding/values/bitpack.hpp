// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// Lance's bitpacking of fixed-width unsigned values (1, 2, 4 or 8 bytes each), both directions. Values
/// go in FastLanes blocks of 1024: an INLINE block is [its bit width as one value-sized word][the packed
/// block]; an OUT-OF-LINE buffer is whole packed blocks at a width the descriptor gives, then a tail
/// either packed or left raw. Values are little-endian, the block's unused tail zero.
namespace nano_lance::bitpack {

/// The bits (0..8 * width_bytes) the largest of `count` values at `src` needs.
unsigned bit_width(const std::uint8_t* src, std::size_t count, std::size_t width_bytes);

/// One inline block of the `count` (<= 1024) values at `src` into `out` (every byte overwritten).
void encode_block(const std::uint8_t* src, std::size_t count, std::size_t width_bytes, std::vector<std::uint8_t>& out);

/// The first `num_values` (<= 1024) values of the inline block `chunk`, appended to `out_fixed`. False
/// when the block is malformed (too short, a width wider than the value, a size that does not match).
bool decode_block(const std::vector<std::uint8_t>& chunk, std::uint64_t num_values, std::size_t width_bytes,
                  std::vector<std::uint8_t>& out_fixed, std::string& error);

/// `count` values from a buffer of consecutive inline blocks (each with its own width), into `out`.
[[nodiscard]] bool decode_series(const std::vector<std::uint8_t>& buffer, std::size_t count,
                                 std::size_t width_bytes, std::vector<std::uint8_t>& out, std::string& error);

/// `count` values packed out of line at `width` bits into `dest` (count * width_bytes bytes). `what`
/// names the buffer in an error.
[[nodiscard]] bool decode_out_of_line(const std::vector<std::uint8_t>& buffer, std::size_t count, unsigned width,
                                      std::size_t width_bytes, std::uint8_t* dest, const char* what,
                                      std::string& error);

/// The inverse of decode_out_of_line: `count` values at `width` bits, one width for every 1,024-value block
/// with no per-block header, the last block zero-padded. `src` holds `count` values of `width_bytes` each.
void encode_out_of_line(const std::uint8_t* src, std::size_t count, unsigned width, std::size_t width_bytes,
                        std::vector<std::uint8_t>& out);

}  // namespace nano_lance::bitpack
