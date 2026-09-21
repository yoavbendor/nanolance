// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/data_file_writer.hpp"

#include "lance_minimal.pb.hpp"
#include "nanolance/blob_v2_external.hpp"
#include "nanolance/bool_bitpack.hpp"
#include "nanolance/byte_stream_split.hpp"
#include "nanolance/fastlanes_bitpack.hpp"
#include "nanolance/schema_mapper.hpp"

#include <zstd.h>

#include <array>
#include <cstring>
#include <fstream>
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
constexpr std::uint32_t kMaxUncompressedMiniblockBytes = kMaxVariableMiniblockBytes;
// Bool is bit-packed (1 bit/value) at the on-disk boundary; a chunk's packed payload must stay within
// the same 32760-byte miniblock cap as every other chunk kind, so it can hold 8x as many values.
constexpr std::size_t kMaxBoolValuesPerChunk = static_cast<std::size_t>(kMaxUncompressedMiniblockBytes) * 8U;

struct MiniblockChunk {
    std::vector<std::uint8_t> bytes;
    std::size_t value_count = 0;
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
std::vector<std::uint8_t> build_mini_block_layout(std::uint32_t bits_per_value_token, std::uint64_t num_items) {
    std::vector<std::uint8_t> flat;                  // Flat{ f1 bits_per_value }
    flat.push_back(0x08U);
    append_varint(flat, bits_per_value_token);

    std::vector<std::uint8_t> compressive;           // CompressiveEncoding{ f1 Flat }
    write_length_delimited(compressive, 1, flat);

    std::vector<std::uint8_t> mini;
    write_length_delimited(mini, 3, compressive);    // f3 value_compression
    mini.push_back(0x32U);                           // f6 layers
    mini.push_back(0x01U);
    mini.push_back(0x01U);
    mini.push_back(0x38U);                           // f7 num_buffers
    mini.push_back(0x01U);
    mini.push_back(0x48U);                           // f9 num_items
    append_varint(mini, num_items);
    mini.push_back(0x50U);                           // f10 has_large_chunk
    mini.push_back(0x01U);
    return mini;
}

/// Byte length of the f3 value_compression submessage that build_mini_block_layout emits, i.e. how
/// much of its output the variable-width wrappers below replace. Derived rather than assumed: the
/// wrappers used to skip a fixed 6 bytes, which only held while the width was a single byte.
std::size_t mini_block_value_compression_prefix(std::uint32_t bits_per_value_token) {
    std::vector<std::uint8_t> flat;
    flat.push_back(0x08U);
    append_varint(flat, bits_per_value_token);
    std::vector<std::uint8_t> compressive;
    write_length_delimited(compressive, 1, flat);
    std::vector<std::uint8_t> prefix;
    write_length_delimited(prefix, 3, compressive);
    return prefix.size();
}

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

std::vector<std::uint8_t> control_buffer_for(const std::vector<MiniblockChunk>& chunks) {
    std::vector<std::uint8_t> out;
    out.reserve(chunks.size() * 2U);
    for (std::size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
        const auto& chunk = chunks[chunk_index];
        const auto words = static_cast<std::uint16_t>((chunk.bytes.size() + 7U) / 8U);
        std::uint16_t entry = static_cast<std::uint16_t>(words << 4U);
        const bool is_last = chunk_index + 1U == chunks.size();
        (void)chunk.value_count;
        entry |= 0U;
        append_le16(out, entry);
    }
    if (out.size() < 4U) {
        out.push_back(0U);
        out.push_back(0U);
    }
    return out;
}

// Chunk-meta (control) buffer for a multi-chunk miniblock page with has_large_chunk=false and one
// value buffer per chunk (the structural-dictionary index chunks). Each u16 word is
// (wrapped_bytes/8 - 1) << 4 | log2(num_values), where wrapped_bytes is the chunk's full footprint in
// the value buffer as written by miniblock_payload (8-byte chunk header + buffer, padded to 8). Lance
// requires every non-final chunk to carry a nonzero log2 (num_values = 1 << log2, so full chunks must
// be a power of two) and derives the final chunk's value count from the page's total item count. The
// shared control_buffer_for() writes log2=0 for every chunk and sizes the raw buffer, which only works
// for single-chunk pages; multi-chunk pages (a >1024-row dictionary column) need this exact layout to
// be readable by stock Lance.
std::vector<std::uint8_t> control_buffer_for_index_chunks(const std::vector<MiniblockChunk>& chunks) {
    std::vector<std::uint8_t> out;
    out.reserve(chunks.size() * 2U);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const std::size_t wrapped = ((8U + chunks[i].bytes.size()) + 7U) / 8U * 8U;
        const auto divided_minus_one = static_cast<std::uint16_t>(wrapped / 8U - 1U);
        std::uint16_t log_num_values = 0U;
        if (i + 1U < chunks.size()) {
            for (std::size_t v = chunks[i].value_count; v > 1U; v >>= 1U) {
                ++log_num_values;
            }
        }
        append_le16(out, static_cast<std::uint16_t>((divided_minus_one << 4U) | (log_num_values & 0x0FU)));
    }
    return out;
}

void append_miniblock_chunk(std::vector<std::uint8_t>& out, const MiniblockChunk& chunk) {
    out.push_back(0U);
    out.push_back(0U);
    append_le16(out, static_cast<std::uint16_t>(chunk.bytes.size()));
    out.push_back(0U);
    out.push_back(0U);
    out.push_back(0xFEU);
    out.push_back(0xFEU);
    out.insert(out.end(), chunk.bytes.begin(), chunk.bytes.end());
    while (out.size() % 8U != 0U) {
        out.push_back(0U);
    }
}

std::vector<std::uint8_t> miniblock_payload(const std::vector<MiniblockChunk>& chunks) {
    std::vector<std::uint8_t> out;
    for (const auto& chunk : chunks) {
        append_miniblock_chunk(out, chunk);
    }
    return out;
}

// Single-chunk fast paths: nanolance emits one chunk per page, so the general vector-based helpers
// above would otherwise force callers to wrap each chunk in a temporary one-element vector (an extra
// heap allocation and copy of the whole chunk on every page). These avoid that entirely.
std::vector<std::uint8_t> control_buffer_for(const MiniblockChunk& chunk) {
    std::vector<std::uint8_t> out;
    const auto words = static_cast<std::uint16_t>((chunk.bytes.size() + 7U) / 8U);
    append_le16(out, static_cast<std::uint16_t>(words << 4U));
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

std::vector<std::uint8_t> variable_width_structural_payload(std::uint32_t bits_token, std::uint64_t rows) {
    const auto mini_block = build_mini_block_layout(bits_token, rows);
    if (mini_block.size() < 6U) {
        return mini_block;
    }
    // Matches IPC2Lance / Lance reference PageLayout for utf8/binary columns.
    std::vector<std::uint8_t> wrapped;
    wrapped.push_back(0x1aU);
    wrapped.push_back(0x08U);
    wrapped.push_back(0x12U);
    wrapped.push_back(0x06U);
    wrapped.push_back(0x0aU);
    wrapped.push_back(0x04U);
    wrapped.push_back(0x0aU);
    wrapped.push_back(0x02U);
    wrapped.push_back(0x08U);
    wrapped.push_back(bits_token);
    wrapped.insert(wrapped.end(),
                   mini_block.begin() + static_cast<std::ptrdiff_t>(
                                            mini_block_value_compression_prefix(bits_token)),
                   mini_block.end());
    return wrapped;
}

std::vector<std::uint8_t> page_layout_bytes(std::uint32_t bits_token, std::uint64_t rows, bool variable_width) {
    std::vector<std::uint8_t> page_layout;
    if (variable_width) {
        write_length_delimited(page_layout, 1, variable_width_structural_payload(bits_token, rows));
    } else {
        write_length_delimited(page_layout, 1, build_mini_block_layout(bits_token, rows));
    }

    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

std::vector<std::uint8_t> page_layout_bytes(const LanceField& field, std::uint64_t rows) {
    return page_layout_bytes(flat_bits_per_value(field), rows, false);
}

// Variable-width structural payload whose value_compression is wrapped in General(ZSTD), so the
// chunk's value buffer is interpreted as [u64 LE uncompressed size][zstd frame]. Byte layout mirrors
// what lance 7.0 emits for a zstd variable-width column (see memory: lance-zstd-variable-encoding).
std::vector<std::uint8_t> variable_width_structural_payload_zstd(std::uint8_t bits_token, std::uint64_t rows) {
    // Uncompressed variable CompressiveEncoding body: f2 Variable{ f1 offsets = Flat{ f1 bits } }.
    const std::vector<std::uint8_t> inner_ce{0x12, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08, bits_token};
    // General{ f1 BufferCompression{ f1 scheme = ZSTD(2) }, f3 values = inner_ce }.
    std::vector<std::uint8_t> general{0x0a, 0x02, 0x08, 0x02, 0x1a, static_cast<std::uint8_t>(inner_ce.size())};
    general.insert(general.end(), inner_ce.begin(), inner_ce.end());
    // value_compression CompressiveEncoding{ f10 General }.
    std::vector<std::uint8_t> value_comp{0x52, static_cast<std::uint8_t>(general.size())};
    value_comp.insert(value_comp.end(), general.begin(), general.end());
    // MiniBlockLayout f3 = value_compression, followed by the unchanged f6/f7/f9/f10 tail.
    std::vector<std::uint8_t> out{0x1a, static_cast<std::uint8_t>(value_comp.size())};
    out.insert(out.end(), value_comp.begin(), value_comp.end());
    const auto mini = build_mini_block_layout(bits_token, rows);
    out.insert(out.end(),
               mini.begin() + static_cast<std::ptrdiff_t>(mini_block_value_compression_prefix(bits_token)),
               mini.end());
    return out;
}

std::vector<std::uint8_t> page_layout_bytes_variable_zstd(std::uint8_t bits_token, std::uint64_t rows) {
    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 1, variable_width_structural_payload_zstd(bits_token, rows));
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
std::vector<std::uint8_t> fixed_width_structural_payload_bss_zstd(std::uint8_t bits_token, std::uint64_t rows) {
    const std::vector<std::uint8_t> flat_ce{0x0a, 0x02, 0x08, bits_token};
    std::vector<std::uint8_t> bss{0x0a, static_cast<std::uint8_t>(flat_ce.size())};
    bss.insert(bss.end(), flat_ce.begin(), flat_ce.end());
    std::vector<std::uint8_t> bss_ce{0x4a, static_cast<std::uint8_t>(bss.size())};
    bss_ce.insert(bss_ce.end(), bss.begin(), bss.end());
    std::vector<std::uint8_t> general{0x0a, 0x02, 0x08, 0x02, 0x1a, static_cast<std::uint8_t>(bss_ce.size())};
    general.insert(general.end(), bss_ce.begin(), bss_ce.end());
    std::vector<std::uint8_t> value_comp{0x52, static_cast<std::uint8_t>(general.size())};
    value_comp.insert(value_comp.end(), general.begin(), general.end());
    std::vector<std::uint8_t> out{0x1a, static_cast<std::uint8_t>(value_comp.size())};
    out.insert(out.end(), value_comp.begin(), value_comp.end());
    const auto mini = build_mini_block_layout(bits_token, rows);
    out.insert(out.end(),
               mini.begin() + static_cast<std::ptrdiff_t>(mini_block_value_compression_prefix(bits_token)),
               mini.end());
    return out;
}

std::vector<std::uint8_t> page_layout_bytes_bss_zstd(std::uint8_t bits_token, std::uint64_t rows) {
    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 1, fixed_width_structural_payload_bss_zstd(bits_token, rows));
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
std::vector<std::uint8_t> build_dict_variable_block(const std::vector<std::string_view>& distinct) {
    const std::size_t n = distinct.size();
    const auto bytes_start_offset = static_cast<std::uint32_t>(8U + (n + 1U) * 4U);
    std::vector<std::uint8_t> out;
    append_le32(out, 32U);                 // bits_per_offset
    append_le32(out, bytes_start_offset);  // where the data bytes start
    std::uint32_t cum = 0;
    append_le32(out, 0U);  // offset[0]
    for (const auto& s : distinct) {
        cum += static_cast<std::uint32_t>(s.size());
        append_le32(out, cum);
    }
    for (const auto& s : distinct) {
        out.insert(out.end(), s.begin(), s.end());
    }
    return out;
}

// PageLayout for structural dictionary with flat bitpacked u32 indices (matches stock Lance for
// scattered low-cardinality strings): value_compression = InlineBitpacking(32), dictionary =
// Variable+Flat(32) without general compression, num_buffers=1, has_large_chunk=false.
std::vector<std::uint8_t> page_layout_bytes_dict(std::uint32_t num_distinct, std::uint64_t num_items) {
    static const std::uint8_t kF3Bitpack[] = {0x1a, 0x04, 0x2a, 0x02, 0x08, 0x20};
    static const std::uint8_t kF4Dict[] = {0x22, 0x08, 0x12, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x20};
    std::vector<std::uint8_t> structural(kF3Bitpack, kF3Bitpack + sizeof(kF3Bitpack));
    structural.insert(structural.end(), kF4Dict, kF4Dict + sizeof(kF4Dict));
    structural.push_back(0x28);  // f5 num_dictionary_items
    append_varint(structural, num_distinct);
    const auto tail = miniblock_tail(num_items, 1U, false);
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
std::vector<std::uint8_t> page_layout_bytes_dict_rle(std::uint32_t num_distinct, std::uint64_t num_items) {
    static const std::uint8_t kF3Rle[] = {0x1a, 0x0e, 0x42, 0x0c, 0x0a, 0x04, 0x0a, 0x02,
                                          0x08, 0x20, 0x12, 0x04, 0x0a, 0x02, 0x08, 0x08};
    static const std::uint8_t kF4Dict[] = {0x22, 0x10, 0x52, 0x0e, 0x0a, 0x02, 0x08, 0x02, 0x1a,
                                           0x08, 0x12, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x20};
    std::vector<std::uint8_t> structural(kF3Rle, kF3Rle + sizeof(kF3Rle));
    structural.insert(structural.end(), kF4Dict, kF4Dict + sizeof(kF4Dict));
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
        constant_layout.push_back(0x32);  // f6 inline_value
        constant_layout.push_back(static_cast<std::uint8_t>(inline_value->size()));
        constant_layout.insert(constant_layout.end(), inline_value->begin(), inline_value->end());
    }
    std::vector<std::uint8_t> page_layout;
    write_length_delimited(page_layout, 2, constant_layout);  // PageLayout f2 = constant_layout
    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

// MiniBlockLayout PageLayout advertising InlineBitpacking{uncompressed_bits_per_value}. Matches lance
// output (CompressiveEncoding f5 = inline_bitpacking). See memory: lance-inline-bitpacking-format.
std::vector<std::uint8_t> page_layout_bytes_inline_bitpacking(std::uint8_t uncompressed_bits, std::uint64_t rows) {
    // value_compression CompressiveEncoding{ f5 InlineBitpacking{ f1 uncompressed_bits_per_value } }.
    const std::vector<std::uint8_t> ce{0x2a, 0x02, 0x08, uncompressed_bits};
    std::vector<std::uint8_t> structural{0x1a, static_cast<std::uint8_t>(ce.size())};  // MiniBlockLayout f3
    structural.insert(structural.end(), ce.begin(), ce.end());
    const auto mini = build_mini_block_layout(0x40U, rows);  // token irrelevant; reuse the f6/f7/f9/f10 tail
    structural.insert(structural.end(),
                      mini.begin() + static_cast<std::ptrdiff_t>(mini_block_value_compression_prefix(0x40U)),
                      mini.end());

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
                           std::string& error) {
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
            const auto num_values = (last_value + 1U) - first_value;
            const auto data_bytes = static_cast<std::size_t>(offsets[last_value + 1U] - offsets[first_value]);
            const auto packed = padded_size((num_values + 1U) * offset_width + data_bytes, 8U);
            if (packed > kMaxVariableMiniblockBytes) {
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
                                      std::string& error) {
    if (column.large) {
        return build_variable_chunks<std::int64_t>(column, chunks, error);
    }
    return build_variable_chunks<std::int32_t>(column, chunks, error);
}

}  // namespace

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

        // Constant column (tagged by the writer): ConstantLayout. The single value comes from the
        // field metadata. Fixed-width stores it inline (zero data buffers); variable-width stores it
        // in one data buffer.
        const auto packing_it = field.metadata.find("nanolance:packing");
        if (packing_it != field.metadata.end() && packing_it->second == "constant") {
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
        if (packing_it != field.metadata.end() && packing_it->second == "rle" &&
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
        if (packing_it != field.metadata.end() && packing_it->second == "dict-rle" &&
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
                    const auto [it, inserted] = dict.emplace(val, id);
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
            if (!zstd_frame_buffer(build_dict_variable_block(distinct), compression_level, dict_frame, error)) {
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
            page.encoding = page_layout_bytes_dict_rle(static_cast<std::uint32_t>(distinct.size()), rows);
            column.pages.push_back(std::move(page));
            columns.push_back(std::move(column));
            continue;
        }

        // Structural dictionary for scattered low-cardinality strings: flat bitpacked u32 indices in
        // buffer[1], uncompressed dictionary variable block in buffer[2].
        if (packing_it != field.metadata.end() && packing_it->second == "dict" &&
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
                    const auto [it, inserted] = dict.emplace(val, id);
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
            const auto dict_block = build_dict_variable_block(distinct);

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
            page.encoding = page_layout_bytes_dict(static_cast<std::uint32_t>(distinct.size()), rows);
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
        // buffer. The generic path below would copy the values into a MiniblockChunk and then again
        // into a payload vector; for a plain fixed-width column the chunk bytes are just a slice of
        // values.fixed, so both copies are pure overhead (memcpy dominates this path after the chunk
        // count was reduced). One chunk per page, byte-identical to the generic flat encoding.
        if (!is_variable && !bitpack && !bss_zstd && !bool_pack) {
            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            const auto total = values.fixed_size() / fixed_bytes_per_value;
            const auto max_chunk_values = max_values_per_uncompressed_chunk(fixed_bytes_per_value);
            // Every full chunk of a column produces IDENTICAL page-encoding bytes (only the row-count
            // varint differs, and full chunks all carry max_chunk_values rows) -- build them once and
            // copy per page instead of re-encoding the whole protobuf tree per page.
            std::vector<std::uint8_t> full_chunk_encoding;
            for (std::size_t off = 0; off < total;) {
                const auto count = std::min(max_chunk_values, total - off);
                const auto chunk_bytes = count * fixed_bytes_per_value;
                const auto words = static_cast<std::uint16_t>((chunk_bytes + 7U) / 8U);
                const std::array<char, 4> control{static_cast<char>((words << 4U) & 0xFFU),
                                                  static_cast<char>(((words << 4U) >> 8U) & 0xFFU), 0, 0};

                align64(out);
                const auto control_offset = pos(out);
                out.write(control.data(), static_cast<std::streamsize>(control.size()));
                align64(out);
                const auto payload_offset = pos(out);
                const auto payload_size = stream_flat_miniblock_payload(
                    out, values.fixed_data() + off * fixed_bytes_per_value, chunk_bytes);

                pb::ColumnPage page;
                page.buffer_offsets.push_back(control_offset);
                page.buffer_offsets.push_back(payload_offset);
                page.buffer_sizes.push_back(control.size());
                page.buffer_sizes.push_back(payload_size);
                page.length = count;
                page.priority = 0;
                if (count == max_chunk_values) {
                    if (full_chunk_encoding.empty()) {
                        full_chunk_encoding = page_layout_bytes(field, count);
                    }
                    page.encoding = full_chunk_encoding;
                } else {
                    page.encoding = page_layout_bytes(field, count);
                }
                column.pages.push_back(std::move(page));
                off += count;
            }
            columns.push_back(std::move(column));
            continue;
        }

        // Bitpack / bool / byte-stream-split+zstd fixed-width columns: STREAM one chunk at a time
        // through hoisted, loop-reused scratch buffers (build chunk -> write control+payload -> reuse),
        // mirroring the flat path above. The previous two-phase shape (materialize every chunk into a
        // std::vector<MiniblockChunk>, then write) paid a fresh zero-initialized allocation per chunk
        // whose fill was immediately overwritten; with reuse, resize() touches nothing after the first
        // chunk since chunks are equal-sized. Output is byte-identical to the two-phase code:
        // stream_flat_miniblock_payload == miniblock_payload over a single chunk, and the 4-byte
        // control word below is the same single-chunk encoding control_buffer_for(chunk) produced.
        if (bitpack || bool_pack || bss_zstd) {
            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            const auto total = bool_pack ? values.fixed_size() : values.fixed_size() / fixed_bytes_per_value;
            const std::size_t step = bitpack      ? 1024U  // one FastLanes block per page
                                     : bool_pack ? kMaxBoolValuesPerChunk
                                                 : max_values_per_uncompressed_chunk(fixed_bytes_per_value);
            std::vector<std::uint8_t> scratch;  // built chunk bytes, reused across chunks
            std::vector<std::uint8_t> framed;   // zstd frame (bss-zstd only), reused across chunks
            // Every full chunk of a column produces IDENTICAL page-encoding bytes (only the row-count
            // varint differs, and full chunks all carry `step` rows) -- build once, copy per page.
            std::vector<std::uint8_t> full_chunk_encoding;
            auto build_page_encoding = [&](std::size_t count) {
                if (bitpack) {
                    return page_layout_bytes_inline_bitpacking(
                        static_cast<std::uint8_t>(fixed_bytes_per_value * 8U), count);
                }
                if (bool_pack) {
                    // Plain Flat{bits_per_value:1} -- no CompressiveEncoding wrapper, matching stock Lance.
                    return page_layout_bytes(flat_bits_per_value(field), count, false);
                }
                // byte-stream-split is restricted to 32/64-bit values, so the cast is safe.
                return page_layout_bytes_bss_zstd(static_cast<std::uint8_t>(flat_bits_per_value(field)), count);
            };
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
                const auto words = static_cast<std::uint16_t>((chunk_bytes.size() + 7U) / 8U);
                const std::array<char, 4> control{static_cast<char>((words << 4U) & 0xFFU),
                                                  static_cast<char>(((words << 4U) >> 8U) & 0xFFU), 0, 0};

                align64(out);
                const auto control_offset = pos(out);
                out.write(control.data(), static_cast<std::streamsize>(control.size()));
                align64(out);
                const auto payload_offset = pos(out);
                const auto payload_size =
                    stream_flat_miniblock_payload(out, chunk_bytes.data(), chunk_bytes.size());

                pb::ColumnPage page;
                page.buffer_offsets.push_back(control_offset);
                page.buffer_offsets.push_back(payload_offset);
                page.buffer_sizes.push_back(control.size());
                page.buffer_sizes.push_back(payload_size);
                page.length = count;
                page.priority = 0;
                if (count == step) {
                    if (full_chunk_encoding.empty()) {
                        full_chunk_encoding = build_page_encoding(count);
                    }
                    page.encoding = full_chunk_encoding;
                } else {
                    page.encoding = build_page_encoding(count);
                }
                column.pages.push_back(std::move(page));
                off += count;
            }
            columns.push_back(std::move(column));
            continue;
        }

        // Variable-width columns keep the two-phase build (chunks are unequal-sized, driven by the
        // offsets math in build_variable_chunks_for_column).
        if (!build_variable_chunks_for_column(values.variable, chunks, error)) {
            return false;
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
            page.encoding = zstd_variable ? page_layout_bytes_variable_zstd(bits_token, chunk.value_count)
                                          : page_layout_bytes(bits_token, chunk.value_count, true);
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
