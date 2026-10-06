// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// An INVERTED index's posting lists (internal; docs/FTS_INDEX.md): blocks of 128 (doc id,
// frequency) pairs, each block a little-endian f32 (the block's best score) followed by either
// BitPacker4x-packed doc id deltas and frequencies (a full block) or varints (the last one).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nano_lance::fts {

constexpr std::size_t kPostingBlock = 128;

/// Bits needed for `v` (0 for 0).
inline std::uint8_t bit_width(std::uint32_t v) {
    std::uint8_t n = 0;
    while (v != 0U) {
        ++n;
        v >>= 1U;
    }
    return n;
}

/// lance-bitpacking's BitPacker4x layout: 128 values in 4 interleaved lanes (value i in lane i % 4),
/// each lane packed LSB first into 32-bit words, word w of lane j stored as u32 4w + j. `bits` * 16
/// bytes.
void bitpack4x_pack(const std::uint32_t* values, std::uint8_t bits, std::uint8_t* out);
void bitpack4x_unpack(const std::uint8_t* in, std::uint8_t bits, std::uint32_t* values);

enum class TailCodec { VarintDelta, Fixed32 };

struct PostingBlockView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

/// The (doc, frequency) pairs of a posting list of `length` entries, appended.
bool decode_posting(const std::vector<PostingBlockView>& blocks, std::uint32_t length, TailCodec codec,
                    std::vector<std::uint32_t>& docs, std::vector<std::uint32_t>& freqs, std::string& error);

/// One block of `n` (<= 128) pairs, prefixed by `score`: packed when full, varints otherwise.
void encode_posting_block(const std::uint32_t* docs, const std::uint32_t* freqs, std::size_t n, float score,
                          std::vector<std::uint8_t>& out);

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v);

}  // namespace nano_lance::fts
