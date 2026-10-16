// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// An INVERTED index's posting lists (internal; docs/FTS_INDEX.md): blocks of 128 or 256 (doc id,
// frequency) pairs. A 128 block starts with a little-endian f32 (its best score); a 256 block has
// none. A full block is BitPacker4x (128) or BitPacker8x (256) packed doc id deltas, then the
// frequencies (plain bit-packed at 128, patched FOR at 256); the last block is the tail codec's
// varints (or plain u32s for fixed32_v1 / format 1).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nano_lance::fts {

constexpr std::size_t kPostingBlock = 128;      // BitPacker4x blocks; also the positions' pack unit
constexpr std::size_t kLargePostingBlock = 256;  // BitPacker8x blocks (format 3, block_size=256)

/// Bits needed for `v` (0 for 0).
inline std::uint8_t bit_width(std::uint32_t v) {
    std::uint8_t n = 0;
    while (v != 0U) {
        ++n;
        v >>= 1U;
    }
    return n;
}

/// lance-bitpacking's BitPacker4x / BitPacker8x layout: 32 * lanes values in `lanes` interleaved
/// lanes (value i in lane i % lanes), each lane packed LSB first into 32-bit words, word w of lane j
/// stored as u32 lanes * w + j. `bits` * 4 * lanes bytes.
void bitpack_pack(const std::uint32_t* values, std::size_t lanes, std::uint8_t bits, std::uint8_t* out);
void bitpack_unpack(const std::uint8_t* in, std::size_t lanes, std::uint8_t bits, std::uint32_t* values);
inline void bitpack4x_pack(const std::uint32_t* values, std::uint8_t bits, std::uint8_t* out) {
    bitpack_pack(values, 4, bits, out);
}
inline void bitpack4x_unpack(const std::uint8_t* in, std::uint8_t bits, std::uint32_t* values) {
    bitpack_unpack(in, 4, bits, values);
}

/// Patched FOR of one 256-value block (Lance's compress_pfor_block_with): [u8 width][u8 exception
/// count][BitPacker8x body][(u8 index, varint high bits) per exception]. Writes the width that
/// Lance picks, so the bytes match.
void encode_pfor256(const std::uint32_t* values, std::vector<std::uint8_t>& out);

enum class TailCodec { VarintDelta, Fixed32 };

struct PostingBlockView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

/// The (doc, frequency) pairs of a posting list of `length` entries in blocks of `block` (128 or
/// 256), appended.
bool decode_posting(const std::vector<PostingBlockView>& blocks, std::uint32_t length, TailCodec codec,
                    std::size_t block, std::vector<std::uint32_t>& docs, std::vector<std::uint32_t>& freqs,
                    std::string& error);

/// One block of `n` (<= `block`) pairs, prefixed by `score` at block 128 only: packed when full,
/// otherwise the tail codec's remainder (varints, or plain u32s for fixed32_v1 / format 1).
void encode_posting_block(const std::uint32_t* docs, const std::uint32_t* freqs, std::size_t n, float score,
                          std::vector<std::uint8_t>& out, TailCodec tail = TailCodec::VarintDelta,
                          std::size_t block = kPostingBlock);

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v);

}  // namespace nano_lance::fts
