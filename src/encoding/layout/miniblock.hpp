// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

/// The mini-block page layout's framing, both directions (Lance's `encodings/logical/primitive.rs`,
/// `MiniBlockLayout` in `encodings_v2_1.proto`). A mini-block page is two buffers:
///
/// - the **chunks**, back to back, each padded to 8 bytes:
///
///       [u16 num_levels]
///       [u16 rep_size]                     only when the layout has rep_compression
///       [u16 def_size]                     only when the layout has def_compression
///       [num_buffers x (u32 if has_large_chunk else u16)]   each value buffer's size
///       pad to 8
///       [rep] pad8  [def] pad8  [buffer_0] pad8  [buffer_1] pad8 ...
///
///   Sizes are the unpadded lengths; padding is Lance's fill byte 0xFE.
/// - the **metadata** (control) buffer, one word per chunk (u32 if has_large_chunk else u16):
///   `(chunk_bytes / 8 - 1) << 4 | log2(values)`. Every chunk but the last holds a power of two values; the
///   last chunk's word has log2 0 and its count is what is left of the page's item count.
///
/// What the buffers inside a chunk hold (bitpacked values, levels, RLE runs) is the value encodings' business;
/// which shape a page has is read from its descriptor by the page decoder.
namespace nano_lance::miniblock {

constexpr std::size_t kAlignment = 8U;
constexpr std::uint8_t kFillByte = 0xFEU;

constexpr std::size_t align(std::size_t bytes) { return (bytes + kAlignment - 1U) & ~(kAlignment - 1U); }

/// Which header slots a page's chunks have, from its MiniBlockLayout. The defaults are the shape of a page
/// with no descriptor at all (only ever an old nanolance file): no levels, two u16 buffer sizes.
struct ChunkShape {
    bool has_repetition = false;
    bool has_definition = false;
    std::uint32_t num_buffers = 2;
    bool large_buffer_sizes = false;

    std::size_t buffer_size_bytes() const { return large_buffer_sizes ? 4U : 2U; }
    /// Bytes the header declares, before its padding.
    std::size_t declared_bytes() const {
        return 2U + (has_repetition ? 2U : 0U) + (has_definition ? 2U : 0U) +
               static_cast<std::size_t>(num_buffers) * buffer_size_bytes();
    }
};

// ---- Writing (nanolance writes has_large_chunk pages, no repetition: u32 sizes, levels optional) ----

/// The bytes of one chunk over `buffers`, appended to `out`: definition levels `def` (empty: none, and then
/// the header has no def_size slot and num_levels is 0), covering `num_levels` values, then each buffer.
void append_chunk(std::vector<std::uint8_t>& out, std::size_t num_levels, std::span<const std::uint8_t> def,
                  std::span<const std::span<const std::uint8_t>> buffers);

/// The same, for one value buffer.
void append_chunk(std::vector<std::uint8_t>& out, std::size_t num_levels, std::span<const std::uint8_t> def,
                  std::span<const std::uint8_t> values);

/// Just the padded header of such a chunk, for a writer that streams the buffers itself (each padded with
/// `pad_bytes` of kFillByte).
std::vector<std::uint8_t> chunk_header(std::size_t num_levels, std::size_t def_bytes,
                                       std::span<const std::size_t> buffer_sizes);

/// Bytes such a chunk occupies, padding included.
std::size_t chunk_bytes(std::size_t def_bytes, std::span<const std::size_t> buffer_sizes);

/// Padding after `written` bytes.
constexpr std::size_t pad_bytes(std::size_t written) { return align(written) - written; }

/// The metadata word for a chunk of `chunk_bytes` (a multiple of 8, at least 8) holding `values`; `last`
/// gives log2 0 whatever `values` is. A non-last chunk's `values` must be a power of two from 2 to 2^15.
std::uint32_t control_word(std::size_t chunk_bytes, std::size_t values, bool last);

/// One chunk, as a metadata buffer sees it.
struct ChunkMeta {
    std::size_t bytes = 0;
    std::size_t values = 0;
};

/// The metadata buffer (u32 words) for `chunks`, in order; the last one's word has log2 0.
std::vector<std::uint8_t> control_buffer(std::span<const ChunkMeta> chunks);

// ---- Reading (any shape; the input is untrusted) ----

struct ControlWord {
    std::uint64_t chunk_bytes = 0;
    std::uint32_t log_values = 0;
};

constexpr ControlWord decode_control_word(std::uint32_t word) {
    return {(static_cast<std::uint64_t>(word >> 4U) + 1U) * 8U, word & 0x0FU};
}

/// Value counts per chunk from a metadata buffer of `large_words` (u32) or u16 words: 2^log for every chunk
/// but the last, which holds what is left of `num_items`. False, with `error`, for a buffer that is not
/// whole words, a non-last chunk declaring log2 0, or counts that pass or do not reach `num_items`.
bool decode_chunk_items(std::span<const std::uint8_t> control, bool large_words, std::uint64_t num_items,
                        std::vector<std::uint64_t>& out, std::string& error);

struct ChunkHeader {
    std::uint16_t num_levels = 0;
    std::uint32_t rep_size = 0;
    std::uint32_t def_size = 0;
    std::vector<std::uint32_t> buffer_sizes;
    /// Bytes the whole chunk occupies, padding included: where the next chunk starts.
    std::size_t chunk_bytes = 0;
};

/// Reads the header of the chunk at `offset` of `payload`. False, with `error`, when the shape declares more
/// than 64 buffers, the header is truncated, or the sizes it declares pass the end of `payload`.
bool read_chunk_header(std::span<const std::uint8_t> payload, std::size_t offset, const ChunkShape& shape,
                       ChunkHeader& out, std::string& error);

/// One chunk's buffers, copied out of the page.
struct ChunkView {
    /// The first value buffer, the only one most encodings have; `extra_buffers` holds the rest (an RLE
    /// value block, for instance, is a values buffer plus a run-lengths buffer).
    std::vector<std::uint8_t> values;
    std::vector<std::vector<std::uint8_t>> extra_buffers;
    /// The definition levels, when the chunk has any, and the header's level count for them.
    std::vector<std::uint8_t> repdef;
    std::uint32_t repdef_values = 0;
    /// List pages only: the repetition levels, the header's level count, and the chunk's value count from
    /// its metadata word (set by the caller). Levels outnumber values there (an empty or null list takes a
    /// level and no value), so neither count can stand in for the other.
    std::vector<std::uint8_t> rep;
    std::uint32_t num_levels = 0;
    std::uint64_t items = 0;
};

/// Splits a page's chunk buffer into its chunks. `out`'s chunks are refilled in place, keeping their
/// buffers' capacity, and `out` is resized to the chunk count. False, with `error`, at the first chunk that
/// read_chunk_header refuses.
bool split_payload(std::span<const std::uint8_t> payload, const ChunkShape& shape, std::vector<ChunkView>& out,
                   std::string& error);

}  // namespace nano_lance::miniblock
