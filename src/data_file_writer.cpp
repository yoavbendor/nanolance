// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/data_file_writer.hpp"

#include "lance_minimal.pb.hpp"
#include "nanolance/blob_v2_external.hpp"
#include "nanolance/bool_bitpack.hpp"
#include "nanolance/byte_stream_split.hpp"
#include "nanolance/fastlanes_bitpack.hpp"
#include "nanolance/fsst.hpp"
#include "nanolance/repdef.hpp"
#include "nanolance/schema_mapper.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nano_lance {
namespace {

void write_le16(std::ostream& out, std::uint16_t value) {
    const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU)};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_le32(std::ostream& out, std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xFFU),
        static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU),
        static_cast<char>((value >> 24U) & 0xFFU),
    };
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_le64(std::ostream& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        const auto byte = static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU);
        out.write(&byte, 1);
    }
}

std::uint64_t pos(std::ostream& out) {
    return static_cast<std::uint64_t>(out.tellp());
}

void align64(std::ostream& out) {
    const auto current = pos(out);
    const auto padding = (64U - (current % 64U)) % 64U;
    static constexpr std::array<char, 64> zeros{};
    out.write(zeros.data(), static_cast<std::streamsize>(padding));
}

std::uint32_t bits_per_value(const LanceField& field) {
    // bool is the one type whose on-disk width is not a whole number of bytes: 1 bit per value,
    // LSB-first, matching stock Lance. Everything else derives from the single shared width table --
    // this used to be a second copy of it that fell through to 64 bits for anything it did not
    // recognize, which silently gave every temporal type 8 bytes and every decimal 8 instead of
    // 16/32, and surfaced as "column value buffer size is not aligned to field width".
    if (field.logical_type == "bool") {
        return 1;
    }
    return static_cast<std::uint32_t>(lance_logical_type_value_bytes(field.logical_type) * 8U);
}

std::size_t value_width_bytes(const LanceField& field) {
    const auto bits = bits_per_value(field);
    if (bits < 8U) {
        return 1U;
    }
    return bits / 8U;
}

constexpr std::uint32_t kMaxEightByteWordsPerMetadata = 4095U;
// Flat (uncompressed) fixed-width chunks are capped at the same 32 KB miniblock max as the
// variable-width path. A previous 800-byte cap turned a 1.6 MB float column into ~2000 tiny chunks,
// and since one chunk == one page here, that meant ~2000 pages each paying full per-page allocation,
// protobuf, and stream-position overhead (dominant in profiles for float/int struct columns). The
// miniblock control word is 12-bit (4095 eight-byte words => 32760 bytes) and the per-chunk size is a
// u16, so 32760 is the largest chunk that both structures can describe.
constexpr std::uint32_t kMaxVariableMiniblockBytes = kMaxEightByteWordsPerMetadata * 8U;
/// The most definition-level bytes a flat nullable chunk carries: 1024 levels bit-packed at width
/// 1 (128 bytes), or a short tail of at most 64 raw u16 levels (128 bytes).
constexpr std::size_t kMaxChunkLevelBytesFlat = 128U;
constexpr std::uint32_t kMaxUncompressedMiniblockBytes = kMaxVariableMiniblockBytes;
// Bool is bit-packed (1 bit/value) at the on-disk boundary; a chunk's packed payload must stay within
// the same 32760-byte miniblock cap as every other chunk kind, so it can hold 8x as many values.
constexpr std::size_t kMaxBoolValuesPerChunk = static_cast<std::size_t>(kMaxUncompressedMiniblockBytes) * 8U;

struct MiniblockChunk {
    std::vector<std::uint8_t> bytes;
    std::size_t value_count = 0;
    /// FastLanes-packed definition levels for this chunk, when the column has nulls. Empty otherwise.
    std::vector<std::uint8_t> repdef;
};

void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void write_length_delimited(std::vector<std::uint8_t>& out, std::uint32_t field_number, const std::vector<std::uint8_t>& payload) {
    out.push_back(static_cast<std::uint8_t>((field_number << 3U) | 2U));
    append_varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

void write_string_field(std::vector<std::uint8_t>& out, std::uint32_t field_number, std::string_view value) {
    write_length_delimited(out, field_number, std::vector<std::uint8_t>(value.begin(), value.end()));
}

/// Declared bits per value for a flat page. This is what tells every reader how wide each value is,
/// so it must be the REAL width, not a nearby one.
///
/// It used to snap anything it did not recognize to 32, which silently mis-declared two types the
/// library already claimed to support: a 16-bit column was written as 32 bits (stock Lance panicked
/// with "range end index 12 out of range for slice of length 6"), and fixed_size_binary(N) for any N
/// but 4 likewise -- including the 6-byte MAC address README.md recommends the type for. With the
/// default structural encodings an integer column escapes through InlineBitpacking, which declares
/// its own width, so int16 happened to survive; fixed_size_binary is not bitpackable and did not.
std::uint32_t flat_bits_per_value(const LanceField& field) {
    return bits_per_value(field);
}

/// MiniBlockLayout for a flat page:
///   f3 value_compression = CompressiveEncoding{ f1 Flat{ f1 bits_per_value } }
///   f6 layers = 1, f7 num_buffers = 1, f9 num_items, f10 has_large_chunk = 1
///
/// Lengths are computed rather than hardcoded: `bits_per_value` is a varint, so a width of 128 or
/// 256 (decimal128 / decimal256) is two bytes, and the nested message lengths in front of it shift
/// accordingly. The previous hand-written byte string baked in a one-byte width.
std::vector<std::uint8_t> repdef_encoding_bytes();

/// The MiniBlockLayout tail shared by every shape: f6 layers, f7 num_buffers, f9 num_items,
/// f10 has_large_chunk. `nullable` switches layers from [1] to [3], which is how Lance announces a
/// definition-level layer.
std::vector<std::uint8_t> mini_block_tail(std::uint64_t num_items, std::uint8_t num_buffers, bool nullable) {
    std::vector<std::uint8_t> tail;
    tail.push_back(0x32U);                           // f6 layers
    tail.push_back(0x01U);
    tail.push_back(nullable ? 0x03U : 0x01U);
    tail.push_back(0x38U);                           // f7 num_buffers
    tail.push_back(num_buffers);
    tail.push_back(0x48U);                           // f9 num_items
    append_varint(tail, num_items);
    tail.push_back(0x50U);                           // f10 has_large_chunk
    tail.push_back(0x01U);
    return tail;
}

std::vector<std::uint8_t> build_mini_block_layout(std::uint32_t bits_per_value_token, std::uint64_t num_items,
                                                  bool nullable = false, std::uint64_t fsl_items = 0) {
    // A fixed_size_list row is `fsl_items` flat values back to back. Lance describes that as
    // CompressiveEncoding{ f11 FixedSizeList{ f1 items_per_value, f2 CompressiveEncoding{ f1 Flat } } },
    // with the ELEMENT's width in the Flat -- the wrapper is what tells a reader it is a list at all.
    const std::uint64_t flat_bits = fsl_items != 0U ? bits_per_value_token / fsl_items : bits_per_value_token;
    std::vector<std::uint8_t> flat;                  // Flat{ f1 bits_per_value }
    flat.push_back(0x08U);
    append_varint(flat, flat_bits);

    std::vector<std::uint8_t> compressive;           // CompressiveEncoding{ f1 Flat }
    write_length_delimited(compressive, 1, flat);
    if (fsl_items != 0U) {
        std::vector<std::uint8_t> fixed_size_list;   // FixedSizeList{ f1 items, f2 values }
        fixed_size_list.push_back(0x08U);
        append_varint(fixed_size_list, fsl_items);
        write_length_delimited(fixed_size_list, 2, compressive);
        compressive.clear();
        write_length_delimited(compressive, 11, fixed_size_list);
    }

    std::vector<std::uint8_t> mini;
    if (nullable) {
        // f2 (how the definition levels are stored) precedes f3, matching what pylance emits.
        write_length_delimited(mini, 2, repdef_encoding_bytes());
    }
    write_length_delimited(mini, 3, compressive);    // f3 value_compression
    const auto tail = mini_block_tail(num_items, 1U, nullable);
    mini.insert(mini.end(), tail.begin(), tail.end());
    return mini;
}

/// page_layout_bytes for a flat page, with the nullable flag threaded through. `fsl_items` non-zero
/// wraps the values as a fixed_size_list of that many elements per row.
std::vector<std::uint8_t> page_layout_bytes_flat(std::uint32_t bits_token, std::uint64_t rows, bool nullable,
                                                 std::uint64_t fsl_items = 0) {
    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 1, build_mini_block_layout(bits_token, rows, nullable, fsl_items));
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

void append_le32(std::vector<std::uint8_t>& out, std::uint32_t value);  // defined with the other buffer writers

void append_le16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

std::size_t max_values_per_metadata_chunk(std::size_t bytes_per_value) {
    const auto max_bytes = static_cast<std::size_t>(kMaxEightByteWordsPerMetadata) * 8U;
    return max_bytes / bytes_per_value;
}

std::size_t max_values_per_uncompressed_chunk(std::size_t bytes_per_value) {
    if (bytes_per_value == 0U) {
        return 1U;
    }
    const auto by_bytes = kMaxUncompressedMiniblockBytes / bytes_per_value;
    const auto by_metadata = max_values_per_metadata_chunk(bytes_per_value);
    return std::max<std::size_t>(1U, std::min(by_bytes, by_metadata));
}

// Chunk-meta (control) buffer for a multi-chunk miniblock page with one value buffer per chunk (the
// structural-dictionary index chunks). Each word is
// (wrapped_bytes/8 - 1) << 4 | log2(num_values), where wrapped_bytes is the chunk's full footprint in
// the value buffer as written by miniblock_payload (8-byte chunk header + buffer, padded to 8). Lance
// requires every non-final chunk to carry a nonzero log2 (num_values = 1 << log2, so full chunks must
// be a power of two) and derives the final chunk's value count from the page's total item count.
// The single-chunk control_buffer_for() below cannot serve here: it always writes log2=0, which is
// correct only because the one chunk it describes is by definition the final one. A multi-chunk page
// (a >1024-row dictionary column) needs this exact layout to be readable by stock Lance.
//
// The words are u32, matching has_large_chunk=1 in the page layout. They used to be u16 with
// has_large_chunk=0, and stock Lance rejects that outright: v2_2's validate_page_layout refuses ANY
// miniblock page without the u32 chunk grammar, before it looks at a single byte. Because that check
// runs over the whole file's page table, one dictionary-encoded column made every OTHER column in the
// same dataset unreadable by Lance too. The word's own layout is identical either way -- only the
// width changes -- so nothing else about the page moved.
std::vector<std::uint8_t> control_buffer_for_index_chunks(const std::vector<MiniblockChunk>& chunks) {
    std::vector<std::uint8_t> out;
    out.reserve(chunks.size() * 4U);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const std::size_t wrapped = ((8U + chunks[i].bytes.size()) + 7U) / 8U * 8U;
        const auto divided_minus_one = static_cast<std::uint32_t>(wrapped / 8U - 1U);
        std::uint32_t log_num_values = 0U;
        if (i + 1U < chunks.size()) {
            for (std::size_t v = chunks[i].value_count; v > 1U; v >>= 1U) {
                ++log_num_values;
            }
        }
        append_le32(out, (divided_minus_one << 4U) | (log_num_values & 0x0FU));
    }
    return out;
}

// A chunk's 8-byte header is four little-endian u16 slots: the number of values the
// repetition/definition layer covers (0 when there is none), then up to three buffer sizes, with
// 0xFEFE marking an unused slot. Without levels the chunk holds values only; with them the level
// buffer comes FIRST and the values follow it.
void append_miniblock_chunk(std::vector<std::uint8_t>& out, const MiniblockChunk& chunk) {
    if (chunk.repdef.empty()) {
        append_le16(out, 0U);                                                  // no repdef layer
        append_le16(out, static_cast<std::uint16_t>(chunk.bytes.size()));      // values
        append_le16(out, 0U);
        append_le16(out, 0xFEFEU);                                             // unused
        out.insert(out.end(), chunk.bytes.begin(), chunk.bytes.end());
    } else {
        append_le16(out, static_cast<std::uint16_t>(chunk.value_count));       // values the levels cover
        append_le16(out, static_cast<std::uint16_t>(chunk.repdef.size()));     // definition levels
        append_le16(out, static_cast<std::uint16_t>(chunk.bytes.size()));      // values
        append_le16(out, 0U);
        out.insert(out.end(), chunk.repdef.begin(), chunk.repdef.end());
        // Every buffer is padded to 8 bytes after it is written, the header recording the UNPADDED
        // length. Invisible while a level buffer was always a 128-byte packed block; a short chunk's
        // raw levels can be any even size, and stock Lance then read the values from the wrong offset
        // ("Inline bitpacking width 67108864 exceeds 64-bit values").
        while (out.size() % 8U != 0U) {
            out.push_back(0U);
        }
        out.insert(out.end(), chunk.bytes.begin(), chunk.bytes.end());
    }
    while (out.size() % 8U != 0U) {
        out.push_back(0U);
    }
}

/// Pack `count` definition levels for the rows starting at `first_row` of `validity` into one
/// FastLanes block at 1 bit per level.
///
/// Lance stores a level per value where **1 means NULL** -- the inverse of Arrow's validity bit --
/// and packs them with the same FastLanes kernel integer columns use. Rows past `count` in the block
/// are padded with 0 (valid); the chunk header states the real count, so they are never read back.
std::vector<std::uint8_t> pack_definition_levels(const std::vector<std::uint8_t>& validity,
                                                 std::uint64_t first_row, std::size_t count) {
    // One FastLanes block is exactly 1024 values, so a chunk carrying levels must not exceed it.
    // Callers cap their chunk size (see kMaxValuesPerNullableChunk); this is the backstop, because
    // getting it wrong overruns `levels` below -- which it did, as a stack smash, when the flat
    // path's 4095-value chunks were first given definition levels.
    if (count > 1024U) {
        std::abort();
    }
    std::uint16_t levels[1024] = {0};
    for (std::size_t i = 0; i < count; ++i) {
        const auto row = first_row + i;
        const bool valid = (validity[static_cast<std::size_t>(row >> 3U)] >> (row & 7U)) & 1U;
        levels[i] = valid ? 0U : 1U;
    }
    // A short final chunk has two legal spellings, and the choice is not ours: Lance's decoder infers
    // which one it is FROM THE BUFFER LENGTH, so writing the wrong one is silently misread rather
    // than rejected. Its rule is to pad up to a full block only when padding costs fewer bits than
    // packing saves; otherwise the levels go in raw, as plain u16 words. At width 1 that means a tail
    // of 64 or fewer values is raw -- and 64 is exactly the packed size, so a padded 64-value chunk
    // is indistinguishable from a raw one and would come back as 64 arbitrary levels.
    const auto packed_words = nano_lance::fastlanes::packed_words_1024<std::uint16_t>(1);
    const std::size_t padding_cost = 1U * (1024U - count);
    const std::size_t pack_savings = (16U - 1U) * count;
    if (count < 1024U && padding_cost >= pack_savings) {
        std::vector<std::uint8_t> out(count * sizeof(std::uint16_t));
        std::memcpy(out.data(), levels, out.size());
        return out;
    }
    std::vector<std::uint16_t> packed(packed_words);
    nano_lance::fastlanes::pack_1024<std::uint16_t>(1, levels, packed.data());
    std::vector<std::uint8_t> out(packed_words * sizeof(std::uint16_t));
    std::memcpy(out.data(), packed.data(), out.size());
    return out;
}

/// PageLayout for a chunk that carries definition levels: the same MiniBlockLayout as the
/// no-nulls case, plus f2 (how the levels are encoded) and f6 layers = [3] instead of [1].
///
/// f2's shape is CompressiveEncoding field 4 -- a bit-width wrapper whose f1 is the level's
/// uncompressed width (16) and whose f3 says how the levels are actually stored (Flat(1), since the
/// only levels here are 0 and 1). Matches pylance 12.0.0 byte for byte.
std::vector<std::uint8_t> repdef_encoding_bytes() {
    std::vector<std::uint8_t> flat{0x08, 0x01};                 // Flat{ f1 bits_per_value = 1 }
    std::vector<std::uint8_t> flat_ce;
    write_length_delimited(flat_ce, 1, flat);                   // CompressiveEncoding{ f1 Flat }
    std::vector<std::uint8_t> wrapper{0x08, 0x10};              // f1 uncompressed_bits_per_value = 16
    write_length_delimited(wrapper, 3, flat_ce);                // f3 values
    std::vector<std::uint8_t> out;
    write_length_delimited(out, 4, wrapper);                    // CompressiveEncoding{ f4 }
    return out;
}

std::vector<std::uint8_t> miniblock_payload(const std::vector<MiniblockChunk>& chunks) {
    std::vector<std::uint8_t> out;
    for (const auto& chunk : chunks) {
        append_miniblock_chunk(out, chunk);
    }
    return out;
}

// Single-chunk fast paths: the variable-width paths emit one chunk per page, so the vector-based
// miniblock_payload() above would otherwise force callers to wrap each chunk in a temporary
// one-element vector (an extra heap allocation and copy of the whole chunk on every page). These
// avoid that entirely. The chunk they describe is always the page's final one, which is why the
// control word's log2 nibble is unconditionally 0 -- see miniblock_control_word.
std::uint16_t miniblock_control_word(std::size_t repdef_bytes, std::size_t value_bytes);

std::vector<std::uint8_t> control_buffer_for(const MiniblockChunk& chunk) {
    std::vector<std::uint8_t> out;
    append_le16(out, miniblock_control_word(chunk.repdef.size(), chunk.bytes.size()));
    out.push_back(0U);
    out.push_back(0U);
    return out;
}

std::vector<std::uint8_t> miniblock_payload(const MiniblockChunk& chunk) {
    std::vector<std::uint8_t> out;
    out.reserve(chunk.bytes.size() + 16U);
    append_miniblock_chunk(out, chunk);
    return out;
}

// Stream one flat (uncompressed) miniblock chunk straight to `out`: the 8-byte miniblock chunk header,
// then `chunk_bytes` value bytes copied directly from the column's value buffer, then 8-byte padding.
// This is byte-identical to miniblock_payload() over a MiniblockChunk holding the same slice, but it
// avoids materializing the slice into a MiniblockChunk and then again into a payload vector — for a
// plain fixed-width column the chunk bytes are just a view into values.fixed. Returns bytes written.
/// Same as stream_flat_miniblock_payload but for a chunk that carries definition levels: the header's
/// four slots become [values covered][level bytes][value bytes][0], and the level buffer precedes the
/// values.

/// The per-chunk control word for a single-chunk page.
///
/// It encodes the chunk's FULL footprint -- its 8-byte header plus every buffer, padded to 8 -- as
/// ((footprint / 8) - 1) << 4, with the low nibble holding log2(value count) for a non-final chunk
/// and 0 for the last one. The pages built with it hold one chunk, so that chunk is always final and
/// the nibble is always 0 (multi-chunk pages go through MiniblockPageWriter).
///
/// The previous form derived the word from the VALUE bytes alone, which agreed with this only
/// because the 8-byte header and the -1 cancel when the values are a multiple of 8. Adding a
/// definition-level buffer broke that coincidence and stock Lance panicked with "the offset + length
/// of the sliced Buffer cannot exceed the existing length".
std::uint16_t miniblock_control_word(std::size_t repdef_bytes, std::size_t value_bytes) {
    // Each buffer is padded to 8 bytes in the chunk, so the level buffer's padding counts toward the
    // footprint too -- see append_miniblock_chunk.
    const auto padded_repdef = (repdef_bytes + 7U) & ~static_cast<std::size_t>(7U);
    const auto footprint = ((8U + padded_repdef + value_bytes) + 7U) / 8U;
    return static_cast<std::uint16_t>((footprint - 1U) << 4U);
}

std::uint64_t stream_miniblock_payload_with_repdef(std::ostream& out, const std::vector<std::uint8_t>& repdef,
                                                   std::size_t value_count, const std::uint8_t* data,
                                                   std::size_t chunk_bytes) {
    std::vector<std::uint8_t> header;
    append_le16(header, static_cast<std::uint16_t>(value_count));
    append_le16(header, static_cast<std::uint16_t>(repdef.size()));
    append_le16(header, static_cast<std::uint16_t>(chunk_bytes));
    append_le16(header, 0U);
    static constexpr std::array<char, 8> zeros{};
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    out.write(reinterpret_cast<const char*>(repdef.data()), static_cast<std::streamsize>(repdef.size()));
    // The level buffer is padded to 8 bytes before the values start (see append_miniblock_chunk).
    const auto repdef_pad = (8U - (repdef.size() % 8U)) % 8U;
    if (repdef_pad != 0U) {
        out.write(zeros.data(), static_cast<std::streamsize>(repdef_pad));
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(chunk_bytes));
    std::uint64_t written = 8U + repdef.size() + repdef_pad + chunk_bytes;
    const auto pad = (8U - (written % 8U)) % 8U;
    if (pad != 0U) {
        out.write(zeros.data(), static_cast<std::streamsize>(pad));
    }
    return written + pad;
}

std::uint64_t stream_flat_miniblock_payload(std::ostream& out, const std::uint8_t* data,
                                            std::size_t chunk_bytes) {
    const std::array<char, 8> header{0, 0, static_cast<char>(chunk_bytes & 0xFFU),
                                     static_cast<char>((chunk_bytes >> 8U) & 0xFFU), 0, 0,
                                     static_cast<char>(0xFE), static_cast<char>(0xFE)};
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(chunk_bytes));
    std::uint64_t written = 8U + chunk_bytes;
    const auto pad = (8U - (written % 8U)) % 8U;
    static constexpr std::array<char, 8> zeros{};
    if (pad != 0U) {
        out.write(zeros.data(), static_cast<std::streamsize>(pad));
    }
    return written + pad;
}

/// Pages of a flat fixed-width column hold this much chunk payload before a new page starts.
///
/// These columns used to be written one chunk per page, so a 2M-row int64 column became ~2000
/// pages of ~2.5 KB, and Rust Lance, which pays a scheduling and decode set-up cost per page, read
/// those files ~5x slower than its own. Measured over 64 KiB - 8 MiB on the tools/bench_matrix.py
/// data: Rust's reads level off from ~512 KiB; nanolance's own reader is fastest at 64 - 256 KiB
/// and slows past that, because a page is read into one buffer before its chunks are copied out
/// (1 MiB pages cost it 10-30% on flat columns, 8 MiB pages 4x). 512 KiB is within noise of the
/// best for Rust and within ~10% of the best for nanolance.
constexpr std::uint64_t kTargetMiniblockPageBytes = 512U << 10U;

/// The most values a non-final chunk of a multi-chunk page may hold: its control word keeps
/// log2(values) in four bits.
constexpr std::size_t kMaxValuesPerMultiChunk = std::size_t{1} << 15U;

/// The largest power of two <= `n` that a non-final chunk can carry (0 when n < 2: a chunk of one
/// value has log2 = 0, which only the final chunk of a page may have).
std::size_t multichunk_values(std::size_t n) {
    n = std::min(n, kMaxValuesPerMultiChunk);
    if (n < 2U) {
        return 0U;
    }
    std::size_t p = 1U;
    while (p * 2U <= n) {
        p *= 2U;
    }
    return p;
}

/// A MiniBlock page of one or more chunks, streamed as the chunks are built.
///
/// The caller streams each chunk (stream_flat_miniblock_payload / stream_miniblock_payload_with_repdef,
/// which pad it to 8 bytes, so consecutive chunks are contiguous) and reports its footprint and value
/// count here. finish() then writes the control buffer: one u32 word per chunk,
/// (footprint/8 - 1) << 4 | log2(values), with the final chunk's nibble 0 -- Lance derives the last
/// chunk's count from the page's item count, and every other chunk must hold a power of two.
class MiniblockPageWriter {
public:
    explicit MiniblockPageWriter(std::ostream& out) : out_(out) {}

    [[nodiscard]] bool empty() const { return chunks_ == 0U; }
    [[nodiscard]] std::uint64_t rows() const { return rows_; }
    [[nodiscard]] std::uint64_t payload_bytes() const { return payload_size_; }

    /// Call before streaming a chunk.
    void begin_chunk() {
        if (chunks_ == 0U) {
            align64(out_);
            payload_offset_ = pos(out_);
        }
    }

    void end_chunk(std::uint64_t footprint, std::size_t values) {
        std::uint32_t log_values = 0U;
        for (std::size_t v = values; v > 1U; v >>= 1U) {
            ++log_values;
        }
        append_le32(control_, static_cast<std::uint32_t>((footprint / 8U - 1U) << 4U) | (log_values & 0x0FU));
        payload_size_ += footprint;
        rows_ += values;
        ++chunks_;
    }

    /// Write the control buffer and fill `page`'s buffers and length; resets for the next page.
    void finish(pb::ColumnPage& page) {
        control_[control_.size() - 4U] &= 0xF0U;  // the final chunk: log2 nibble 0
        align64(out_);
        const auto control_offset = pos(out_);
        out_.write(reinterpret_cast<const char*>(control_.data()), static_cast<std::streamsize>(control_.size()));
        page.buffer_offsets = {control_offset, payload_offset_};
        page.buffer_sizes = {control_.size(), payload_size_};
        page.length = rows_;
        page.priority = 0;
        control_.clear();
        payload_size_ = 0;
        rows_ = 0;
        chunks_ = 0;
    }

private:
    std::ostream& out_;
    std::vector<std::uint8_t> control_;
    std::uint64_t payload_offset_ = 0;
    std::uint64_t payload_size_ = 0;
    std::uint64_t rows_ = 0;
    std::size_t chunks_ = 0;
};

std::vector<std::uint8_t> bytes_from_hex(std::string_view hex) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2U);
    for (std::size_t i = 0; i + 1U < hex.size(); i += 2U) {
        const auto pair = std::string(hex.substr(i, 2U));
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16)));
    }
    return bytes;
}

std::vector<std::uint8_t> column_encoding_bytes() {
    return bytes_from_hex("0a1f2f6c616e63652e656e636f64696e67732e436f6c756d6e456e636f64696e6712020a00");
}

/// MiniBlockLayout for a variable-width (utf8/binary) column: the value buffer holds the offsets
/// followed by the bytes, so value_compression is CompressiveEncoding{ f2 Variable{ f1 offsets } }
/// rather than the Flat encoding a fixed-width column uses. Matches IPC2Lance / the Lance reference
/// PageLayout. `nullable` adds the f2 repdef_compression and the layers=[3] marker in the tail, in
/// the same places variable_width_structural_payload_zstd puts them.
std::vector<std::uint8_t> variable_width_structural_payload(std::uint32_t bits_token, std::uint64_t rows,
                                                            bool nullable) {
    // CompressiveEncoding{ f2 Variable{ f1 offsets = Flat{ f1 bits } } }.
    const std::vector<std::uint8_t> value_comp{
        0x12, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08, static_cast<std::uint8_t>(bits_token)};
    std::vector<std::uint8_t> out;
    if (nullable) {
        write_length_delimited(out, 2, repdef_encoding_bytes());
    }
    write_length_delimited(out, 3, value_comp);
    const auto tail = mini_block_tail(rows, 1U, nullable);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

std::vector<std::uint8_t> page_layout_bytes(std::uint32_t bits_token, std::uint64_t rows, bool variable_width,
                                            bool nullable = false) {
    std::vector<std::uint8_t> page_layout;
    if (variable_width) {
        write_length_delimited(page_layout, 1, variable_width_structural_payload(bits_token, rows, nullable));
    } else {
        write_length_delimited(page_layout, 1, build_mini_block_layout(bits_token, rows, nullable));
    }

    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

// Variable-width structural payload whose value_compression is wrapped in General(ZSTD), so the
// chunk's value buffer is interpreted as [u64 LE uncompressed size][zstd frame]. Byte layout mirrors
// what lance 7.0 emits for a zstd variable-width column (see memory: lance-zstd-variable-encoding).
std::vector<std::uint8_t> variable_width_structural_payload_zstd(std::uint8_t bits_token, std::uint64_t rows,
                                                                 bool nullable) {
    // Uncompressed variable CompressiveEncoding body: f2 Variable{ f1 offsets = Flat{ f1 bits } }.
    const std::vector<std::uint8_t> inner_ce{0x12, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08, bits_token};
    // General{ f1 BufferCompression{ f1 scheme = ZSTD(2) }, f3 values = inner_ce }.
    std::vector<std::uint8_t> general{0x0a, 0x02, 0x08, 0x02, 0x1a, static_cast<std::uint8_t>(inner_ce.size())};
    general.insert(general.end(), inner_ce.begin(), inner_ce.end());
    // value_compression CompressiveEncoding{ f10 General }.
    std::vector<std::uint8_t> value_comp{0x52, static_cast<std::uint8_t>(general.size())};
    value_comp.insert(value_comp.end(), general.begin(), general.end());
    // MiniBlockLayout [f2 repdef,] f3 = value_compression, then the tail.
    std::vector<std::uint8_t> out;
    if (nullable) {
        write_length_delimited(out, 2, repdef_encoding_bytes());
    }
    out.push_back(0x1aU);
    out.push_back(static_cast<std::uint8_t>(value_comp.size()));
    out.insert(out.end(), value_comp.begin(), value_comp.end());
    const auto tail = mini_block_tail(rows, 1U, nullable);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

std::vector<std::uint8_t> page_layout_bytes_variable_zstd(std::uint8_t bits_token, std::uint64_t rows,
                                                          bool nullable = false) {
    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 1, variable_width_structural_payload_zstd(bits_token, rows, nullable));
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

// Fixed-width CompressiveEncoding wrapped General(ZSTD) -> ByteStreamSplit -> Flat, matching what stock
// Lance emits for a zstd-compressed, byte-stream-split float/double column. The field-tag numbers below
// were verified byte-for-byte against a real `lance` 8.0.0-written file (written via pylance with
// `lance-encoding:compression=zstd` + `lance-encoding:bss=on` field metadata, data_storage_version=2.2)
// by hex-dumping its on-disk page.encoding bytes and walking the protobuf wire format field by field;
// they are NOT guessed. Restricted to 32/64-bit values (bits_token 0x20/0x40) — matches stock Lance's
// own ByteStreamSplit restriction ("only supports 32-bit (f32) or 64-bit (f64) values").
//   CompressiveEncoding{ f1 Flat{f1 bits} }                          -- innermost
//   -> ByteStreamSplit{ f1 values = above }                          -- f9 of CompressiveEncoding
//   -> General{ f1 BufferCompression{f1 scheme=ZSTD(2)}, f3 values = above }   -- f10 of CompressiveEncoding
//   -> MiniBlockLayout.value_compression (f3) = CompressiveEncoding{ f10 General = above }
std::vector<std::uint8_t> fixed_width_structural_payload_bss_zstd(std::uint8_t bits_token, std::uint64_t rows,
                                                                  bool nullable) {
    const std::vector<std::uint8_t> flat_ce{0x0a, 0x02, 0x08, bits_token};
    std::vector<std::uint8_t> bss{0x0a, static_cast<std::uint8_t>(flat_ce.size())};
    bss.insert(bss.end(), flat_ce.begin(), flat_ce.end());
    std::vector<std::uint8_t> bss_ce{0x4a, static_cast<std::uint8_t>(bss.size())};
    bss_ce.insert(bss_ce.end(), bss.begin(), bss.end());
    std::vector<std::uint8_t> general{0x0a, 0x02, 0x08, 0x02, 0x1a, static_cast<std::uint8_t>(bss_ce.size())};
    general.insert(general.end(), bss_ce.begin(), bss_ce.end());
    std::vector<std::uint8_t> value_comp{0x52, static_cast<std::uint8_t>(general.size())};
    value_comp.insert(value_comp.end(), general.begin(), general.end());
    std::vector<std::uint8_t> out;
    if (nullable) {
        write_length_delimited(out, 2, repdef_encoding_bytes());
    }
    out.push_back(0x1aU);                                         // f3 value_compression
    out.push_back(static_cast<std::uint8_t>(value_comp.size()));
    out.insert(out.end(), value_comp.begin(), value_comp.end());
    const auto tail = mini_block_tail(rows, 1U, nullable);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

std::vector<std::uint8_t> page_layout_bytes_bss_zstd(std::uint8_t bits_token, std::uint64_t rows,
                                                     bool nullable = false) {
    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 1, fixed_width_structural_payload_bss_zstd(bits_token, rows, nullable));
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

void append_le32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void append_le64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    append_le32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
    append_le32(out, static_cast<std::uint32_t>(value >> 32U));
}

// Assemble one miniblock chunk holding multiple buffers (has_large_chunk => u32 sizes), matching
// Lance's decode_miniblock_chunk: [u16 num_levels=0][u32 size_i...][pad8]([buf_i][pad8])*.
std::vector<std::uint8_t> build_multibuffer_chunk(const std::vector<std::vector<std::uint8_t>>& buffers) {
    std::vector<std::uint8_t> out;
    append_le16(out, 0U);  // num_levels (no rep/def)
    for (const auto& b : buffers) {
        append_le32(out, static_cast<std::uint32_t>(b.size()));
    }
    while (out.size() % 8U != 0U) {
        out.push_back(0U);
    }
    for (const auto& b : buffers) {
        out.insert(out.end(), b.begin(), b.end());
        while (out.size() % 8U != 0U) {
            out.push_back(0U);
        }
    }
    return out;
}

// MiniBlockLayout tail (f6 layers, f7 num_buffers, f9 num_items, f10 has_large_chunk).
std::vector<std::uint8_t> miniblock_tail(std::uint64_t num_items, std::uint8_t num_buffers,
                                         bool has_large_chunk = true) {
    std::vector<std::uint8_t> t{0x32, 0x01, 0x01, 0x38, num_buffers, 0x48};
    append_varint(t, num_items);
    t.push_back(0x50);
    t.push_back(has_large_chunk ? 0x01U : 0x00U);
    return t;
}

// PageLayout for a run-length-encoded fixed-width column: value_compression =
// Rle{ values=Flat(value_bits), run_lengths=Flat(length_bits) }, num_buffers=2.
std::vector<std::uint8_t> page_layout_bytes_rle(std::uint8_t value_bits, std::uint8_t length_bits,
                                                std::uint64_t num_items) {
    std::vector<std::uint8_t> structural{0x1a, 0x0e, 0x42, 0x0c, 0x0a, 0x04, 0x0a, 0x02,
                                         0x08, value_bits, 0x12, 0x04, 0x0a, 0x02, 0x08, length_bits};
    const auto tail = miniblock_tail(num_items, 2U);
    structural.insert(structural.end(), tail.begin(), tail.end());
    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 1, structural);
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

// Build the dictionary's inner Variable block (Lance VariableEncoder block format):
// [u32 bits_per_offset=32][u32 bytes_start_offset][u32 offsets (N+1, relative to data, start 0)][data].
// For large_utf8 / large_binary every one of those words is a u64 and bits_per_offset is 64: Lance
// decodes the dictionary straight into the column's Arrow type and refuses 32-bit offsets for a large
// one ("expected 64-bit offsets but got 32-bit offsets"). Its descriptor says Flat(64) to match.
std::vector<std::uint8_t> build_dict_variable_block(const std::vector<std::string_view>& distinct, bool large) {
    const std::size_t n = distinct.size();
    const std::size_t word = large ? 8U : 4U;
    const auto put = [&](std::vector<std::uint8_t>& out, std::uint64_t v) {
        if (large) {
            append_le64(out, v);
        } else {
            append_le32(out, static_cast<std::uint32_t>(v));
        }
    };
    std::vector<std::uint8_t> out;
    put(out, large ? 64U : 32U);            // bits_per_offset
    put(out, 2U * word + (n + 1U) * word);  // where the data bytes start
    std::uint64_t cum = 0;
    put(out, 0U);  // offset[0]
    for (const auto& s : distinct) {
        cum += s.size();
        put(out, cum);
    }
    for (const auto& s : distinct) {
        out.insert(out.end(), s.begin(), s.end());
    }
    return out;
}

/// The last byte of a Variable{offsets = Flat(bits)} node: the offset width.
std::uint8_t offset_bits_token(bool large) {
    return large ? static_cast<std::uint8_t>(0x40U) : static_cast<std::uint8_t>(0x20U);
}

// PageLayout for structural dictionary with flat bitpacked u32 indices (matches stock Lance for
// scattered low-cardinality strings): value_compression = InlineBitpacking(32), dictionary =
// Variable+Flat(32) without general compression, num_buffers=1, has_large_chunk=true (the chunk-meta
// words are u32 to match -- see control_buffer_for_index_chunks).
std::vector<std::uint8_t> page_layout_bytes_dict(std::uint32_t num_distinct, std::uint64_t num_items, bool large) {
    static const std::uint8_t kF3Bitpack[] = {0x1a, 0x04, 0x2a, 0x02, 0x08, 0x20};
    static const std::uint8_t kF4Dict[] = {0x22, 0x08, 0x12, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x20};
    std::vector<std::uint8_t> structural(kF3Bitpack, kF3Bitpack + sizeof(kF3Bitpack));
    structural.insert(structural.end(), kF4Dict, kF4Dict + sizeof(kF4Dict));
    structural.back() = offset_bits_token(large);
    structural.push_back(0x28);  // f5 num_dictionary_items
    append_varint(structural, num_distinct);
    const auto tail = miniblock_tail(num_items, 1U);
    structural.insert(structural.end(), tail.begin(), tail.end());

    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 1, structural);
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

// PageLayout for a dictionary-encoded low-cardinality column: value_compression = Rle over u32
// indices, dictionary = General(ZSTD)+Variable, num_dictionary_items, num_buffers=2.
std::vector<std::uint8_t> page_layout_bytes_dict_rle(std::uint32_t num_distinct, std::uint64_t num_items,
                                                     bool large) {
    static const std::uint8_t kF3Rle[] = {0x1a, 0x0e, 0x42, 0x0c, 0x0a, 0x04, 0x0a, 0x02,
                                          0x08, 0x20, 0x12, 0x04, 0x0a, 0x02, 0x08, 0x08};
    static const std::uint8_t kF4Dict[] = {0x22, 0x10, 0x52, 0x0e, 0x0a, 0x02, 0x08, 0x02, 0x1a,
                                           0x08, 0x12, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x20};
    std::vector<std::uint8_t> structural(kF3Rle, kF3Rle + sizeof(kF3Rle));
    structural.insert(structural.end(), kF4Dict, kF4Dict + sizeof(kF4Dict));
    structural.back() = offset_bits_token(large);
    structural.push_back(0x28);  // f5 num_dictionary_items
    append_varint(structural, num_distinct);
    const auto tail = miniblock_tail(num_items, 2U);
    structural.insert(structural.end(), tail.begin(), tail.end());

    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 1, structural);
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

// Lance scalar value buffer for a length-1 string/binary array: [u32 num_buffers][u32 buf_len...]
// [buffers]. A utf8/binary value is 2 buffers (offsets [0,len] + data). See lance-arrow scalar.rs.
std::vector<std::uint8_t> encode_scalar_variable_value(const std::vector<std::uint8_t>& value, bool large) {
    std::vector<std::uint8_t> out;
    auto write_u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
        }
    };
    const std::uint32_t offsets_len = large ? 16U : 8U;
    write_u32(2U);                                          // num_buffers (offsets + data)
    write_u32(offsets_len);                                // buffer 0: offsets
    write_u32(static_cast<std::uint32_t>(value.size()));   // buffer 1: data
    if (large) {
        const std::uint64_t start = 0;
        const std::uint64_t end = value.size();
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<std::uint8_t>((start >> (8 * i)) & 0xFFU));
        }
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<std::uint8_t>((end >> (8 * i)) & 0xFFU));
        }
    } else {
        write_u32(0U);
        write_u32(static_cast<std::uint32_t>(value.size()));
    }
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

// PageLayout = ConstantLayout. Fixed-width constants store the value inline in the descriptor (zero
// data buffers); variable-width (string/binary) constants omit inline_value and store the single
// value in one data buffer instead. Bytes match lance output.
std::vector<std::uint8_t> constant_layout_message(const std::vector<std::uint8_t>* inline_value) {
    std::vector<std::uint8_t> constant_layout{0x2a, 0x01, 0x01};  // f5 layers = single non-null layer
    if (inline_value != nullptr) {
        write_length_delimited(constant_layout, 6, *inline_value);  // f6 inline_value, varint length
    }
    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 2, constant_layout);  // PageLayout f2 = constant_layout
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

// PageLayout = ConstantLayout with a NULLABLE_ITEM layer and no value: Lance's spelling of a column
// whose every row is null. Zero data buffers. Byte-identical to what pylance writes for pa.nulls(n).
std::vector<std::uint8_t> all_null_constant_layout_message() {
    const std::vector<std::uint8_t> constant_layout{0x2a, 0x01, 0x03};  // f5 layers = [NULLABLE_ITEM]
    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 2, constant_layout);
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

// MiniBlockLayout PageLayout advertising InlineBitpacking{uncompressed_bits_per_value}. Matches lance
// output (CompressiveEncoding f5 = inline_bitpacking). See memory: lance-inline-bitpacking-format.
std::vector<std::uint8_t> page_layout_bytes_inline_bitpacking(std::uint8_t uncompressed_bits, std::uint64_t rows,
                                                              bool nullable = false) {
    // value_compression CompressiveEncoding{ f5 InlineBitpacking{ f1 uncompressed_bits_per_value } }.
    const std::vector<std::uint8_t> ce{0x2a, 0x02, 0x08, uncompressed_bits};
    std::vector<std::uint8_t> structural;
    if (nullable) {
        write_length_delimited(structural, 2, repdef_encoding_bytes());
    }
    write_length_delimited(structural, 3, ce);  // MiniBlockLayout f3
    const auto tail = mini_block_tail(rows, 1U, nullable);
    structural.insert(structural.end(), tail.begin(), tail.end());

    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 1, structural);
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

// Bit width (0..bits) needed to represent the largest of `count` little-endian values of `width_bytes`.
unsigned chunk_bit_width(const std::uint8_t* src, std::size_t count, std::size_t width_bytes) {
    std::uint64_t max_value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t v = 0;
        std::memcpy(&v, src + i * width_bytes, width_bytes);
        max_value |= v;
    }
    unsigned bits = 0;
    while (max_value != 0U) {
        ++bits;
        max_value >>= 1U;
    }
    return bits;
}

// Build one bitpacked chunk buffer: [bit_width as one width_bytes word][FastLanes packed 1024 values],
// into `out`. `count` (<=1024) values are read from `src`; the rest of the 1024-block is zero-padded.
// Every byte of `out` is unconditionally overwritten, so callers reusing `out` across chunks pay no
// per-chunk allocation or zero-fill. Staging into `in` is one bulk memcpy (src is contiguous typed
// data), zeroing only the tail of the final partial chunk instead of the whole 1024-block every call.
template <class T>
void build_bitpacked_chunk_typed(const std::uint8_t* src, std::size_t count, std::vector<std::uint8_t>& out) {
    T in[1024];
    std::memcpy(in, src, count * sizeof(T));
    if (count < 1024U) {
        std::memset(in + count, 0, (1024U - count) * sizeof(T));
    }
    unsigned width = chunk_bit_width(src, count, sizeof(T));
    // thread_local + resize (not a fresh zero-filled vector per chunk): bounded by <=1024 words, fully
    // overwritten by pack_1024 below -- same reuse precedent as unpack_bitpacked_page on the read side.
    thread_local std::vector<T> packed;
    packed.resize(nano_lance::fastlanes::packed_words_1024<T>(width));
    nano_lance::fastlanes::pack_1024<T>(width, in, packed.data());
    out.resize(sizeof(T) * (1U + packed.size()));
    const T width_word = static_cast<T>(width);
    std::memcpy(out.data(), &width_word, sizeof(T));
    if (!packed.empty()) {
        std::memcpy(out.data() + sizeof(T), packed.data(), packed.size() * sizeof(T));
    }
}

void build_bitpacked_chunk(const std::uint8_t* src, std::size_t count, std::size_t width_bytes,
                           std::vector<std::uint8_t>& out) {
    switch (width_bytes) {
        case 1U:
            return build_bitpacked_chunk_typed<std::uint8_t>(src, count, out);
        case 2U:
            return build_bitpacked_chunk_typed<std::uint16_t>(src, count, out);
        case 4U:
            return build_bitpacked_chunk_typed<std::uint32_t>(src, count, out);
        default:
            return build_bitpacked_chunk_typed<std::uint64_t>(src, count, out);
    }
}

// Frame a raw buffer as Lance's general-compression payload: [u64 LE uncompressed size][zstd frame].
bool zstd_frame_buffer(const std::vector<std::uint8_t>& raw, int level, std::vector<std::uint8_t>& out,
                       std::string& error) {
    const auto bound = ZSTD_compressBound(raw.size());
    // resize() (not assign(n, 0)): the 8-byte header is filled byte-by-byte below and ZSTD_compress
    // unconditionally overwrites the rest, so this only needs to zero a growing size delta (or nothing,
    // if the caller reuses `out` across calls of similar or shrinking size) rather than always
    // re-touching all n bytes the way assign(n, val) is specified to.
    out.resize(8U + bound);
    const std::uint64_t uncompressed = raw.size();
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((uncompressed >> (8 * i)) & 0xFFU);
    }
    // Reused per-thread compression context: one-shot ZSTD_compress() allocates AND ZEROES a fresh
    // multi-hundred-KB context (hash/chain tables, window) on every call, which callgrind showed as
    // ~40% of a compressed float column's write instructions across its ~150 per-chunk calls -- far
    // more than the actual compression work. ZSTD_compressCCtx on a reused context skips that setup
    // cost and produces byte-identical output (same algorithm, same level).
    thread_local std::unique_ptr<ZSTD_CCtx, std::size_t (*)(ZSTD_CCtx*)> cctx(ZSTD_createCCtx(),
                                                                              &ZSTD_freeCCtx);
    const auto csize = cctx != nullptr
                           ? ZSTD_compressCCtx(cctx.get(), out.data() + 8U, bound, raw.data(), raw.size(), level)
                           : ZSTD_compress(out.data() + 8U, bound, raw.data(), raw.size(), level);
    if (ZSTD_isError(csize) != 0U) {
        error = std::string("zstd compress failed: ") + ZSTD_getErrorName(csize);
        return false;
    }
    out.resize(8U + csize);
    return true;
}

std::size_t padded_size(std::size_t size, std::size_t alignment) {
    return (size + alignment - 1U) & ~(alignment - 1U);
}

template <typename OffsetType>
bool read_offsets(const std::vector<std::uint8_t>& bytes, std::vector<OffsetType>& out, std::string& error) {
    if (bytes.size() % sizeof(OffsetType) != 0U) {
        error = "offset buffer size is not aligned";
        return false;
    }
    out.resize(bytes.size() / sizeof(OffsetType));
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

template <typename OffsetType>
bool build_variable_chunk_bytes(const std::vector<OffsetType>& offsets,
                                const std::vector<std::uint8_t>& data,
                                std::size_t first_value,
                                std::size_t last_value,
                                std::vector<std::uint8_t>& out) {
    const auto offset_width = sizeof(OffsetType);
    const auto num_values = last_value - first_value;
    const auto chunk_bytes = offsets[last_value] - offsets[first_value];
    const auto offsets_bytes = (num_values + 1U) * offset_width;
    const auto padded = padded_size(offsets_bytes + static_cast<std::size_t>(chunk_bytes), 8U);
    out.resize(padded, 0U);
    std::size_t write_offset = 0;
    for (std::size_t i = first_value; i <= last_value; ++i) {
        const auto relative = offsets[i] - offsets[first_value] + static_cast<OffsetType>(offsets_bytes);
        std::memcpy(out.data() + write_offset, &relative, offset_width);
        write_offset += offset_width;
    }
    const auto* chunk_start = data.data() + static_cast<std::size_t>(offsets[first_value]);
    std::memcpy(out.data() + offsets_bytes, chunk_start, static_cast<std::size_t>(chunk_bytes));
    return true;
}

template <typename OffsetType>
bool build_variable_chunks(const VariableWidthColumnValues& column,
                           std::vector<MiniblockChunk>& chunks,
                           std::string& error,
                           std::size_t max_values_per_chunk = 0U) {
    std::vector<OffsetType> offsets;
    if (!read_offsets(column.offsets, offsets, error)) {
        return false;
    }
    if (offsets.size() < 2U) {
        MiniblockChunk chunk;
        chunk.value_count = 0;
        chunks.push_back(std::move(chunk));
        return true;
    }
    const auto total_values = offsets.size() - 1U;
    const auto offset_width = sizeof(OffsetType);
    std::size_t first_value = 0;
    while (first_value < total_values) {
        // Grow the chunk as far as it fits, computing the packed size in O(1) per step directly from
        // the offsets: packed = pad8((num_values + 1) * offset_width + (offsets[end] - offsets[first])).
        // The previous version rebuilt (and memcpy'd) the whole growing chunk on every candidate value
        // just to measure it, which is O(n^2) in the chunk length and dominated uncompressed
        // variable-width writes. The full buffer is now materialized exactly once per finalized chunk.
        std::size_t last_value = first_value + 1U;  // at least one value per chunk (a lone oversized value gets its own)
        while (last_value < total_values) {
            // A chunk carrying definition levels is limited to one FastLanes block; callers pass
            // that cap in. Without nulls the chunk is bounded only by its byte budget.
            if (max_values_per_chunk != 0U && (last_value - first_value) >= max_values_per_chunk) {
                break;
            }
            const auto num_values = (last_value + 1U) - first_value;
            const auto data_bytes = static_cast<std::size_t>(offsets[last_value + 1U] - offsets[first_value]);
            const auto packed = padded_size((num_values + 1U) * offset_width + data_bytes, 8U);
            // The chunk's footprint -- 8-byte header, its definition levels, then these values --
            // has to fit the control word's 4095 x 8 bytes. Values alone may use all of it; a
            // chunk that also carries levels (at most 128 bytes: 1024 bits packed, or 64 raw u16)
            // has 128 bytes less. Without that, 1024 nullable ~32-byte strings overflowed the
            // word and stock Lance read past the page ("offset + length of the sliced Buffer").
            const std::size_t budget =
                max_values_per_chunk != 0U ? kMaxVariableMiniblockBytes - kMaxChunkLevelBytesFlat : kMaxVariableMiniblockBytes;
            if (packed > budget) {
                break;
            }
            last_value++;
        }
        MiniblockChunk chunk;
        if (!build_variable_chunk_bytes(offsets, column.data, first_value, last_value, chunk.bytes)) {
            error = "failed to build variable-width miniblock chunk";
            return false;
        }
        chunk.value_count = last_value - first_value;
        chunks.push_back(std::move(chunk));
        first_value = last_value;
    }
    return true;
}

bool build_variable_chunks_for_column(const VariableWidthColumnValues& column,
                                      std::vector<MiniblockChunk>& chunks,
                                      std::string& error,
                                      std::size_t max_values_per_chunk = 0U) {
    if (column.large) {
        return build_variable_chunks<std::int64_t>(column, chunks, error, max_values_per_chunk);
    }
    return build_variable_chunks<std::int32_t>(column, chunks, error, max_values_per_chunk);
}

}  // namespace

// ── Nested columns (lists, maps) ────────────────────────────────────────────────────────────────
//
// One leaf column under list layers: its levels come from repdef::serialize, its values are the
// leaf items those levels point at. Every page is ONE mini-block chunk -- the page's final chunk --
// so its value count is free (non-final chunks must hold 2^k values, which would split rows across
// chunks) and the page holds whole rows. Levels are bit-packed at the narrowest width that holds
// them, as pylance writes its own.

/// The narrowest bit width holding every level (at least 1). One width per page: the page descriptor
/// declares a single level encoding for all of its chunks.
unsigned level_width(const std::vector<std::uint16_t>& levels) {
    std::uint16_t max_level = 0;
    for (const auto level : levels) {
        max_level = std::max(max_level, level);
    }
    unsigned width = 1;
    while (width < 16U && (max_level >> width) != 0U) {
        ++width;
    }
    return width;
}

/// CompressiveEncoding{ f4 Bitpacked{ uncompressed 16, values Flat(width) } } -- the spelling pylance
/// writes for its own levels.
std::vector<std::uint8_t> levels_encoding(unsigned width) {
    std::vector<std::uint8_t> flat{0x08};
    append_varint(flat, width);
    std::vector<std::uint8_t> flat_ce;
    write_length_delimited(flat_ce, 1, flat);
    std::vector<std::uint8_t> bitpacked{0x08, 0x10};  // f1 uncompressed_bits_per_value = 16
    write_length_delimited(bitpacked, 3, flat_ce);    // f3 values
    std::vector<std::uint8_t> encoding;
    write_length_delimited(encoding, 4, bitpacked);
    return encoding;
}

/// Bit-pack `n` levels at `width`: whole 1024-level FastLanes blocks, then the tail either raw (u16
/// words) or padded to a full block -- whichever Lance's encoder would pick, because its decoder tells
/// the two apart by the buffer's length alone (the rule pack_definition_levels follows at width 1).
void pack_levels(const std::uint16_t* levels, std::size_t n, unsigned width, std::vector<std::uint8_t>& out) {
    const auto packed_words = nano_lance::fastlanes::packed_words_1024<std::uint16_t>(width);
    std::vector<std::uint16_t> packed(packed_words);
    std::uint16_t block[1024];
    out.clear();
    const auto append_words = [&out](const std::uint16_t* words, std::size_t count) {
        const auto at = out.size();
        out.resize(at + count * sizeof(std::uint16_t));
        std::memcpy(out.data() + at, words, count * sizeof(std::uint16_t));
    };
    std::size_t at = 0;
    for (; at + 1024U <= n; at += 1024U) {
        nano_lance::fastlanes::pack_1024<std::uint16_t>(width, levels + at, packed.data());
        append_words(packed.data(), packed_words);
    }
    const auto tail = n - at;
    if (tail != 0U) {
        const std::size_t padding_cost = width * (1024U - tail);
        const std::size_t pack_savings = (16U - width) * tail;
        if (padding_cost >= pack_savings) {
            append_words(levels + at, tail);
        } else {
            std::fill(std::begin(block), std::end(block), std::uint16_t{0});
            std::memcpy(block, levels + at, tail * sizeof(std::uint16_t));
            nano_lance::fastlanes::pack_1024<std::uint16_t>(width, block, packed.data());
            append_words(packed.data(), packed_words);
        }
    }
}

void append_u16_levels(std::vector<std::uint8_t>& out, const std::vector<std::uint16_t>& levels) {
    for (const auto level : levels) {
        append_le16(out, level);
    }
}

void pad8(std::vector<std::uint8_t>& out) {
    while (out.size() % 8U != 0U) {
        out.push_back(0U);
    }
}

std::uint64_t write_buffer(std::ofstream& out, const std::vector<std::uint8_t>& bytes) {
    align64(out);
    const auto at = pos(out);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return at;
}

std::vector<std::uint8_t> page_encoding(std::uint32_t layout_field, const std::vector<std::uint8_t>& layout) {
    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, layout_field, layout);
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

/// How a nested page's item values are encoded -- chosen once per page, since the descriptor declares
/// one value encoding for all of its chunks.
enum class ItemEncoding { kFlat, kBool, kBitpacked, kVariable };

ItemEncoding item_encoding_for(const LanceField& field, const ColumnValues& values) {
    if (values.kind == ColumnValues::Kind::VariableWidth) {
        return ItemEncoding::kVariable;
    }
    if (field.logical_type == "bool") {
        return ItemEncoding::kBool;
    }
    const auto width = lance_logical_type_value_bytes(field.logical_type);
    if (lance_logical_type_is_bitpackable_integer(field.logical_type) &&
        (width == 1U || width == 2U || width == 4U || width == 8U)) {
        return ItemEncoding::kBitpacked;
    }
    return ItemEncoding::kFlat;
}

/// The value_compression node for `encoding`.
std::vector<std::uint8_t> item_value_encoding(ItemEncoding encoding, const LanceField& field, bool large) {
    switch (encoding) {
        case ItemEncoding::kVariable:
            // CompressiveEncoding{ f2 Variable{ f1 offsets = CompressiveEncoding{ Flat(32 or 64) } } }
            return {0x12, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08, offset_bits_token(large)};
        case ItemEncoding::kBitpacked:
            // CompressiveEncoding{ f5 InlineBitpacking{ f1 uncompressed_bits_per_value } }
            return {0x2a, 0x02, 0x08,
                    static_cast<std::uint8_t>(lance_logical_type_value_bytes(field.logical_type) * 8U)};
        case ItemEncoding::kBool:
        case ItemEncoding::kFlat: {
            std::vector<std::uint8_t> flat{0x08};
            append_varint(flat, encoding == ItemEncoding::kBool ? 1U
                                                                : lance_logical_type_value_bytes(field.logical_type) * 8U);
            std::vector<std::uint8_t> out;
            write_length_delimited(out, 1, flat);
            return out;
        }
    }
    return {};
}

/// One chunk's value buffer: the items `items[first, first + count)`, gathered from the column by
/// leaf index. Bit-packed chunks hold at most 1024 values (one FastLanes block, width in its header),
/// which is why nested pages are cut into 1024-value chunks.
bool gather_chunk_values(const LanceField& field, const ColumnValues& values, ItemEncoding encoding,
                         const std::vector<std::uint64_t>& items, std::size_t first, std::size_t count,
                         std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    if (encoding == ItemEncoding::kVariable) {
        const bool large = values.variable.large;
        const auto width = static_cast<std::size_t>(large ? 8U : 4U);
        const auto offset = [&](std::uint64_t i) {
            std::int64_t v = 0;
            if (large) {
                std::memcpy(&v, values.variable.offsets.data() + i * 8U, 8U);
            } else {
                std::int32_t n = 0;
                std::memcpy(&n, values.variable.offsets.data() + i * 4U, 4U);
                v = n;
            }
            return v;
        };
        const auto entries = values.variable.offsets.size() / width;
        // The chunk's own offsets have the column's width too: u64 for large_utf8 / large_binary,
        // which Lance's decoder requires to match the Arrow type.
        const auto header = (count + 1U) * width;
        std::uint64_t total = header;
        for (std::size_t k = first; k < first + count; ++k) {
            if (items[k] + 1U >= entries) {
                error = "list items run past the column's values";
                return false;
            }
            total += static_cast<std::uint64_t>(offset(items[k] + 1U) - offset(items[k]));
        }
        if (!large && total > std::numeric_limits<std::uint32_t>::max()) {
            error = "a list chunk's strings exceed 4 GiB";
            return false;
        }
        out.resize(static_cast<std::size_t>(total));
        std::uint64_t at = header;
        const auto put = [&](std::size_t slot) {
            if (large) {
                std::memcpy(out.data() + slot * 8U, &at, 8U);
            } else {
                const auto narrow = static_cast<std::uint32_t>(at);
                std::memcpy(out.data() + slot * 4U, &narrow, 4U);
            }
        };
        put(0U);
        for (std::size_t k = 0; k < count; ++k) {
            const auto begin = offset(items[first + k]);
            const auto end = offset(items[first + k] + 1U);
            if (end > begin) {
                std::memcpy(out.data() + at, values.variable.data.data() + begin, static_cast<std::size_t>(end - begin));
            }
            at += static_cast<std::uint64_t>(end - begin);
            put(k + 1U);
        }
        // Lance's binary decompressor requires the chunk to be a whole number of offset words; the
        // flat string path pads to 8 for the same reason (see build_variable_chunk_bytes).
        pad8(out);
        return true;
    }
    const auto width = lance_logical_type_value_bytes(field.logical_type);
    const auto* data = values.fixed_borrowed != nullptr ? values.fixed_borrowed : values.fixed.data();
    const auto size = values.fixed_borrowed != nullptr ? values.fixed_borrowed_size : values.fixed.size();
    if (width == 0U) {
        error = "column '" + field.name + "': unsupported list item type " + field.logical_type;
        return false;
    }
    for (std::size_t k = first; k < first + count; ++k) {
        if ((items[k] + 1U) * width > size) {
            error = "list items run past the column's values";
            return false;
        }
    }
    if (encoding == ItemEncoding::kBool) {
        // Flat(1): the items' bits, LSB first -- the same packing a flat bool page uses.
        out.assign((count + 7U) / 8U, 0U);
        for (std::size_t k = 0; k < count; ++k) {
            if (data[items[first + k]] != 0U) {
                out[k >> 3U] |= static_cast<std::uint8_t>(1U << (k & 7U));
            }
        }
        return true;
    }
    // A chunk's items are nearly always consecutive values (a list's children, a flat column's rows):
    // then they are one slice of the value buffer, used in place.
    bool consecutive = true;
    for (std::size_t k = first + 1U; k < first + count && consecutive; ++k) {
        consecutive = items[k] == items[k - 1U] + 1U;
    }
    const std::uint8_t* chunk = count == 0U ? data : data + items[first] * width;
    std::vector<std::uint8_t> gathered;
    if (!consecutive) {
        gathered.resize(count * width);
        for (std::size_t k = 0; k < count; ++k) {
            std::memcpy(gathered.data() + k * width, data + items[first + k] * width, width);
        }
        chunk = gathered.data();
    }
    if (encoding == ItemEncoding::kBitpacked) {
        build_bitpacked_chunk(chunk, count, width, out);
        return true;
    }
    out.assign(chunk, chunk + count * width);
    return true;
}

/// One page of a nested column, built from its serialized levels: 1024 values per chunk (the last one
/// takes the rest), each chunk carrying the levels that lead up to its last value -- Lance's
/// RepDefSlicer rule: levels with no value slot after a chunk's last value belong to the next chunk.
/// Returns false with `too_big` set when a chunk's levels would not fit its u16 header fields, so the
/// caller can retry with fewer rows.
struct NestedPageBuffers {
    std::vector<std::uint8_t> control;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> dictionary;  // a dictionary-encoded string page only
    std::vector<std::uint8_t> rep_index;
    std::vector<std::uint8_t> descriptor;
};

/// A page's string items as a dictionary, when that pays: repeated tags, categories, keys -- the
/// usual content of a list of strings. `indices` has one entry per item. Chosen per page, like
/// pylance, and only when the dictionary plus 32-bit indices (bit-packed on disk, so smaller still)
/// come in under 80% of the plain strings.
bool plan_item_dictionary(const ColumnValues& values, const std::vector<std::uint64_t>& items,
                          std::vector<std::string_view>& distinct, std::vector<std::uint32_t>& indices) {
    constexpr std::size_t kMinItems = 64U;
    constexpr std::size_t kMaxDistinct = 65536U;
    distinct.clear();
    indices.clear();
    if (items.size() < kMinItems || values.kind != ColumnValues::Kind::VariableWidth) {
        return false;
    }
    const bool large = values.variable.large;
    const auto offset = [&](std::uint64_t i) {
        std::int64_t v = 0;
        if (large) {
            std::memcpy(&v, values.variable.offsets.data() + i * 8U, 8U);
        } else {
            std::int32_t n = 0;
            std::memcpy(&n, values.variable.offsets.data() + i * 4U, 4U);
            v = n;
        }
        return v;
    };
    const auto* base = reinterpret_cast<const char*>(values.variable.data.data());
    const auto value_at = [&](std::uint64_t i) {
        const auto begin = offset(i);
        return std::string_view(base + begin, static_cast<std::size_t>(offset(i + 1U) - begin));
    };
    // Most string pages that are not dictionary material (ids, text, URLs) are nearly all distinct,
    // and finding that out by hashing every value until half the page is distinct used to be a third
    // of a string column's write time. So a big page is sampled first -- kSample values spread
    // across it -- and its number of distinct values estimated with Chao1: D ~ d + f1^2 / (2 f2),
    // from the sample's distinct count d and the values seen once (f1) and twice (f2). Unlike the
    // sample's distinct ratio alone it is not fooled by one frequent value (the empty strings of null
    // rows next to unique text) and keeps a column drawing on a few thousand values (~5,000 of them:
    // ~90% distinct in the sample, estimated within a few percent). Only a page estimated at over
    // twice the rule's limit is declined without the full pass.
    constexpr std::size_t kSample = 1024U;
    if (items.size() >= 8U * kSample) {
        std::vector<std::string_view> sample(kSample);
        const auto stride = items.size() / kSample;
        for (std::size_t k = 0; k < kSample; ++k) {
            sample[k] = value_at(items[k * stride]);
        }
        std::sort(sample.begin(), sample.end());
        double d = 0.0;
        double f1 = 0.0;
        double f2 = 0.0;
        for (std::size_t k = 0; k < kSample;) {
            std::size_t run = 1;
            while (k + run < kSample && sample[k + run] == sample[k]) {
                ++run;
            }
            d += 1.0;
            f1 += run == 1U ? 1.0 : 0.0;
            f2 += run == 2U ? 1.0 : 0.0;
            k += run;
        }
        const auto estimate = d + f1 * (f1 - 1.0) / (2.0 * (f2 + 1.0));  // bias-corrected Chao1
        const auto limit = static_cast<double>(std::min<std::size_t>(kMaxDistinct, items.size() / 2U));
        if (estimate > 2.0 * limit) {
            return false;
        }
    }
    std::unordered_map<std::string_view, std::uint32_t> ids;
    ids.reserve(std::min<std::size_t>(items.size(), 4096U));
    std::uint64_t plain_bytes = 0;
    std::uint64_t dict_bytes = 0;
    indices.reserve(items.size());
    for (const auto i : items) {
        const auto value = value_at(i);
        plain_bytes += value.size() + 4U;
        const auto [it, inserted] = ids.try_emplace(value, static_cast<std::uint32_t>(distinct.size()));
        if (inserted) {
            distinct.push_back(value);
            dict_bytes += value.size() + 4U;
            if (distinct.size() > kMaxDistinct || distinct.size() * 2U > items.size()) {
                distinct.clear();
                indices.clear();
                return false;
            }
        }
        indices.push_back(it->second);
    }
    std::uint32_t index_bits = 1;
    while (index_bits < 32U && (std::uint64_t{1} << index_bits) < distinct.size()) {
        ++index_bits;
    }
    const auto index_bytes = (items.size() * index_bits + 7U) / 8U;
    if (static_cast<double>(dict_bytes + index_bytes) >= 0.8 * static_cast<double>(plain_bytes)) {
        distinct.clear();
        indices.clear();
        return false;
    }
    return true;
}

/// Is row `i` valid? An empty bitmap means every row is.
bool validity_bit(const std::vector<std::uint8_t>& validity, std::uint64_t i) {
    return validity.empty() || ((validity[static_cast<std::size_t>(i >> 3U)] >> (i & 7U)) & 1U) != 0U;
}

/// One offset of a variable-width column's offset buffer, 4 or 8 bytes wide.
void append_offset(std::vector<std::uint8_t>& offsets, std::int64_t value, bool large) {
    if (large) {
        const auto v = value;
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        offsets.insert(offsets.end(), p, p + 8);
    } else {
        const auto v = static_cast<std::int32_t>(value);
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        offsets.insert(offsets.end(), p, p + 4);
    }
}

/// Lance's rule for FSST on a mini-block page (`try_variable_miniblock` in lance-encoding): strings,
/// not binary, at least 32 KiB of them, the longest at least 5 bytes.
constexpr std::uint64_t kFsstMinBytes = 32U * 1024U;
constexpr std::uint64_t kFsstMinMaxLength = 5U;

bool fsst_applies_to(const LanceField& field) {
    return field.logical_type == "utf8" || field.logical_type == "large_utf8";
}

/// Offset `i` of a variable-width column, 4 or 8 bytes wide.
std::int64_t variable_offset(const ColumnValues& values, std::uint64_t i) {
    std::int64_t v = 0;
    if (values.variable.large) {
        std::memcpy(&v, values.variable.offsets.data() + i * 8U, 8U);
    } else {
        std::int32_t n = 0;
        std::memcpy(&n, values.variable.offsets.data() + i * 4U, 4U);
        v = n;
    }
    return v;
}

/// Train one FSST table for a whole string column, on a sample of all of its values (roadmap F2).
/// Every page is then compressed with it: training per page, as Lance does, was half of the write
/// time of a high-cardinality column for no measurable gain in size. Declines (false) below Lance's
/// thresholds -- under 32 KiB of strings, or none longer than 4 bytes -- or when nothing is learned.
bool train_column_fsst(const LanceField& field, const ColumnValues& values, fsst::Encoder& encoder) {
    if (!fsst_applies_to(field) || values.kind != ColumnValues::Kind::VariableWidth ||
        values.variable.data.size() < kFsstMinBytes) {
        return false;
    }
    const auto count = values.variable.offsets.size() / (values.variable.large ? 8U : 4U);
    if (count < 2U) {
        return false;
    }
    std::vector<std::pair<const std::uint8_t*, std::size_t>> views;
    views.reserve(count - 1U);
    std::uint64_t longest = 0;
    for (std::uint64_t i = 0; i + 1U < count; ++i) {
        const auto begin = variable_offset(values, i);
        const auto size = static_cast<std::size_t>(variable_offset(values, i + 1U) - begin);
        views.emplace_back(values.variable.data.data() + begin, size);
        longest = std::max<std::uint64_t>(longest, size);
    }
    return longest >= kFsstMinMaxLength && fsst::train(views, encoder);
}

/// Compress a page's items with the column's FSST table when that pays: into `packed`, a
/// variable-width column holding item k's compressed bytes as value k. Declines (false) when the
/// compressed values plus the 2,312-byte table would not be at least 10% smaller than the plain
/// values -- the page is then written plain.
bool plan_item_fsst(const fsst::Encoder& encoder, const ColumnValues& values, const std::vector<std::uint64_t>& items,
                    ColumnValues& packed) {
    const bool large = values.variable.large;
    std::uint64_t raw = 0;
    for (const auto i : items) {
        raw += static_cast<std::uint64_t>(variable_offset(values, i + 1U) - variable_offset(values, i));
    }
    packed = ColumnValues{};
    packed.kind = ColumnValues::Kind::VariableWidth;
    packed.variable.large = large;
    // Compressed into a reused scratch buffer sized for the worst case (every byte escaped: 2x), then
    // copied out at its real size: sizing the output itself for the worst case zero-filled twice the
    // page's bytes on every page.
    thread_local std::vector<std::uint8_t> scratch;
    if (scratch.size() < 2U * raw) {
        scratch.resize(static_cast<std::size_t>(2U * raw));
    }
    const auto width = large ? 8U : 4U;
    packed.variable.offsets.resize((items.size() + 1U) * width);
    auto* offsets = packed.variable.offsets.data();
    const auto put = [&](std::size_t slot, std::uint64_t value) {
        if (large) {
            std::memcpy(offsets + slot * 8U, &value, 8U);
        } else {
            const auto narrow = static_cast<std::int32_t>(value);
            std::memcpy(offsets + slot * 4U, &narrow, 4U);
        }
    };
    put(0U, 0U);
    std::size_t at = 0;
    std::size_t slot = 0;
    for (const auto i : items) {
        const auto begin = variable_offset(values, i);
        const auto size = static_cast<std::size_t>(variable_offset(values, i + 1U) - begin);
        at += fsst::compress_into(encoder, values.variable.data.data() + begin, size, scratch.data() + at);
        put(++slot, at);
    }
    packed.variable.data.assign(scratch.data(), scratch.data() + at);
    return (at + fsst::kSymbolTableBytes) * 10U < raw * 9U;
}

/// With zstd requested, strings go to FSST only if it pays on the column's first page-sized run of
/// rows; otherwise they keep the zstd pages, where a page FSST declined would be stored raw.
bool fsst_pays_on_first_page(const fsst::Encoder& encoder, const ColumnValues& values, std::uint64_t rows) {
    std::vector<std::uint64_t> items(static_cast<std::size_t>(std::min<std::uint64_t>(rows, 32768U)));
    for (std::size_t k = 0; k < items.size(); ++k) {
        items[k] = k;
    }
    ColumnValues packed;
    return plan_item_fsst(encoder, values, items, packed);
}

bool build_nested_page(const LanceField& field, const ColumnValues& values, const repdef::Serialized& ser,
                       std::uint64_t rows, const fsst::Encoder* fsst_encoder, NestedPageBuffers& page, bool& too_big,
                       std::string& error) {
    constexpr std::size_t kValuesPerChunk = 1024U;  // 2^10: the log in every non-final chunk's word
    constexpr std::size_t kMaxChunkLevelBytes = 65535U;
    too_big = false;
    page = NestedPageBuffers{};
    const auto encoding = item_encoding_for(field, values);
    std::vector<std::string_view> distinct;
    std::vector<std::uint32_t> indices;
    const bool dictionary = encoding == ItemEncoding::kVariable && plan_item_dictionary(values, ser.items, distinct, indices);
    // Not repetitive enough for a dictionary: FSST, when it pays. Its chunks are ordinary Variable
    // chunks of the COMPRESSED values, gathered from `fsst_values` in page order.
    ColumnValues fsst_values;
    std::vector<std::uint8_t> fsst_table;
    std::vector<std::uint64_t> fsst_items;
    const bool fsst = encoding == ItemEncoding::kVariable && !dictionary && fsst_encoder != nullptr &&
                      plan_item_fsst(*fsst_encoder, values, ser.items, fsst_values);
    if (fsst) {
        fsst_table = fsst::serialize(*fsst_encoder);
        fsst_items.resize(ser.items.size());
        for (std::size_t k = 0; k < fsst_items.size(); ++k) {
            fsst_items[k] = k;
        }
    }
    const auto rep_width = ser.has_rep ? level_width(ser.rep) : 0U;
    const auto def_width = ser.has_def ? level_width(ser.def) : 0U;
    const std::uint16_t max_rep = ser.has_rep ? *std::max_element(ser.rep.begin(), ser.rep.end()) : 0U;
    const auto num_levels = ser.has_rep ? ser.rep.size() : ser.has_def ? ser.def.size() : ser.items.size();

    std::size_t level_at = 0;
    std::size_t item_at = 0;
    std::vector<std::uint8_t> rep_bytes;
    std::vector<std::uint8_t> def_bytes;
    std::vector<std::uint8_t> value_bytes;
    std::vector<std::uint64_t> rep_index;
    const auto num_chunks = std::max<std::size_t>(1U, (ser.items.size() + kValuesPerChunk - 1U) / kValuesPerChunk);
    for (std::size_t c = 0; c < num_chunks; ++c) {
        const bool last = c + 1U == num_chunks;
        const auto values_here = last ? ser.items.size() - item_at : kValuesPerChunk;
        // This chunk's levels: up to and including its last value slot (all the rest, if final).
        std::size_t level_end = level_at;
        if (last) {
            level_end = num_levels;
        } else if (ser.is_slot.empty()) {
            level_end = level_at + values_here;
        } else {
            std::size_t taken = 0;
            while (taken < values_here) {
                taken += ser.is_slot[level_end] ? 1U : 0U;
                ++level_end;
            }
        }
        const auto chunk_levels = level_end - level_at;
        if (ser.has_rep) {
            pack_levels(ser.rep.data() + level_at, chunk_levels, rep_width, rep_bytes);
        }
        if (ser.has_def) {
            pack_levels(ser.def.data() + level_at, chunk_levels, def_width, def_bytes);
        }
        if (chunk_levels > 0xFFFFU || rep_bytes.size() > kMaxChunkLevelBytes || def_bytes.size() > kMaxChunkLevelBytes) {
            too_big = true;
            return false;
        }
        if (dictionary) {
            // The chunk holds its items' dictionary indices, bit-packed like any u32 column.
            build_bitpacked_chunk(reinterpret_cast<const std::uint8_t*>(indices.data() + item_at), values_here, 4U,
                                  value_bytes);
        } else if (!gather_chunk_values(field, fsst ? fsst_values : values, encoding, fsst ? fsst_items : ser.items,
                                        item_at, values_here, value_bytes, error)) {
            return false;
        }
        // Chunk: [u16 levels][u16 rep bytes][u16 def bytes][u32 value bytes] padded to 8, then each
        // buffer padded to 8.
        const auto chunk_start = page.payload.size();
        auto& chunk = page.payload;
        append_le16(chunk, static_cast<std::uint16_t>(ser.has_rep || ser.has_def ? chunk_levels : 0U));
        if (ser.has_rep) {
            append_le16(chunk, static_cast<std::uint16_t>(rep_bytes.size()));
        }
        if (ser.has_def) {
            append_le16(chunk, static_cast<std::uint16_t>(def_bytes.size()));
        }
        append_le32(chunk, static_cast<std::uint32_t>(value_bytes.size()));
        pad8(chunk);
        if (ser.has_rep) {
            chunk.insert(chunk.end(), rep_bytes.begin(), rep_bytes.end());
            pad8(chunk);
        }
        if (ser.has_def) {
            chunk.insert(chunk.end(), def_bytes.begin(), def_bytes.end());
            pad8(chunk);
        }
        chunk.insert(chunk.end(), value_bytes.begin(), value_bytes.end());
        pad8(chunk);
        const auto footprint = chunk.size() - chunk_start;
        const std::uint32_t log_values = last ? 0U : 10U;
        append_le32(page.control, static_cast<std::uint32_t>(((footprint / 8U - 1U) << 4U) | log_values));

        // Repetition index, depth 1, as Lance's encoder computes it: rows that FINISH in this chunk,
        // and the levels left over after the last row start (0 in the final chunk). A chunk that
        // begins a row turns the previous chunk's leftovers into one more finished row.
        if (ser.has_rep) {
            const auto* r = ser.rep.data() + level_at;
            std::uint64_t finished = 0;
            for (std::size_t i = 1; i < chunk_levels; ++i) {
                finished += r[i] == max_rep ? 1U : 0U;
            }
            std::uint64_t leftovers = 0;
            if (!last) {
                leftovers = chunk_levels;
                for (std::size_t i = chunk_levels; i-- > 0;) {
                    if (r[i] == max_rep) {
                        leftovers = chunk_levels - i;
                        break;
                    }
                }
            }
            if (c != 0U && chunk_levels != 0U && r[0] == max_rep && rep_index.back() != 0U) {
                rep_index[rep_index.size() - 2U] += 1U;
                rep_index.back() = 0U;
            }
            if (last) {
                finished += 1U;
            }
            rep_index.push_back(finished);
            rep_index.push_back(leftovers);
        }
        level_at = level_end;
        item_at += values_here;
    }
    for (const auto v : rep_index) {
        for (int b = 0; b < 8; ++b) {
            page.rep_index.push_back(static_cast<std::uint8_t>((v >> (8 * b)) & 0xFFU));
        }
    }
    (void)rows;

    std::vector<std::uint8_t> mini;
    if (ser.has_rep) {
        write_length_delimited(mini, 1, levels_encoding(rep_width));
    }
    if (ser.has_def) {
        write_length_delimited(mini, 2, levels_encoding(def_width));
    }
    if (dictionary) {
        // Indices: InlineBitpacking(32). Dictionary: Variable{Flat(32 or 64)}, stored raw -- the same
        // block the flat string path writes (build_dict_variable_block).
        const bool large = values.variable.large;
        write_length_delimited(mini, 3, std::vector<std::uint8_t>{0x2a, 0x02, 0x08, 0x20});
        write_length_delimited(mini, 4, std::vector<std::uint8_t>{0x12, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08,
                                                                  offset_bits_token(large)});
        mini.push_back(0x28U);  // f5 num_dictionary_items
        append_varint(mini, distinct.size());
        page.dictionary = build_dict_variable_block(distinct, large);
    } else if (fsst) {
        // CompressiveEncoding{ f6 Fsst{ f1 symbol_table, f2 values = Variable{Flat(32 or 64)} } }
        std::vector<std::uint8_t> fsst_message;
        write_length_delimited(fsst_message, 1, fsst_table);
        write_length_delimited(fsst_message, 2, item_value_encoding(encoding, field, values.variable.large));
        std::vector<std::uint8_t> value_encoding;
        write_length_delimited(value_encoding, 6, fsst_message);
        write_length_delimited(mini, 3, value_encoding);
    } else {
        write_length_delimited(mini, 3, item_value_encoding(encoding, field, values.variable.large));
    }
    std::vector<std::uint8_t> layer_bytes(ser.layers.begin(), ser.layers.end());
    write_length_delimited(mini, 6, layer_bytes);
    mini.push_back(0x38U);  // f7 num_buffers
    mini.push_back(0x01U);
    if (ser.has_rep) {
        mini.push_back(0x40U);  // f8 repetition_index_depth
        mini.push_back(0x01U);
    }
    mini.push_back(0x48U);  // f9 num_items
    append_varint(mini, ser.items.size());
    mini.push_back(0x50U);  // f10 has_large_chunk
    mini.push_back(0x01U);
    page.descriptor = page_encoding(1, mini);
    return true;
}

bool write_nested_column(std::ofstream& out, const LanceField& field, const ColumnValues& values, std::uint64_t rows,
                         pb::ColumnMetadata& column, std::string& error, const fsst::Encoder* fsst_encoder = nullptr) {
    // One FSST table for the whole column, used by every page it pays on.
    fsst::Encoder trained;
    if (fsst_encoder == nullptr && train_column_fsst(field, values, trained)) {
        fsst_encoder = &trained;
    }
    std::vector<repdef::SerializeLayer> layers;
    for (const auto& layer : values.layers) {
        layers.push_back({layer.is_list, &layer.offsets, &layer.validity});
    }
    // A flat column (no list or struct layer) comes here too, for its multi-chunk pages -- FSST
    // needs one symbol table over many chunks. Its levels are just item validity.
    const bool flat = values.layers.empty();
    const auto serialize_rows = [&](std::uint64_t first, std::uint64_t n, repdef::Serialized& ser) {
        if (!flat) {
            return repdef::serialize(layers, values.validity, first, n, ser, error);
        }
        ser = repdef::Serialized{};
        ser.items.resize(static_cast<std::size_t>(n));
        bool nulls = false;
        for (std::uint64_t k = 0; k < n; ++k) {
            ser.items[static_cast<std::size_t>(k)] = first + k;
            nulls = nulls || !validity_bit(values.validity, first + k);
        }
        ser.layers = {nulls ? repdef::kNullableItem : repdef::kAllValidItem};
        if (nulls) {
            ser.has_def = true;
            ser.def.resize(static_cast<std::size_t>(n));
            for (std::uint64_t k = 0; k < n; ++k) {
                ser.def[static_cast<std::size_t>(k)] = validity_bit(values.validity, first + k) ? 0U : 1U;
            }
        }
        return true;
    };
    if (!flat && values.layers.front().length != rows) {
        error = "column '" + field.name + "' holds " + std::to_string(values.layers.front().length) +
                " rows, the fragment " + std::to_string(rows);
        return false;
    }
    // Pages are cut at row boundaries. A page may hold many chunks; what bounds it is the reader's
    // working set, so keep a page to a few MiB of values and a bounded number of rows.
    constexpr std::size_t kMaxValueBytes = std::size_t{8} << 20U;
    constexpr std::uint64_t kMaxRowsPerPage = 32768U;
    column.encoding = column_encoding_bytes();
    std::uint64_t row = 0;
    std::uint64_t try_rows = kMaxRowsPerPage;
    repdef::Serialized ser;
    NestedPageBuffers built;
    while (row < rows) {
        const auto n = std::min(try_rows, rows - row);
        if (!serialize_rows(row, n, ser)) {
            error = "column '" + field.name + "': " + error;
            return false;
        }
        pb::ColumnPage page;
        page.length = n;
        page.priority = 0;
        if (ser.items.empty()) {
            // No values at all -- every list empty or null. Lance writes a ConstantLayout with no
            // value and the levels as raw u16 buffers [rep, def]; so does this.
            std::vector<std::uint8_t> layer_bytes(ser.layers.begin(), ser.layers.end());
            std::vector<std::uint8_t> constant;
            write_length_delimited(constant, 5, layer_bytes);
            constant.push_back(0x48U);  // f9 num_rep_values
            append_varint(constant, ser.rep.size());
            constant.push_back(0x50U);  // f10 num_def_values
            append_varint(constant, ser.def.size());
            std::vector<std::uint8_t> raw_rep;
            std::vector<std::uint8_t> raw_def;
            append_u16_levels(raw_rep, ser.rep);
            append_u16_levels(raw_def, ser.def);
            page.buffer_offsets.push_back(write_buffer(out, raw_rep));
            page.buffer_sizes.push_back(raw_rep.size());
            page.buffer_offsets.push_back(write_buffer(out, raw_def));
            page.buffer_sizes.push_back(raw_def.size());
            page.encoding = page_encoding(2, constant);
        } else {
            bool too_big = false;
            const bool built_ok = build_nested_page(field, values, ser, n, fsst_encoder, built, too_big, error);
            if (!built_ok && !too_big) {
                error = "column '" + field.name + "': " + error;
                return false;
            }
            if (too_big || (built.payload.size() > kMaxValueBytes && n > 1U)) {
                if (n == 1U) {
                    error = "column '" + field.name + "': row " + std::to_string(row) +
                            " holds more list entries than one page chunk can describe";
                    return false;
                }
                try_rows = std::max<std::uint64_t>(1U, n / 2U);
                continue;
            }
            page.buffer_offsets.push_back(write_buffer(out, built.control));
            page.buffer_sizes.push_back(built.control.size());
            page.buffer_offsets.push_back(write_buffer(out, built.payload));
            page.buffer_sizes.push_back(built.payload.size());
            if (!built.dictionary.empty()) {
                page.buffer_offsets.push_back(write_buffer(out, built.dictionary));
                page.buffer_sizes.push_back(built.dictionary.size());
            }
            if (ser.has_rep) {
                page.buffer_offsets.push_back(write_buffer(out, built.rep_index));
                page.buffer_sizes.push_back(built.rep_index.size());
            }
            page.encoding = built.descriptor;
        }
        column.pages.push_back(std::move(page));
        row += n;
        try_rows = std::min<std::uint64_t>(kMaxRowsPerPage, try_rows * 2U);
    }
    return true;
}

bool write_lance_data_file(const std::filesystem::path& dataset_path,
                           const std::string& file_name,
                           const LanceSchemaMapping& mapping,
                           const std::vector<ColumnValues>& column_values,
                           std::uint64_t rows,
                           int compression_level,
                           bool compress,
                           DataFileResult& result,
                           std::string& error) {
    error.clear();
    if (mapping.fields.empty()) {
        error = "cannot write Lance data file without mapped fields";
        return false;
    }
    const auto physical_fields = lance_physical_fields(mapping);
    if (column_values.size() != physical_fields.size()) {
        error = "column value count does not match physical schema field count";
        return false;
    }

    const auto relative_path = std::filesystem::path("data") / file_name;
    const auto full_path = dataset_path / relative_path;
    std::error_code ec;
    std::filesystem::create_directories(full_path.parent_path(), ec);
    if (ec) {
        error = "failed to create data directory: " + ec.message();
        return false;
    }

    std::ofstream out(full_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open data file for writing";
        return false;
    }

    std::vector<pb::ColumnMetadata> columns;
    columns.reserve(physical_fields.size());
    for (std::size_t field_index = 0; field_index < physical_fields.size(); ++field_index) {
        const auto& field = *physical_fields[field_index];
        const auto& values = column_values[field_index];

        // Nulls are encodable only on the paths that have been taught the definition-level layer.
        // Everything else must refuse rather than drop them: ingest no longer rejects a null batch,
        // so this is the backstop that keeps a column with nulls from being written as if it had
        // none -- which is exactly the silent [10, null, 30] -> [10, 0, 30] corruption this whole
        // area is here to prevent.
        const bool column_has_nulls = values.null_count != 0U;
        if (column_has_nulls) {
            if (values.kind == ColumnValues::Kind::BlobV2External) {
                error = "column '" + field.name +
                        "' is a lance.blob.v2 external reference and cannot carry nulls yet";
                return false;
            }
            if (values.validity.empty()) {
                error = "column '" + field.name + "' reports " + std::to_string(values.null_count) +
                        " nulls but carries no validity bitmap";
                return false;
            }
        }

        if (values.kind == ColumnValues::Kind::BlobV2External) {
            const auto& blob = values.blob_v2;
            if (blob.row_packed_sizes.size() != static_cast<std::size_t>(rows)) {
                error = "blob v2 row count does not match fragment row count for ";
                error += field.name;
                return false;
            }
            if (blob.packed_payload.empty() && rows != 0U) {
                error = "blob v2 packed payload is empty for ";
                error += field.name;
                return false;
            }

            const auto control = blob_v2_build_control_buffer(blob.row_packed_sizes);

            align64(out);
            const auto values_offset = pos(out);
            out.write(reinterpret_cast<const char*>(blob.packed_payload.data()),
                      static_cast<std::streamsize>(blob.packed_payload.size()));

            align64(out);
            const auto control_offset = pos(out);
            out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));

            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            pb::ColumnPage page;
            page.buffer_offsets.push_back(values_offset);
            page.buffer_offsets.push_back(control_offset);
            page.buffer_sizes.push_back(blob.packed_payload.size());
            page.buffer_sizes.push_back(control.size());
            page.length = rows;
            page.priority = 0;
            page.encoding = blob_v2_column_page_encoding();
            column.pages.push_back(std::move(page));
            columns.push_back(std::move(column));
            continue;
        }

        if (values.needs_nested_pages()) {
            pb::ColumnMetadata column;
            if (!write_nested_column(out, field, values, rows, column, error)) {
                return false;
            }
            columns.push_back(std::move(column));
            continue;
        }

        // Arrow's null type: every row is null and there is no value to store at all.
        if (field.logical_type == "null") {
            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            pb::ColumnPage page;
            page.length = rows;
            page.priority = 0;
            page.encoding = all_null_constant_layout_message();
            column.pages.push_back(std::move(page));
            columns.push_back(std::move(column));
            continue;
        }

        // Constant column (tagged by the writer): ConstantLayout. The single value comes from the
        // field metadata. Fixed-width stores it inline (zero data buffers); variable-width stores it
        // in one data buffer.
        const auto packing_it = field.metadata.find("nanolance:packing");
// Skipped for a column with nulls: these encodings rewrite the value sequence (a constant column
        // stores one value, RLE stores runs, a dictionary stores indices) and the definition-level layer is
        // positional -- one level per value, in row order. Pairing them needs the levels to be encoded
        // against the rewritten sequence, which is not done yet, so a nullable column takes the flat or
        // bit-packed path instead. It costs size, never correctness.
        // A fixed-width constant is inlined in the descriptor, which Lance caps at 32 bytes. Wider
        // ones fall through to the flat path -- this used to write the inline value's length as ONE
        // byte, so any value of 128 bytes or more produced a descriptor neither nanolance nor pylance
        // could parse: a corrupt file, from a constant fixed_size_binary(200) column.
        const bool constant_fits =
            lance_field_is_variable_width(field.logical_type) ||
            lance_logical_type_value_bytes(field.logical_type) <= kMaxInlineConstantBytes;
        if (!column_has_nulls && constant_fits && packing_it != field.metadata.end() &&
            packing_it->second == "constant") {
            const auto value_it = field.metadata.find("nanolance:const-value");
            if (value_it == field.metadata.end()) {
                error = "constant column missing nanolance:const-value for ";
                error += field.name;
                return false;
            }
            const std::vector<std::uint8_t> value(value_it->second.begin(), value_it->second.end());
            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            pb::ColumnPage page;
            page.length = rows;
            page.priority = 0;
            if (lance_field_is_variable_width(field.logical_type)) {
                const bool large =
                    lance_logical_type_has_large_offsets(field.logical_type);
                const auto scalar_buffer = encode_scalar_variable_value(value, large);
                align64(out);
                const auto value_offset = pos(out);
                out.write(reinterpret_cast<const char*>(scalar_buffer.data()),
                          static_cast<std::streamsize>(scalar_buffer.size()));
                page.buffer_offsets.push_back(value_offset);
                page.buffer_sizes.push_back(scalar_buffer.size());
                page.encoding = constant_layout_message(nullptr);  // value is in the data buffer
            } else {
                page.encoding = constant_layout_message(&value);  // inline, no data buffers
            }
            column.pages.push_back(std::move(page));
            columns.push_back(std::move(column));
            continue;
        }

        // Run-length encoded fixed-width column (tagged by the writer): one chunk with two buffers
        // (run values + run lengths) -> Rle PageLayout.
        if (!column_has_nulls && packing_it != field.metadata.end() && packing_it->second == "rle" &&
            values.kind == ColumnValues::Kind::FixedWidth) {
            const auto bpv = value_width_bytes(field);
            const std::size_t n = values.fixed_size() / bpv;
            std::vector<std::uint8_t> run_values;
            std::vector<std::uint8_t> run_lengths;  // Lance requires 8-bit run lengths
            auto emit_run = [&](std::size_t row, std::uint64_t run) {
                for (std::uint64_t remaining = run; remaining > 0;) {
                    const auto take = static_cast<std::uint8_t>(std::min<std::uint64_t>(255U, remaining));
                    run_values.insert(run_values.end(), values.fixed_data() + row * bpv,
                                      values.fixed_data() + (row + 1U) * bpv);
                    run_lengths.push_back(take);
                    remaining -= take;
                }
            };
            if (values.fixed_rle_plan.computed) {
                // The write-side "is RLE beneficial?" heuristic already detected every run while
                // deciding -- reuse it verbatim instead of re-running the same memcmp-based scan.
                for (const auto& [row, run] : values.fixed_rle_plan.runs) {
                    emit_run(row, run);
                }
            } else {
                std::size_t i = 0;
                while (i < n) {
                    std::size_t run = 1;
                    while (i + run < n && std::memcmp(values.fixed_data() + (i + run) * bpv,
                                                      values.fixed_data() + i * bpv, bpv) == 0) {
                        ++run;
                    }
                    emit_run(i, run);
                    i += run;
                }
            }
            const std::size_t length_bytes = 1U;
            const auto chunk_bytes = build_multibuffer_chunk({run_values, run_lengths});
            MiniblockChunk chunk;
            chunk.bytes = chunk_bytes;
            chunk.value_count = n;
            const auto control = control_buffer_for({chunk});

            align64(out);
            const auto control_offset = pos(out);
            out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));
            align64(out);
            const auto data_offset = pos(out);
            out.write(reinterpret_cast<const char*>(chunk_bytes.data()), static_cast<std::streamsize>(chunk_bytes.size()));

            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            pb::ColumnPage page;
            page.buffer_offsets.push_back(control_offset);
            page.buffer_offsets.push_back(data_offset);
            page.buffer_sizes.push_back(control.size());
            page.buffer_sizes.push_back(chunk_bytes.size());
            page.length = rows;
            page.priority = 0;
            page.encoding = page_layout_bytes_rle(static_cast<std::uint8_t>(bpv * 8U),
                                                  static_cast<std::uint8_t>(length_bytes * 8U), rows);
            column.pages.push_back(std::move(page));
            columns.push_back(std::move(column));
            continue;
        }

        // Dictionary + RLE for a low-cardinality variable-width column: distinct values in buffer[2],
        // per-row u32 indices RLE'd in buffer[1].
        if (!column_has_nulls && packing_it != field.metadata.end() && packing_it->second == "dict-rle" &&
            values.kind == ColumnValues::Kind::VariableWidth) {
            const bool large =
                lance_logical_type_has_large_offsets(field.logical_type);
            const std::size_t ow = large ? 8U : 4U;
            const std::size_t num_rows = values.variable.offsets.size() / ow - 1U;
            auto read_offset = [&](std::size_t idx) -> std::int64_t {
                const auto* p = values.variable.offsets.data() + idx * ow;
                if (large) {
                    std::int64_t v = 0;
                    std::memcpy(&v, p, 8);
                    return v;
                }
                std::int32_t v = 0;
                std::memcpy(&v, p, 4);
                return v;
            };
            std::vector<std::string_view> distinct;
            std::vector<std::uint8_t> run_values;
            std::vector<std::uint8_t> run_lengths;
            const bool have_rle_plan = values.structural_dict_rle_plan.computed;
            if (have_rle_plan) {
                // The write-side "is dict-RLE beneficial?" heuristic already detected every run and
                // built the (run-keyed, not row-keyed) dictionary while deciding -- reuse it verbatim
                // instead of re-scanning row-by-row and re-running RLE detection from scratch.
                distinct = values.structural_dict_rle_plan.distinct;
                for (const auto& [index, run_length] : values.structural_dict_rle_plan.runs) {
                    for (std::uint64_t remaining = run_length; remaining > 0;) {
                        const auto take = static_cast<std::uint8_t>(std::min<std::uint64_t>(255U, remaining));
                        append_le32(run_values, index);
                        run_lengths.push_back(take);
                        remaining -= take;
                    }
                }
            } else {
                // Dictionary keyed by string_view into the stable column data buffer: no per-row heap
                // string allocation and hash lookups instead of full-string red-black-tree comparisons.
                std::unordered_map<std::string_view, std::uint32_t> dict;
                dict.reserve(num_rows / 4U + 1U);
                std::vector<std::uint32_t> indices;
                indices.reserve(num_rows);
                for (std::size_t r = 0; r < num_rows; ++r) {
                    const auto s = read_offset(r);
                    const auto e = read_offset(r + 1);
                    const std::string_view val(reinterpret_cast<const char*>(values.variable.data.data() + s),
                                               static_cast<std::size_t>(e - s));
                    const auto id = static_cast<std::uint32_t>(distinct.size());
                    const auto [it, inserted] = dict.try_emplace(val, id);
                    if (inserted) {
                        distinct.push_back(val);
                    }
                    indices.push_back(it->second);
                }
                // RLE the u32 indices (8-bit sub-runs).
                for (std::size_t r = 0; r < indices.size();) {
                    std::size_t run = 1;
                    while (r + run < indices.size() && indices[r + run] == indices[r]) {
                        ++run;
                    }
                    for (std::size_t remaining = run; remaining > 0;) {
                        const std::size_t take = std::min<std::size_t>(255U, remaining);
                        append_le32(run_values, indices[r]);
                        run_lengths.push_back(static_cast<std::uint8_t>(take));
                        remaining -= take;
                    }
                    r += run;
                }
            }
            const auto chunk_bytes = build_multibuffer_chunk({run_values, run_lengths});
            MiniblockChunk chunk;
            chunk.bytes = chunk_bytes;
            const auto control = control_buffer_for({chunk});
            std::vector<std::uint8_t> dict_frame;
            if (!zstd_frame_buffer(build_dict_variable_block(distinct, values.variable.large), compression_level,
                                   dict_frame, error)) {
                return false;
            }

            align64(out);
            const auto control_offset = pos(out);
            out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));
            align64(out);
            const auto data_offset = pos(out);
            out.write(reinterpret_cast<const char*>(chunk_bytes.data()), static_cast<std::streamsize>(chunk_bytes.size()));
            align64(out);
            const auto dict_offset = pos(out);
            out.write(reinterpret_cast<const char*>(dict_frame.data()), static_cast<std::streamsize>(dict_frame.size()));

            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            pb::ColumnPage page;
            page.buffer_offsets.push_back(control_offset);
            page.buffer_offsets.push_back(data_offset);
            page.buffer_offsets.push_back(dict_offset);
            page.buffer_sizes.push_back(control.size());
            page.buffer_sizes.push_back(chunk_bytes.size());
            page.buffer_sizes.push_back(dict_frame.size());
            page.length = rows;
            page.priority = 0;
            page.encoding =
                page_layout_bytes_dict_rle(static_cast<std::uint32_t>(distinct.size()), rows, values.variable.large);
            column.pages.push_back(std::move(page));
            columns.push_back(std::move(column));
            continue;
        }

        // Structural dictionary for scattered low-cardinality strings: flat bitpacked u32 indices in
        // buffer[1], uncompressed dictionary variable block in buffer[2].
        if (!column_has_nulls && packing_it != field.metadata.end() && packing_it->second == "dict" &&
            values.kind == ColumnValues::Kind::VariableWidth) {
            const bool large =
                lance_logical_type_has_large_offsets(field.logical_type);
            const std::size_t ow = large ? 8U : 4U;
            const std::size_t num_rows = values.variable.offsets.size() / ow - 1U;
            auto read_offset = [&](std::size_t idx) -> std::int64_t {
                const auto* p = values.variable.offsets.data() + idx * ow;
                if (large) {
                    std::int64_t v = 0;
                    std::memcpy(&v, p, 8);
                    return v;
                }
                std::int32_t v = 0;
                std::memcpy(&v, p, 4);
                return v;
            };
            // Reuse the dictionary the write-side heuristic already built for this column when it is
            // available (the common path via the public writer): the (dedup + per-row index) scan is
            // identical, so recomputing it here would double the hashing/comparison work for exactly
            // the columns this encoding targets. Fall back to building it for direct callers of
            // write_lance_data_file that never ran the heuristic (e.g. some tests).
            std::unordered_map<std::string_view, std::uint32_t> dict;  // only used on the fallback path
            std::vector<std::string_view> local_distinct;
            std::vector<std::uint32_t> local_indices;
            const bool have_plan = values.structural_dict_plan.computed;
            if (!have_plan) {
                dict.reserve(num_rows / 4U + 1U);
                local_indices.reserve(num_rows);
                for (std::size_t r = 0; r < num_rows; ++r) {
                    const auto s = read_offset(r);
                    const auto e = read_offset(r + 1);
                    const std::string_view val(reinterpret_cast<const char*>(values.variable.data.data() + s),
                                               static_cast<std::size_t>(e - s));
                    const auto id = static_cast<std::uint32_t>(local_distinct.size());
                    const auto [it, inserted] = dict.try_emplace(val, id);
                    if (inserted) {
                        local_distinct.push_back(val);
                    }
                    local_indices.push_back(it->second);
                }
            }
            const std::vector<std::string_view>& distinct =
                have_plan ? values.structural_dict_plan.distinct : local_distinct;
            const std::vector<std::uint32_t>& indices =
                have_plan ? values.structural_dict_plan.indices : local_indices;
            std::vector<MiniblockChunk> index_chunks;
            for (std::size_t off = 0; off < indices.size(); off += 1024U) {
                const auto count = std::min<std::size_t>(1024U, indices.size() - off);
                MiniblockChunk chunk;
                chunk.value_count = count;
                build_bitpacked_chunk(reinterpret_cast<const std::uint8_t*>(indices.data() + off), count, 4U,
                                      chunk.bytes);
                index_chunks.push_back(std::move(chunk));
            }
            const auto payload = miniblock_payload(index_chunks);
            const auto control = control_buffer_for_index_chunks(index_chunks);
            const auto dict_block = build_dict_variable_block(distinct, values.variable.large);

            align64(out);
            const auto control_offset = pos(out);
            out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));
            align64(out);
            const auto data_offset = pos(out);
            out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
            align64(out);
            const auto dict_offset = pos(out);
            out.write(reinterpret_cast<const char*>(dict_block.data()), static_cast<std::streamsize>(dict_block.size()));

            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            pb::ColumnPage page;
            page.buffer_offsets.push_back(control_offset);
            page.buffer_offsets.push_back(data_offset);
            page.buffer_offsets.push_back(dict_offset);
            page.buffer_sizes.push_back(control.size());
            page.buffer_sizes.push_back(payload.size());
            page.buffer_sizes.push_back(dict_block.size());
            page.length = rows;
            page.priority = 0;
            page.encoding =
                page_layout_bytes_dict(static_cast<std::uint32_t>(distinct.size()), rows, values.variable.large);
            column.pages.push_back(std::move(page));
            columns.push_back(std::move(column));
            continue;
        }

        std::vector<MiniblockChunk> chunks;
        const bool is_variable = values.kind == ColumnValues::Kind::VariableWidth;
        // Bitpacking is a structural encoding, signalled by the writer's packing tag (independent of the
        // zstd `compress` flag, which now gates only the variable-width zstd path below).
        const bool bitpack = !is_variable && packing_it != field.metadata.end() &&
                             packing_it->second == "bitpack" &&
                             lance_logical_type_is_bitpackable_integer(field.logical_type);
        // Byte-stream-split + zstd: float/double, tagged when set_compression(true) (writer.cpp).
        const bool bss_zstd = !is_variable && packing_it != field.metadata.end() &&
                              packing_it->second == "bss-zstd";
        // bool is always stored bit-packed (1 bit/value, LSB-first) on disk, matching stock Lance's own
        // Flat{bits_per_value:1} representation (verified empirically) -- not gated by any opt-in flag.
        const bool bool_pack = !is_variable && field.logical_type == "bool";
        const auto fixed_bytes_per_value = value_width_bytes(field);
        if (!is_variable) {
            if (values.fixed_size() % fixed_bytes_per_value != 0U) {
                error = "column value buffer size is not aligned to field width for ";
                error += field.name;
                return false;
            }
            if (values.fixed_size() / fixed_bytes_per_value != static_cast<std::size_t>(rows)) {
                error = "column value count does not match row count for ";
                error += field.name;
                return false;
            }
        }

        // Flat (uncompressed) fixed-width column: stream each miniblock chunk directly from the value
        // buffer -- the chunk bytes are just a slice of values.fixed, so building a MiniblockChunk
        // first would only copy them. Chunks are gathered into pages of ~kTargetMiniblockPageBytes
        // (MiniblockPageWriter); a value too wide for two to share a chunk gets a page per chunk.
        if (!is_variable && !bitpack && !bss_zstd && !bool_pack) {
            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            const auto total = values.fixed_size() / fixed_bytes_per_value;
            std::uint64_t fsl_items = 0;
            {
                std::string element;
                (void)lance_fixed_size_list_parts(field.logical_type, element, fsl_items);
            }
            // A chunk that carries definition levels is limited to one FastLanes block (1024
            // values); without nulls the chunk is sized by its byte budget, rounded down to the
            // power of two a non-final chunk needs.
            const auto by_bytes = max_values_per_uncompressed_chunk(fixed_bytes_per_value);
            const auto multi = multichunk_values(column_has_nulls ? std::min<std::size_t>(1024U, by_bytes) : by_bytes);
            const auto max_chunk_values = multi != 0U ? multi : std::size_t{1};
            // Every full page produces IDENTICAL page-encoding bytes: build them once per row count.
            std::uint64_t cached_rows = 0;
            std::vector<std::uint8_t> cached_encoding;
            MiniblockPageWriter page_writer(out);
            for (std::size_t off = 0; off < total;) {
                const auto count = std::min(max_chunk_values, total - off);
                const auto chunk_bytes = count * fixed_bytes_per_value;
                const auto chunk_repdef =
                    column_has_nulls ? pack_definition_levels(values.validity, off, count)
                                     : std::vector<std::uint8_t>{};
                page_writer.begin_chunk();
                const auto footprint =
                    column_has_nulls
                        ? stream_miniblock_payload_with_repdef(
                              out, chunk_repdef, count,
                              values.fixed_data() + off * fixed_bytes_per_value, chunk_bytes)
                        : stream_flat_miniblock_payload(
                              out, values.fixed_data() + off * fixed_bytes_per_value, chunk_bytes);
                page_writer.end_chunk(footprint, count);
                off += count;
                if (off < total && multi != 0U && page_writer.payload_bytes() < kTargetMiniblockPageBytes) {
                    continue;
                }
                pb::ColumnPage page;
                const auto page_rows = page_writer.rows();
                page_writer.finish(page);
                if (page_rows != cached_rows || cached_encoding.empty()) {
                    cached_rows = page_rows;
                    cached_encoding =
                        page_layout_bytes_flat(flat_bits_per_value(field), page_rows, column_has_nulls, fsl_items);
                }
                page.encoding = cached_encoding;
                column.pages.push_back(std::move(page));
            }
            columns.push_back(std::move(column));
            continue;
        }

        // Bitpack / bool / byte-stream-split+zstd fixed-width columns: STREAM one chunk at a time
        // through hoisted, loop-reused scratch buffers (build chunk -> write it -> reuse); after the
        // first chunk resize() touches nothing, since chunks are equal-sized. Pages gather chunks as
        // the flat path above does.
        if (bitpack || bool_pack || bss_zstd) {
            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            const auto total = bool_pack ? values.fixed_size() : values.fixed_size() / fixed_bytes_per_value;
            std::size_t step = bitpack      ? 1024U  // one FastLanes block per chunk
                               : bool_pack ? kMaxBoolValuesPerChunk
                                           : max_values_per_uncompressed_chunk(fixed_bytes_per_value);
            if (column_has_nulls) {
                step = std::min<std::size_t>(step, 1024U);  // see pack_definition_levels
            }
            // A power of two for every non-final chunk. For byte-stream-split it also keeps the raw
            // chunk at half the 32 KiB cap, leaving room for a zstd frame that does not shrink.
            const auto multi = multichunk_values(step);
            step = multi != 0U ? multi : std::size_t{1};
            std::vector<std::uint8_t> scratch;  // built chunk bytes, reused across chunks
            std::vector<std::uint8_t> framed;   // zstd frame (bss-zstd only), reused across chunks
            auto build_page_encoding = [&](std::uint64_t count) {
                if (bitpack) {
                    return page_layout_bytes_inline_bitpacking(
                        static_cast<std::uint8_t>(fixed_bytes_per_value * 8U), count, column_has_nulls);
                }
                if (bool_pack) {
                    // Plain Flat{bits_per_value:1} -- no CompressiveEncoding wrapper, matching stock Lance.
                    return page_layout_bytes_flat(flat_bits_per_value(field), count, column_has_nulls);
                }
                // byte-stream-split is restricted to 32/64-bit values, so the cast is safe.
                return page_layout_bytes_bss_zstd(static_cast<std::uint8_t>(flat_bits_per_value(field)), count,
                                                  column_has_nulls);
            };
            std::uint64_t cached_rows = 0;
            std::vector<std::uint8_t> cached_encoding;
            MiniblockPageWriter page_writer(out);
            for (std::size_t off = 0; off < total;) {
                const auto count = std::min(step, total - off);
                if (bitpack) {
                    // Each chunk buffer = [bit_width][FastLanes packed 1024 values].
                    build_bitpacked_chunk(values.fixed_data() + off * fixed_bytes_per_value, count,
                                          fixed_bytes_per_value, scratch);
                } else if (bool_pack) {
                    // Bit-pack LSB-first, ceil(count/8) bytes; no zstd on top (stock Lance doesn't
                    // compress 1-bit-packed bool either). values.fixed is one byte per value here.
                    boolpack::pack_lsb_first(values.fixed_data() + off, count, scratch);
                } else {
                    // Byte-transpose (mantissa/exponent bytes grouped) so the zstd frame compresses
                    // meaningfully, then frame it: bytes become [u64 raw size][zstd].
                    bss::transpose(values.fixed_data() + off * fixed_bytes_per_value,
                                   fixed_bytes_per_value, count, scratch);
                    if (!zstd_frame_buffer(scratch, compression_level, framed, error)) {
                        return false;
                    }
                }
                const auto& chunk_bytes = bss_zstd ? framed : scratch;
                const auto chunk_repdef =
                    column_has_nulls ? pack_definition_levels(values.validity, off, count)
                                     : std::vector<std::uint8_t>{};
                page_writer.begin_chunk();
                const auto footprint =
                    column_has_nulls ? stream_miniblock_payload_with_repdef(out, chunk_repdef, count,
                                                                            chunk_bytes.data(),
                                                                            chunk_bytes.size())
                                     : stream_flat_miniblock_payload(out, chunk_bytes.data(),
                                                                     chunk_bytes.size());
                page_writer.end_chunk(footprint, count);
                off += count;
                if (off < total && multi != 0U && page_writer.payload_bytes() < kTargetMiniblockPageBytes) {
                    continue;
                }
                pb::ColumnPage page;
                const auto page_rows = page_writer.rows();
                page_writer.finish(page);
                if (page_rows != cached_rows || cached_encoding.empty()) {
                    cached_rows = page_rows;
                    cached_encoding = build_page_encoding(page_rows);
                }
                page.encoding = cached_encoding;
                column.pages.push_back(std::move(page));
            }
            columns.push_back(std::move(column));
            continue;
        }

        // A string column FSST can compress (roadmap F2) goes to the multi-chunk page writer: one
        // symbol table per page, over 1024-value chunks. The single-chunk pages below would pay the
        // 2,312-byte table every 32 KiB. build_nested_page still declines FSST page by page when it
        // does not pay, writing plain Variable chunks instead.
        // Only when the writer's structural detection tagged the column: FSST is one of those
        // encodings, and set_structural_encoding(false) must still write plain pages.
        fsst::Encoder fsst_encoder;
        if (packing_it != field.metadata.end() && packing_it->second == "fsst" &&
            train_column_fsst(field, values, fsst_encoder) &&
            (!compress || fsst_pays_on_first_page(fsst_encoder, values, rows))) {
            pb::ColumnMetadata column;
            if (!write_nested_column(out, field, values, rows, column, error, &fsst_encoder)) {
                return false;
            }
            columns.push_back(std::move(column));
            continue;
        }

        // Variable-width columns keep the two-phase build (chunks are unequal-sized, driven by the
        // offsets math in build_variable_chunks_for_column). A nullable column additionally caps each
        // chunk at one FastLanes block, since that is what a definition-level buffer covers.
        if (!build_variable_chunks_for_column(values.variable, chunks, error,
                                              column_has_nulls ? 1024U : 0U)) {
            return false;
        }
        if (column_has_nulls) {
            std::uint64_t row = 0;
            for (auto& chunk : chunks) {
                chunk.repdef = pack_definition_levels(values.validity, row, chunk.value_count);
                row += chunk.value_count;
            }
        }

        const bool zstd_variable = compress;
        pb::ColumnMetadata column;
        column.encoding = column_encoding_bytes();
        // Hoisted out of the loop (not freshly declared per chunk): zstd_frame_buffer's resize() only
        // avoids re-zeroing already-there bytes when the buffer is reused across calls of similar size,
        // which requires swap (not move) below so `framed`'s capacity survives being handed off.
        std::vector<std::uint8_t> framed;
        for (auto& chunk : chunks) {
            if (zstd_variable) {
                if (!zstd_frame_buffer(chunk.bytes, compression_level, framed, error)) {
                    return false;
                }
                std::swap(chunk.bytes, framed);  // value_count unchanged; bytes are now [u64][zstd]
            }
            const auto control = control_buffer_for(chunk);
            const auto payload = miniblock_payload(chunk);

            align64(out);
            const auto control_offset = pos(out);
            out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));

            align64(out);
            const auto payload_offset = pos(out);
            out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));

            pb::ColumnPage page;
            page.buffer_offsets.push_back(control_offset);
            page.buffer_offsets.push_back(payload_offset);
            page.buffer_sizes.push_back(control.size());
            page.buffer_sizes.push_back(payload.size());
            page.length = chunk.value_count;
            page.priority = 0;
            const auto bits_token =
                values.variable.large ? static_cast<std::uint8_t>(0x40U) : static_cast<std::uint8_t>(0x20U);
            page.encoding = zstd_variable
                                ? page_layout_bytes_variable_zstd(bits_token, chunk.value_count,
                                                                  column_has_nulls)
                                : page_layout_bytes(bits_token, chunk.value_count, true, column_has_nulls);
            column.pages.push_back(std::move(page));
        }
        columns.push_back(std::move(column));
    }

    pb::FileDescriptor descriptor;
    descriptor.length = rows;
    for (const auto& mapped_field : mapping.fields) {
        pb::Field field;
        field.name = mapped_field.name;
        field.logical_type = lance_on_disk_logical_type(mapped_field.logical_type);
        field.id = mapped_field.id;
        field.parent_id = mapped_field.parent_id;
        field.type = 2;
        field.nullable = mapped_field.nullable;
        field.encoding = lance_on_disk_field_encoding(mapped_field.logical_type);
        for (const auto& kv : mapped_field.metadata) {
            field.metadata[kv.first] = std::vector<std::uint8_t>(kv.second.begin(), kv.second.end());
        }
        descriptor.fields.push_back(std::move(field));
    }
    const auto descriptor_bytes = pb::encode_file_descriptor(descriptor);
    align64(out);
    const auto global_buffer_offset = pos(out);
    out.write(reinterpret_cast<const char*>(descriptor_bytes.data()), static_cast<std::streamsize>(descriptor_bytes.size()));

    align64(out);
    const auto column_metadata_start = pos(out);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> column_metadata_offsets;
    for (const auto& column : columns) {
        const auto encoded = pb::encode_column_metadata(column);
        const auto offset = pos(out);
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        column_metadata_offsets.emplace_back(offset, encoded.size());
    }

    align64(out);
    const auto column_offsets_start = pos(out);
    for (const auto& [offset, size] : column_metadata_offsets) {
        write_le64(out, offset);
        write_le64(out, size);
    }

    const auto global_offsets_start = pos(out);
    write_le64(out, global_buffer_offset);
    write_le64(out, descriptor_bytes.size());

    write_le64(out, column_metadata_start);
    write_le64(out, column_offsets_start);
    write_le64(out, global_offsets_start);
    write_le32(out, 1);
    write_le32(out, static_cast<std::uint32_t>(columns.size()));
    write_le16(out, 2);
    write_le16(out, 2);
    out.write("LANC", 4);
    out.flush();
    if (!out) {
        error = "failed to write Lance data file";
        return false;
    }

    result.relative_path = relative_path;
    result.file_size_bytes = std::filesystem::file_size(full_path, ec);
    if (ec) {
        error = "failed to stat data file: " + ec.message();
        return false;
    }
    return true;
}

}  // namespace nano_lance
