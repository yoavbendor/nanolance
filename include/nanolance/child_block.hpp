// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/page_layout.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// One fixed-width block of values as a compressive encoding stores it, for the encodings that nest
/// inside another: a sparse layout's u64 position and count buffers, and the values and run lengths of
/// a run-length-encoded chunk. Covers Flat, InlineBitpacking, Bitpacked over Flat, and General over any
/// of those. Everything else is refused by name.
///
/// Lance sizes a flat or general block from its bytes, but a bitpacked block only from the count the
/// reader already knows (`needs_count`). A caller that has no count for such a block refuses.
namespace nano_lance::child_block {

/// Whether `encoding` can only be decoded when the number of values is known.
bool needs_count(const page_layout::Compressive& encoding);

/// The uncompressed width in bits of the values `encoding` yields: the Flat width, or the uncompressed
/// width of a bitpacking. 0 when it is not one of the supported encodings.
std::uint32_t value_bits(const page_layout::Compressive& encoding);

/// Decodes one block of `value_bits`-wide values (8, 16, 32 or 64) into raw little-endian bytes in
/// `out`. `count` is the number of values, or nullopt when the encoding sizes itself (see needs_count).
bool decode(const page_layout::Compressive& encoding, const std::vector<std::uint8_t>& bytes,
            std::optional<std::uint64_t> count, std::uint32_t value_bits, std::vector<std::uint8_t>& out,
            std::string& error);

/// One RLE chunk: `values` holds one value per run, `lengths` one run length per run, each encoded with
/// its own compressive encoding. At most one of the two may need the run count, which the other one
/// gives. The runs are appended to `out` as raw values of `value_bits` each, every run repeated by its
/// length. `length_bits` is the width of a run length (8, 16 or 32).
///
/// `values_scheme` is the buffer compression a General wrapper put around the whole RLE encoding: it
/// frames the values buffer (one frame per chunk), and the lengths are never framed. kNone: `values` is
/// the values as they are.
bool decode_rle_chunk(const page_layout::Compressive& values_encoding, const page_layout::Compressive& lengths_encoding,
                      const std::vector<std::uint8_t>& values, const std::vector<std::uint8_t>& lengths,
                      page_layout::BufferScheme values_scheme, std::uint32_t value_bits, std::uint32_t length_bits,
                      std::vector<std::uint8_t>& out, std::string& error);

}  // namespace nano_lance::child_block
