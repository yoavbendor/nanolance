// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

/// Lance's run-length encoding of a buffer of values, both directions (Lance's `encodings/physical/`, the
/// RLE value encoding). A run is one value and its length. The encoded form is two buffers: the run values
/// (one `width`-byte value per run) and the run lengths (one u8 per run, so a run longer than 255 is split
/// into runs of at most 255 carrying the same value). Dictionary-RLE stores the run values as u32 indices
/// (4 bytes each, little-endian) instead. Definition levels use the same runs in one block:
/// [u64 LE values size][run values][run lengths], the widths named by the descriptor's Rle{Flat, Flat}.
/// The chunking of a page around these buffers is the page decoder's and writer's job, not this module's.
namespace nano_lance::rle {

/// Appends one run of `length` rows of `value` (value.size() bytes) to `values`, and its length to `lengths`
/// as one or more u8 lengths of at most 255.
void append_run(std::span<const std::uint8_t> value, std::uint64_t length, std::vector<std::uint8_t>& values,
                std::vector<std::uint8_t>& lengths);

/// The same for a dictionary index: `length` rows of the index, as a 4-byte little-endian run value.
void append_index_run(std::uint32_t index, std::uint64_t length, std::vector<std::uint8_t>& values,
                      std::vector<std::uint8_t>& lengths);

/// Rows per mini-block chunk of a run-length page, pylance's rule (measured for W1q, see SPEC_GAPS): the
/// largest power of two up to 4096 whose rows encode to at most 8192 bytes, where the encoding is an 8-byte
/// header plus one (value, length) pair per run piece (a run longer than 255 is `ceil(length / 255)` pieces),
/// padded to 8. The last chunk takes the rows that remain. `run_lengths` are the column's runs in order (each
/// at least 1); `value_bytes` is the width of one run value. The returned counts sum to the total rows.
std::vector<std::size_t> chunk_rows(std::span<const std::uint64_t> run_lengths, std::size_t value_bytes,
                                    std::size_t max_rows = 4096U);

/// Expands the runs (`values`: one `width`-byte value per run; `lengths`: one u8 length per run) onto the end
/// of `out`. False, with `error`, when `width` is 0 or `values` is not a whole number of values, when the
/// number of runs and lengths disagree, or when the expansion would pass the decoded-size limit.
bool decode(std::span<const std::uint8_t> values, std::span<const std::uint8_t> lengths, std::size_t width,
            std::vector<std::uint8_t>& out, std::string& error);

/// Run-length definition levels: `block` is [u64 LE values size][run values][run lengths], with `value_bits`
/// (8 or 16) per run value and `length_bits` (8, 16 or 32) per run length. Writes `count` levels to `levels`.
/// False, with `error`, for widths outside those, a block whose sizes disagree, or runs that do not cover
/// exactly `count` levels.
bool decode_levels(std::span<const std::uint8_t> block, std::uint32_t value_bits, std::uint32_t length_bits,
                   std::uint32_t count, std::uint16_t* levels, std::string& error);

}  // namespace nano_lance::rle
