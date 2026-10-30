// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/byte_sink.hpp"
#include "nanolance/data_file_writer.hpp"

#include "encoding/values/bitpack.hpp"
#include "encoding/values/general.hpp"
#include "encoding/values/byte_stream_split.hpp"
#include "encoding/values/dictionary.hpp"
#include "encoding/values/rle.hpp"
#include "encoding/values/scalar_value.hpp"
#include "encoding/layout/miniblock.hpp"
#include "encoding/layout/full_zip.hpp"
#include "nanolance/parallel.hpp"
#include "nanolance/work_stats.hpp"

#include "encoding/lance_descriptors.hpp"

#include <nanom/columnar_encode.hpp>

#include "lance_minimal.pb.hpp"
#include "nanolance/blob_v2_external.hpp"
#include "nanolance/bool_bitpack.hpp"
#include "nanolance/fsst.hpp"
#include "nanolance/repdef.hpp"
#include "nanolance/schema_mapper.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <streambuf>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <deque>
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

/// An output stream's buffer held in memory: where a column is encoded when columns are encoded in
/// parallel. tellp() reports bytes written, so pos()/align64() count from the column's own start.
class VectorStreambuf : public std::streambuf {
   public:
    const std::vector<char>& data() const { return bytes_; }
    void release() { std::vector<char>().swap(bytes_); }
    void reserve(std::size_t bytes) { bytes_.reserve(bytes); }

   protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        bytes_.insert(bytes_.end(), s, s + n);
        return n;
    }
    int_type overflow(int_type c) override {
        if (!traits_type::eq_int_type(c, traits_type::eof())) {
            bytes_.push_back(traits_type::to_char_type(c));
        }
        return traits_type::not_eof(c);
    }
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override {
        if (off == 0 && dir == std::ios_base::cur && (which & std::ios_base::out) != 0) {
            return pos_type(static_cast<off_type>(bytes_.size()));
        }
        return pos_type(off_type(-1));
    }

   private:
    std::vector<char> bytes_;
};

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
/// Fill the MiniBlockLayout fields every shape shares: one layer ([1], or [3] with a definition-level
/// layer -- how Lance announces one), `num_buffers`, `num_items`, has_large_chunk.
void mini_block_tail(descriptor::MiniBlock& m, std::uint64_t num_items, std::uint8_t num_buffers, bool nullable) {
    m.layers = {nullable ? std::uint8_t{3} : std::uint8_t{1}};
    m.num_buffers = num_buffers;
    m.num_items = num_items;
    m.has_large_chunk = true;
}

descriptor::MiniBlock build_mini_block_layout(std::uint32_t bits_per_value_token, std::uint64_t num_items,
                                             bool nullable = false, std::uint64_t fsl_items = 0) {
    // A fixed_size_list row is `fsl_items` flat values back to back. Lance describes that as
    // FixedSizeList{ items_per_value, values = Flat(element bits) } -- the wrapper is what tells a
    // reader it is a list at all.
    const std::uint64_t flat_bits = fsl_items != 0U ? bits_per_value_token / fsl_items : bits_per_value_token;
    descriptor::MiniBlock m;
    m.value_compression = fsl_items != 0U ? descriptor::fixed_size_list(fsl_items, descriptor::flat(flat_bits))
                                          : descriptor::flat(flat_bits);
    if (nullable) {
        m.def_compression = repdef_encoding_bytes();
    }
    mini_block_tail(m, num_items, 1U, nullable);
    return m;
}

/// page_layout_bytes for a flat page, with the nullable flag threaded through. `fsl_items` non-zero
/// wraps the values as a fixed_size_list of that many elements per row.
std::vector<std::uint8_t> page_layout_bytes_flat(std::uint32_t bits_token, std::uint64_t rows, bool nullable,
                                                 std::uint64_t fsl_items = 0) {
    return descriptor::page_encoding(build_mini_block_layout(bits_token, rows, nullable, fsl_items));
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
    const auto packed_words = ::nanom::columnar::fastlanes::packed_words_1024<std::uint16_t>(1);
    const std::size_t padding_cost = 1U * (1024U - count);
    const std::size_t pack_savings = (16U - 1U) * count;
    if (count < 1024U && padding_cost >= pack_savings) {
        std::vector<std::uint8_t> out(count * sizeof(std::uint16_t));
        std::memcpy(out.data(), levels, out.size());
        return out;
    }
    std::vector<std::uint16_t> packed(packed_words);
    ::nanom::columnar::fastlanes::pack_1024_unchecked<std::uint16_t>(1, levels, packed.data());
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
    // OutOfLineBitpacking{ uncompressed 16, values = Flat(1) }: a plain nullable column's levels.
    return descriptor::out_of_line_bitpacking(16U, descriptor::flat(1U));
}

// The chunk framing and the metadata words are miniblock's (encoding/layout/miniblock.hpp); these adapt it to
// MiniblockChunk. A page's chunks are padded to 8 bytes each, so consecutive chunks are contiguous.

/// Bytes `chunk` occupies in a page: header, levels and values, each padded to 8.
std::size_t chunk_footprint(const MiniblockChunk& chunk) {
    const std::size_t sizes[1] = {chunk.bytes.size()};
    return miniblock::chunk_bytes(chunk.repdef.size(), sizes);
}

std::vector<std::uint8_t> miniblock_payload(std::span<const MiniblockChunk> chunks) {
    std::vector<std::uint8_t> out;
    for (const auto& chunk : chunks) {
        miniblock::append_chunk(out, chunk.value_count, chunk.repdef, chunk.bytes);
    }
    return out;
}

/// The metadata buffer for `chunks`: every chunk but the last must hold a power of two values (Lance
/// derives the last one's count from the page's item count). The words are u32, matching has_large_chunk=1
/// in the page layout: stock Lance's v2_2 validate_page_layout refuses any mini-block page without the u32
/// chunk grammar, over the whole file's page table, so one u16 page made every column unreadable to it.
std::vector<std::uint8_t> control_buffer_for(std::span<const MiniblockChunk> chunks) {
    std::vector<miniblock::ChunkMeta> meta;
    meta.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        meta.push_back({chunk_footprint(chunk), chunk.value_count});
    }
    return miniblock::control_buffer(meta);
}

/// Stream one chunk straight to `out`: definition levels `def` (empty: none) covering `value_count` values,
/// then each buffer, with the header and every buffer padded to 8. Byte-identical to
/// miniblock::append_chunk, without first copying the value slices into a payload vector -- for a plain
/// fixed-width column a chunk's bytes are just a view into values.fixed. Returns the bytes written.
std::uint64_t stream_miniblock_chunk(std::ostream& out, std::size_t value_count, const std::vector<std::uint8_t>& def,
                                     const std::vector<std::pair<const std::uint8_t*, std::size_t>>& buffers) {
    static constexpr std::array<char, 8> fill{static_cast<char>(miniblock::kFillByte), static_cast<char>(miniblock::kFillByte),
                                              static_cast<char>(miniblock::kFillByte), static_cast<char>(miniblock::kFillByte),
                                              static_cast<char>(miniblock::kFillByte), static_cast<char>(miniblock::kFillByte),
                                              static_cast<char>(miniblock::kFillByte), static_cast<char>(miniblock::kFillByte)};
    std::vector<std::size_t> sizes;
    sizes.reserve(buffers.size());
    for (const auto& b : buffers) {
        sizes.push_back(b.second);
    }
    const auto header = miniblock::chunk_header(value_count, def.size(), sizes);
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    std::uint64_t written = header.size();
    const auto put = [&](const std::uint8_t* data, std::size_t size) {
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        const auto pad = miniblock::pad_bytes(size);
        out.write(fill.data(), static_cast<std::streamsize>(pad));
        written += size + pad;
    };
    if (!def.empty()) {
        put(def.data(), def.size());
    }
    for (const auto& b : buffers) {
        put(b.first, b.second);
    }
    return written;
}

/// One chunk of one value buffer, `def` empty for a column without nulls.
std::uint64_t stream_miniblock_chunk(std::ostream& out, std::size_t value_count, const std::vector<std::uint8_t>& def,
                                     const std::uint8_t* data, std::size_t bytes) {
    return stream_miniblock_chunk(out, value_count, def, {{data, bytes}});
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
/// The caller streams each chunk (stream_miniblock_chunk) and reports its footprint and value count here.
/// finish() then writes the control buffer: one u32 miniblock::control_word per chunk, the final chunk's
/// log2 nibble 0 -- Lance derives the last chunk's count from the page's item count, and every other chunk
/// must hold a power of two.
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
        append_le32(control_, miniblock::control_word(footprint, values, false));
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
        page.priority = first_row_;  // the pages before this one hold this many top-level rows
        first_row_ += rows_;
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
    std::uint64_t first_row_ = 0;  // top-level rows in the pages finished so far
    std::size_t chunks_ = 0;
};

std::vector<std::uint8_t> column_encoding_bytes() {
    return descriptor::column_encoding();
}

/// MiniBlockLayout for a variable-width (utf8/binary) column: the value buffer holds the offsets
/// followed by the bytes, so value_compression is CompressiveEncoding{ f2 Variable{ f1 offsets } }
/// rather than the Flat encoding a fixed-width column uses. Matches IPC2Lance / the Lance reference
/// PageLayout. `nullable` adds the f2 repdef_compression and the layers=[3] marker in the tail, in
/// the same places variable_width_structural_payload_zstd puts them.
descriptor::MiniBlock variable_width_structural_payload(std::uint32_t bits_token, std::uint64_t rows, bool nullable) {
    descriptor::MiniBlock m;
    m.value_compression = descriptor::variable(descriptor::flat(bits_token));
    if (nullable) {
        m.def_compression = repdef_encoding_bytes();
    }
    mini_block_tail(m, rows, 1U, nullable);
    return m;
}

std::vector<std::uint8_t> page_layout_bytes(std::uint32_t bits_token, std::uint64_t rows, bool variable_width,
                                            bool nullable = false) {
    return descriptor::page_encoding(variable_width ? variable_width_structural_payload(bits_token, rows, nullable)
                                                    : build_mini_block_layout(bits_token, rows, nullable));
}

// Variable-width structural payload whose value_compression is wrapped in General(ZSTD), so the
// chunk's value buffer is interpreted as [u64 LE uncompressed size][zstd frame]. Byte layout mirrors
// what lance 7.0 emits for a zstd variable-width column (see memory: lance-zstd-variable-encoding).
descriptor::MiniBlock variable_width_structural_payload_zstd(std::uint8_t bits_token, std::uint64_t rows,
                                                             bool nullable,
                                                             std::uint64_t general_scheme = descriptor::kZstd) {
    descriptor::MiniBlock m;
    m.value_compression = descriptor::general(general_scheme, descriptor::variable(descriptor::flat(bits_token)));
    if (nullable) {
        m.def_compression = repdef_encoding_bytes();
    }
    mini_block_tail(m, rows, 1U, nullable);
    return m;
}

// `general_scheme` is descriptor::kZstd (the default) or descriptor::kLz4 (`lance-encoding:compression=lz4`).
std::vector<std::uint8_t> page_layout_bytes_variable_zstd(std::uint8_t bits_token, std::uint64_t rows,
                                                          bool nullable = false,
                                                          std::uint64_t general_scheme = descriptor::kZstd) {
    return descriptor::page_encoding(variable_width_structural_payload_zstd(bits_token, rows, nullable, general_scheme));
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
// The same shape with LZ4 (`lance-encoding:compression=lz4`, pylance's measured layout for an int64 column):
// General{LZ4, ByteStreamSplit{Flat(64)}}. `general_scheme` is descriptor::kZstd or descriptor::kLz4.
descriptor::MiniBlock fixed_width_structural_payload_bss_general(std::uint8_t bits_token, std::uint64_t rows,
                                                                 bool nullable, std::uint64_t general_scheme) {
    descriptor::MiniBlock m;
    m.value_compression =
        descriptor::general(general_scheme, descriptor::byte_stream_split(descriptor::flat(bits_token)));
    if (nullable) {
        m.def_compression = repdef_encoding_bytes();
    }
    mini_block_tail(m, rows, 1U, nullable);
    return m;
}

std::vector<std::uint8_t> page_layout_bytes_bss_general(std::uint8_t bits_token, std::uint64_t rows,
                                                        std::uint64_t general_scheme, bool nullable = false) {
    return descriptor::page_encoding(
        fixed_width_structural_payload_bss_general(bits_token, rows, nullable, general_scheme));
}

void append_le32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

// MiniBlockLayout tail (f6 layers, f7 num_buffers, f9 num_items, f10 has_large_chunk).
void miniblock_tail(descriptor::MiniBlock& m, std::uint64_t num_items, std::uint8_t num_buffers,
                    bool has_large_chunk = true) {
    m.layers = {1U};
    m.num_buffers = num_buffers;
    m.num_items = num_items;
    m.has_large_chunk = has_large_chunk;
}

// PageLayout for a run-length-encoded fixed-width column: value_compression =
// Rle{ values=Flat(value_bits), run_lengths=Flat(length_bits) }, num_buffers=2.
std::vector<std::uint8_t> page_layout_bytes_rle(std::uint8_t value_bits, std::uint8_t length_bits,
                                                std::uint64_t num_items) {
    descriptor::MiniBlock m;
    m.value_compression = descriptor::rle(descriptor::flat(value_bits), descriptor::flat(length_bits));
    miniblock_tail(m, num_items, 2U);
    return descriptor::page_encoding(m);
}

/// The last byte of a Variable{offsets = Flat(bits)} node: the offset width.
std::uint8_t offset_bits_token(bool large) {
    return large ? static_cast<std::uint8_t>(0x40U) : static_cast<std::uint8_t>(0x20U);
}

// PageLayout for structural dictionary with flat bitpacked u32 indices (matches stock Lance for
// scattered low-cardinality strings): value_compression = InlineBitpacking(32), dictionary =
// Variable+Flat(32) without general compression, num_buffers=1, has_large_chunk=true (the chunk-meta
// words are u32 to match -- see control_buffer_for).
// `dict_scheme` 0 is the plain dictionary buffer; descriptor::kZstd / kLz4 wrap it in General (pylance's
// lance-encoding:dict-values-compression).
// `index_layout` is the indices' encoding: bit-packed (the default), Flat(32) under compression=none, or
// General{index_scheme, ByteStreamSplit{Flat(32)}} under compression=lz4|zstd (pylance's shape, W1r).
enum class DictIndexLayout { kBitpacked, kFlat, kGeneral };

std::vector<std::uint8_t> page_layout_bytes_dict(std::uint32_t num_distinct, std::uint64_t num_items, bool large,
                                                 std::uint64_t dict_scheme = 0U,
                                                 DictIndexLayout index_layout = DictIndexLayout::kBitpacked,
                                                 std::uint64_t index_scheme = descriptor::kZstd) {
    descriptor::MiniBlock m;
    switch (index_layout) {
        case DictIndexLayout::kBitpacked:
            m.value_compression = descriptor::inline_bitpacking(32U);
            break;
        case DictIndexLayout::kFlat:
            m.value_compression = descriptor::flat(32U);
            break;
        case DictIndexLayout::kGeneral:
            m.value_compression =
                descriptor::general(index_scheme, descriptor::byte_stream_split(descriptor::flat(32U)));
            break;
    }
    const auto plain = descriptor::variable(descriptor::flat(offset_bits_token(large)));
    m.dictionary = dict_scheme == 0U ? plain : descriptor::general(dict_scheme, plain);
    m.num_dictionary_items = num_distinct;
    miniblock_tail(m, num_items, 1U);
    return descriptor::page_encoding(m);
}

// PageLayout for a dictionary-encoded low-cardinality column: value_compression = Rle over u32
// indices, dictionary = General(ZSTD)+Variable, num_dictionary_items, num_buffers=2.
std::vector<std::uint8_t> page_layout_bytes_dict_rle(std::uint32_t num_distinct, std::uint64_t num_items,
                                                     bool large) {
    descriptor::MiniBlock m;
    m.value_compression = descriptor::rle(descriptor::flat(32U), descriptor::flat(8U));
    m.dictionary = descriptor::general(descriptor::kZstd, descriptor::variable(descriptor::flat(offset_bits_token(large))));
    m.num_dictionary_items = num_distinct;
    miniblock_tail(m, num_items, 2U);
    return descriptor::page_encoding(m);
}

// PageLayout = ConstantLayout. Fixed-width constants store the value inline in the descriptor (zero
// data buffers); variable-width (string/binary) constants omit inline_value and store the single
// value in one data buffer instead. Bytes match lance output.
std::vector<std::uint8_t> constant_layout_message(const std::vector<std::uint8_t>* inline_value) {
    descriptor::Constant c;
    c.layers = {1U};  // a single non-null layer
    c.inline_value = inline_value;
    return descriptor::page_encoding(c);
}

// PageLayout = ConstantLayout with a NULLABLE_ITEM layer and no value: Lance's spelling of a column
// whose every row is null. Zero data buffers. Byte-identical to what pylance writes for pa.nulls(n).
std::vector<std::uint8_t> all_null_constant_layout_message() {
    descriptor::Constant c;
    c.layers = {3U};  // NULLABLE_ITEM, no value
    return descriptor::page_encoding(c);
}

// MiniBlockLayout PageLayout advertising InlineBitpacking{uncompressed_bits_per_value}. Matches lance
// output (CompressiveEncoding f5 = inline_bitpacking). See memory: lance-inline-bitpacking-format.
std::vector<std::uint8_t> page_layout_bytes_inline_bitpacking(std::uint8_t uncompressed_bits, std::uint64_t rows,
                                                              bool nullable = false) {
    descriptor::MiniBlock m;
    m.value_compression = descriptor::inline_bitpacking(uncompressed_bits);
    if (nullable) {
        m.def_compression = repdef_encoding_bytes();
    }
    mini_block_tail(m, rows, 1U, nullable);
    return descriptor::page_encoding(m);
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
    return descriptor::out_of_line_bitpacking(16U, descriptor::flat(width));
}

/// Bit-pack `n` levels at `width`: whole 1024-level FastLanes blocks, then the tail either raw (u16
/// words) or padded to a full block -- whichever Lance's encoder would pick, because its decoder tells
/// the two apart by the buffer's length alone (the rule pack_definition_levels follows at width 1).
void pack_levels(const std::uint16_t* levels, std::size_t n, unsigned width, std::vector<std::uint8_t>& out) {
    const auto packed_words = ::nanom::columnar::fastlanes::packed_words_1024<std::uint16_t>(width);
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
        ::nanom::columnar::fastlanes::pack_1024_unchecked<std::uint16_t>(width, levels + at, packed.data());
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
            ::nanom::columnar::fastlanes::pack_1024_unchecked<std::uint16_t>(width, block, packed.data());
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

std::uint64_t write_buffer(std::ostream& out, const std::vector<std::uint8_t>& bytes) {
    align64(out);
    const auto at = pos(out);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return at;
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
            return descriptor::variable(descriptor::flat(offset_bits_token(large)));
        case ItemEncoding::kBitpacked:
            return descriptor::inline_bitpacking(lance_logical_type_value_bytes(field.logical_type) * 8U);
        case ItemEncoding::kBool:
        case ItemEncoding::kFlat: {
            // A fixed_size_list item, as Lance describes it: FixedSizeList{ items_per_value, values =
            // Flat(element bits) } -- not one wide Flat value.
            std::string element;
            std::uint64_t items = 0;
            if (encoding == ItemEncoding::kFlat && lance_fixed_size_list_parts(field.logical_type, element, items)) {
                return descriptor::fixed_size_list(items, descriptor::flat(lance_logical_type_value_bytes(element) * 8U));
            }
            return descriptor::flat(encoding == ItemEncoding::kBool ? 1U
                                                                    : lance_logical_type_value_bytes(field.logical_type) * 8U);
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
    // Appended to `out` (a page's payload, at a chunk's value section): gathering into a scratch
    // buffer that zero-filled first and was copied into the payload after was a third of a binary
    // column's write.
    const auto base = out.size();
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
        out.reserve(base + static_cast<std::size_t>(total) + 8U);
        out.resize(base + header);  // the offsets; the bytes are appended after them
        std::uint64_t at = header;
        const auto put = [&](std::size_t slot) {
            if (large) {
                std::memcpy(out.data() + base + slot * 8U, &at, 8U);
            } else {
                const auto narrow = static_cast<std::uint32_t>(at);
                std::memcpy(out.data() + base + slot * 4U, &narrow, 4U);
            }
        };
        put(0U);
        const auto* bytes = values.variable.data.data();
        bool consecutive = count != 0U;
        for (std::size_t k = first + 1U; k < first + count && consecutive; ++k) {
            consecutive = items[k] == items[k - 1U] + 1U;
        }
        if (consecutive) {
            // A flat column's chunk (and most lists'): its values are one run of the value buffer.
            const auto begin = offset(items[first]);
            for (std::size_t k = 0; k < count; ++k) {
                at = header + static_cast<std::uint64_t>(offset(items[first + k] + 1U) - begin);
                put(k + 1U);
            }
            const auto end = offset(items[first + count - 1U] + 1U);
            out.insert(out.end(), bytes + begin, bytes + end);
            pad8(out);
            return true;
        }
        for (std::size_t k = 0; k < count; ++k) {
            const auto begin = offset(items[first + k]);
            const auto end = offset(items[first + k] + 1U);
            if (end > begin) {
                out.insert(out.end(), bytes + begin, bytes + end);
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
        out.resize(base + (count + 7U) / 8U, 0U);
        for (std::size_t k = 0; k < count; ++k) {
            if (data[items[first + k]] != 0U) {
                out[base + (k >> 3U)] |= static_cast<std::uint8_t>(1U << (k & 7U));
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
        thread_local std::vector<std::uint8_t> packed;
        bitpack::encode_block(chunk, count, width, packed);
        out.insert(out.end(), packed.begin(), packed.end());
        return true;
    }
    out.insert(out.end(), chunk, chunk + count * width);
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
        // The sampled values' 64-bit hashes, sorted: equal values sit together as with the values themselves
        // (integer compares instead of string compares; a collision could only merge two runs).
        std::vector<std::size_t> sample(kSample);
        const auto stride = items.size() / kSample;
        const std::hash<std::string_view> hash;
        for (std::size_t k = 0; k < kSample; ++k) {
            sample[k] = hash(value_at(items[k * stride]));
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
    const auto limit = std::min<std::size_t>(kMaxDistinct, items.size() / 2U);
    // With threads to spare: each part of the items builds its own dictionary (first-appearance
    // order within the part), then the parts are merged in order -- which is exactly the whole page's
    // first-appearance order -- and the indices remapped. Declining when the distinct count passes
    // the limit is the same test on the merged count, since that count only grows.
    constexpr std::size_t kItemsPerPart = 16384U;
    const auto parts = std::min<std::size_t>(parallel::threads(), items.size() / kItemsPerPart);
    if (parts > 1U) {
        struct Part {
            std::unordered_map<std::string_view, std::uint32_t> ids;
            std::vector<std::string_view> distinct;
            std::uint64_t plain_bytes = 0;
            bool over = false;
        };
        std::vector<Part> part(parts);
        std::vector<std::size_t> first(parts + 1U);
        for (std::size_t k = 0; k <= parts; ++k) {
            first[k] = items.size() * k / parts;
        }
        indices.resize(items.size());
        parallel::for_each(parts, [&](std::size_t k) {
            auto& p = part[k];
            p.ids.reserve(4096U);
            for (auto j = first[k]; j < first[k + 1U]; ++j) {
                const auto value = value_at(items[j]);
                p.plain_bytes += value.size() + 4U;
                const auto [it, inserted] = p.ids.try_emplace(value, static_cast<std::uint32_t>(p.distinct.size()));
                if (inserted) {
                    p.distinct.push_back(value);
                    if (p.distinct.size() > limit) {
                        p.over = true;
                        return;
                    }
                }
                indices[j] = it->second;
            }
        });
        std::vector<std::vector<std::uint32_t>> remap(parts);
        for (std::size_t k = 0; k < parts; ++k) {
            if (part[k].over) {
                distinct.clear();
                indices.clear();
                return false;
            }
            plain_bytes += part[k].plain_bytes;
            remap[k].resize(part[k].distinct.size());
            for (std::size_t d = 0; d < part[k].distinct.size(); ++d) {
                const auto value = part[k].distinct[d];
                const auto [it, inserted] = ids.try_emplace(value, static_cast<std::uint32_t>(distinct.size()));
                if (inserted) {
                    distinct.push_back(value);
                    dict_bytes += value.size() + 4U;
                    if (distinct.size() > limit) {
                        distinct.clear();
                        indices.clear();
                        return false;
                    }
                }
                remap[k][d] = it->second;
            }
        }
        parallel::for_each(parts, [&](std::size_t k) {
            for (auto j = first[k]; j < first[k + 1U]; ++j) {
                indices[j] = remap[k][indices[j]];
            }
        });
    }
    if (parts <= 1U) {
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
    // A value with 7 readable bytes after it in the column's buffer is compressed without the zero-padded
    // copy of its last bytes (the same output; most of the work for short values).
    const auto* base = values.variable.data.data();
    const std::size_t readable = values.variable.data.size();
    const auto compress = [&](std::uint64_t begin, std::size_t size, std::uint8_t* to) {
        return static_cast<std::size_t>(begin) + size + 7U <= readable
                   ? fsst::compress_into_padded(encoder, base + begin, size, to)
                   : fsst::compress_into(encoder, base + begin, size, to);
    };
    // With threads to spare, the items in parts compressed side by side (a string column's write
    // was 70% here): part k compresses into its own worst-case region of the scratch -- twice the
    // raw bytes before it -- with offsets relative to that region, and a pass after closes the gaps
    // and rebases the offsets. The same bytes as one thread.
    constexpr std::size_t kItemsPerPart = 4096U;
    const auto parts = std::min<std::size_t>(parallel::threads(), items.size() / kItemsPerPart);
    std::size_t at = 0;
    if (parts <= 1U) {
        std::size_t slot = 0;
        for (const auto i : items) {
            const auto begin = variable_offset(values, i);
            const auto size = static_cast<std::size_t>(variable_offset(values, i + 1U) - begin);
            at += compress(begin, size, scratch.data() + at);
            put(++slot, at);
        }
    } else {
        std::vector<std::size_t> first(parts + 1U);   // items of part k: [first[k], first[k+1])
        std::vector<std::size_t> region(parts + 1U);  // where part k compresses to
        std::vector<std::size_t> written(parts);
        for (std::size_t k = 0; k <= parts; ++k) {
            first[k] = items.size() * k / parts;
        }
        std::size_t raw_before = 0;
        for (std::size_t k = 0; k < parts; ++k) {
            region[k] = 2U * raw_before;
            for (auto j = first[k]; j < first[k + 1U]; ++j) {
                raw_before += static_cast<std::size_t>(variable_offset(values, items[j] + 1U) -
                                                       variable_offset(values, items[j]));
            }
        }
        auto* dst = scratch.data();
        parallel::for_each(parts, [&](std::size_t k) {
            std::size_t local = 0;
            for (auto j = first[k]; j < first[k + 1U]; ++j) {
                const auto begin = variable_offset(values, items[j]);
                const auto size = static_cast<std::size_t>(variable_offset(values, items[j] + 1U) - begin);
                local += compress(begin, size, dst + region[k] + local);
                put(j + 1U, local);
            }
            written[k] = local;
        });
        for (std::size_t k = 0; k < parts; ++k) {
            if (region[k] != at) {
                std::memmove(dst + at, dst + region[k], written[k]);
            }
            for (auto j = first[k]; j < first[k + 1U]; ++j) {
                std::uint64_t local = 0;
                if (large) {
                    std::memcpy(&local, offsets + (j + 1U) * 8U, 8U);
                } else {
                    std::int32_t narrow = 0;
                    std::memcpy(&narrow, offsets + (j + 1U) * 4U, 4U);
                    local = static_cast<std::uint64_t>(narrow);
                }
                put(j + 1U, at + local);
            }
            at += written[k];
        }
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

/// The dictionary of an Arrow dictionary leaf under a struct or list (W1k2): every entry of the Arrow
/// dictionary, in its order, and each item's index into it. The values were recorded against that dictionary
/// when they were appended (array_accessor: append_dictionary_values), so every item has an entry.
bool plan_arrow_item_dictionary(const ColumnValues& values, const std::vector<std::uint64_t>& items,
                                std::vector<std::string_view>& distinct, std::vector<std::uint32_t>& indices) {
    distinct.clear();
    indices.clear();
    if (values.kind != ColumnValues::Kind::VariableWidth || values.dictionary == nullptr) {
        return false;
    }
    std::unordered_map<std::string_view, std::uint32_t> position;
    for (const auto& entry : *values.dictionary) {
        position.emplace(std::string_view(entry), static_cast<std::uint32_t>(distinct.size()));
        distinct.emplace_back(entry);
    }
    indices.reserve(items.size());
    for (const auto item : items) {
        if (!validity_bit(values.validity, item)) {
            // A null item holds an empty value, which is no entry: its index is never read (its level says null).
            indices.push_back(0U);
            continue;
        }
        const auto s = static_cast<std::size_t>(variable_offset(values, item));
        const auto e = static_cast<std::size_t>(variable_offset(values, item + 1U));
        const std::string_view value(reinterpret_cast<const char*>(values.variable.data.data()) + s, e - s);
        const auto found = position.find(value);
        if (found == position.end()) {
            distinct.clear();
            indices.clear();
            return false;
        }
        indices.push_back(found->second);
    }
    return true;
}

bool build_nested_page(const LanceField& field, const ColumnValues& values, const repdef::Serialized& ser,
                       std::uint64_t rows, const fsst::Encoder* fsst_encoder, bool plain, NestedPageBuffers& page,
                       bool& too_big, std::string& error) {
    constexpr std::size_t kValuesPerChunk = 1024U;  // 2^10: the log in every non-final chunk's word
    constexpr std::size_t kMaxChunkLevelBytes = 65535U;
    too_big = false;
    // Emptied, not replaced: the caller reuses one set across pages, and keeping the capacity saves
    // faulting a fresh payload in for every page.
    page.control.clear();
    page.payload.clear();
    page.dictionary.clear();
    page.rep_index.clear();
    page.descriptor.clear();
    const auto encoding = item_encoding_for(field, values);
    std::vector<std::string_view> distinct;
    std::vector<std::uint32_t> indices;
    // `plain`: the caller asked for no structural encoding (set_structural_encoding(false)), so the
    // values are written as they are -- no page dictionary here, and no FSST (it passes none).
    // An Arrow dictionary leaf (W1k2) keeps its own dictionary, in its order and with its unused entries, as
    // pylance writes it; its items are looked up in that dictionary. Any other leaf may get a page dictionary.
    const bool arrow_dictionary = !field.dictionary_index_format.empty();
    const bool dictionary =
        arrow_dictionary ? (values.kind == ColumnValues::Kind::VariableWidth && values.dictionary != nullptr &&
                            plan_arrow_item_dictionary(values, ser.items, distinct, indices))
                         : !plain && encoding == ItemEncoding::kVariable &&
                               plan_item_dictionary(values, ser.items, distinct, indices);
    if (arrow_dictionary && !dictionary) {
        error = "dictionary column '" + field.name + "' inside a struct or list has no dictionary to write";
        return false;
    }
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
    // Null elements of a fixed_size_list leaf (W1k2): each chunk carries its items' element validity bits as a
    // second buffer ahead of the values, as at the top level (write_fixed_size_list_column) and as pylance writes
    // it under a list or struct: FixedSizeList{N x Flat, validity}, two buffers per chunk.
    const bool element_validity = !values.item_validity.empty();
    std::string fsl_element;
    std::uint64_t fsl_items = 0;
    if (element_validity && (!lance_fixed_size_list_parts(field.logical_type, fsl_element, fsl_items) || fsl_items == 0U)) {
        error = "element validity on a column that is not a fixed_size_list: " + field.name;
        return false;
    }
    std::vector<std::uint8_t> element_bits;
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
    {
        // The payload in one allocation: values (an upper bound for bit-packed ones), levels, and a
        // chunk header and padding apiece. Growing it chunk by chunk re-copied it every doubling.
        std::size_t estimate = num_chunks * 64U + (ser.has_rep ? num_levels * 2U : 0U) +
                               (ser.has_def ? num_levels * 2U : 0U);
        if (encoding == ItemEncoding::kVariable && !dictionary) {
            const auto& v = fsst ? fsst_values : values;
            const auto& its = fsst ? fsst_items : ser.items;
            const bool large = v.variable.large;
            const auto* raw = v.variable.offsets.data();
            const auto entries = v.variable.offsets.size() / (large ? 8U : 4U);
            const auto at = [&](std::uint64_t i) -> std::uint64_t {
                if (large) {
                    std::uint64_t x = 0;
                    std::memcpy(&x, raw + i * 8U, 8U);
                    return x;
                }
                std::uint32_t x = 0;
                std::memcpy(&x, raw + i * 4U, 4U);
                return x;
            };
            for (const auto item : its) {
                if (item + 1U < entries) {
                    estimate += static_cast<std::size_t>(at(item + 1U) - at(item)) + (large ? 8U : 4U);
                }
            }
        } else if (encoding != ItemEncoding::kBool) {
            estimate += ser.items.size() * std::max<std::size_t>(4U, lance_logical_type_value_bytes(field.logical_type));
        }
        page.payload.reserve(estimate);
    }
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
            bitpack::encode_block(reinterpret_cast<const std::uint8_t*>(indices.data() + item_at), values_here, 4U,
                                  value_bytes);
        }
        // Chunk: [u16 levels][u16 rep bytes][u16 def bytes][u32 value bytes] padded to 8, then each
        // buffer padded to 8. Values other than dictionary indices are gathered in place, after the
        // levels, and their size patched into the header.
        const auto chunk_start = page.payload.size();
        auto& chunk = page.payload;
        append_le16(chunk, static_cast<std::uint16_t>(ser.has_rep || ser.has_def ? chunk_levels : 0U));
        if (ser.has_rep) {
            append_le16(chunk, static_cast<std::uint16_t>(rep_bytes.size()));
        }
        if (ser.has_def) {
            append_le16(chunk, static_cast<std::uint16_t>(def_bytes.size()));
        }
        // With element validity the element bits are a buffer of their own, ahead of the values: the header then
        // holds the validity buffer's size and the values' size.
        if (element_validity) {
            element_bits.assign((values_here * fsl_items + 7U) / 8U, 0U);
            for (std::size_t k = 0; k < values_here * fsl_items; ++k) {
                const auto at = ser.items[item_at + k / fsl_items] * fsl_items + k % fsl_items;
                if ((values.item_validity[at >> 3U] >> (at & 7U)) & 1U) {
                    element_bits[k >> 3U] |= static_cast<std::uint8_t>(1U << (k & 7U));
                }
            }
        }
        const auto value_size_at = chunk.size();
        append_le32(chunk, 0U);
        if (element_validity) {
            append_le32(chunk, 0U);
        }
        pad8(chunk);
        if (ser.has_rep) {
            chunk.insert(chunk.end(), rep_bytes.begin(), rep_bytes.end());
            pad8(chunk);
        }
        if (ser.has_def) {
            chunk.insert(chunk.end(), def_bytes.begin(), def_bytes.end());
            pad8(chunk);
        }
        if (element_validity) {
            chunk.insert(chunk.end(), element_bits.begin(), element_bits.end());
            pad8(chunk);
        }
        const auto values_at = chunk.size();
        if (dictionary) {
            chunk.insert(chunk.end(), value_bytes.begin(), value_bytes.end());
        } else if (!gather_chunk_values(field, fsst ? fsst_values : values, encoding, fsst ? fsst_items : ser.items,
                                        item_at, values_here, chunk, error)) {
            return false;
        }
        const auto value_size = static_cast<std::uint32_t>(chunk.size() - values_at);
        if (element_validity) {
            const auto bits_size = static_cast<std::uint32_t>(element_bits.size());
            std::memcpy(chunk.data() + value_size_at, &bits_size, 4U);  // little-endian, as append_le32
            std::memcpy(chunk.data() + value_size_at + 4U, &value_size, 4U);
        } else {
            std::memcpy(chunk.data() + value_size_at, &value_size, 4U);  // little-endian, as append_le32
        }
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

    descriptor::MiniBlock mini;
    if (ser.has_rep) {
        mini.rep_compression = levels_encoding(rep_width);
        mini.repetition_index_depth = 1U;
    }
    if (ser.has_def) {
        mini.def_compression = levels_encoding(def_width);
    }
    if (dictionary) {
        // Indices: InlineBitpacking(32). Dictionary: Variable{Flat(32 or 64)}, stored raw -- the same
        // block the flat string path writes (dictionary::encode_variable).
        const bool large = values.variable.large;
        mini.value_compression = descriptor::inline_bitpacking(32U);
        mini.num_dictionary_items = distinct.size();
        if (arrow_dictionary) {
            // pylance's shape for a dictionary under a struct or list: the dictionary LZ4-framed as one block.
            const auto raw = dictionary::encode_variable(distinct, large);
            if (!general::lz4_frame(raw, page.dictionary, error)) {
                return false;
            }
            mini.dictionary = descriptor::general(descriptor::kLz4, descriptor::variable(descriptor::flat(offset_bits_token(large))));
        } else {
            mini.dictionary = descriptor::variable(descriptor::flat(offset_bits_token(large)));
            page.dictionary = dictionary::encode_variable(distinct, large);
        }
    } else if (fsst) {
        mini.value_compression =
            descriptor::fsst(fsst_table, item_value_encoding(encoding, field, values.variable.large));
    } else if (element_validity) {
        mini.value_compression =
            descriptor::fixed_size_list(fsl_items, descriptor::flat(static_cast<std::uint32_t>(lance_logical_type_value_bytes(fsl_element) * 8U)), true);
    } else {
        mini.value_compression = item_value_encoding(encoding, field, values.variable.large);
    }
    mini.layers.assign(ser.layers.begin(), ser.layers.end());
    mini.num_buffers = element_validity ? 2U : 1U;
    mini.num_items = ser.items.size();
    mini.has_large_chunk = true;
    page.descriptor = descriptor::page_encoding(mini);
    return true;
}

bool write_nested_column(std::ostream& out, const LanceField& field, const ColumnValues& values, std::uint64_t rows,
                         pb::ColumnMetadata& column, std::string& error, const fsst::Encoder* fsst_encoder = nullptr,
                         bool plain = false) {
    // One FSST table for the whole column, used by every page it pays on.
    fsst::Encoder trained;
    if (plain) {
        fsst_encoder = nullptr;
    } else if (fsst_encoder == nullptr && train_column_fsst(field, values, trained)) {
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
    // working set, so keep a page to a few MiB of values and a bounded number of rows -- and, when
    // rows are large (a waveform, a long token list), to 1 MiB. Measured on Speech Commands (32 KiB
    // rows): one-row pages cost Rust Lance a page per row (a shuffled epoch 1.4 s, a scan 0.8 s);
    // 8 MiB pages cost nanolance's own scan 2x in page-sized intermediates; 1 MiB costs neither
    // (Rust 0.5 s / 0.58 s). take() reads only the chunks holding its rows, whatever the page size.
    // Each page's row count is sized from the bytes per row the previous page measured, instead of
    // trying 32,768 rows and halving: with 32 KiB rows that built ~1 GB before the first page fit.
    constexpr std::size_t kMaxValueBytes = std::size_t{8} << 20U;
    constexpr std::size_t kLargeRowPageBytes = std::size_t{1} << 20U;
    constexpr std::size_t kLargeRowBytes = std::size_t{4} << 10U;
    constexpr std::uint64_t kMaxRowsPerPage = 32768U;
    column.encoding = column_encoding_bytes();
    std::uint64_t row = 0;
    std::uint64_t try_rows = 1024U;
    std::size_t page_bytes = kMaxValueBytes;
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
        page.priority = row;
        if (ser.items.empty()) {
            // No values at all -- every list empty or null. Lance writes a ConstantLayout with no
            // value and the levels as raw u16 buffers [rep, def]; so does this.
            descriptor::Constant constant;
            constant.layers.assign(ser.layers.begin(), ser.layers.end());
            constant.num_rep_values = ser.rep.size();
            constant.num_def_values = ser.def.size();
            std::vector<std::uint8_t> raw_rep;
            std::vector<std::uint8_t> raw_def;
            append_u16_levels(raw_rep, ser.rep);
            append_u16_levels(raw_def, ser.def);
            page.buffer_offsets.push_back(write_buffer(out, raw_rep));
            page.buffer_sizes.push_back(raw_rep.size());
            page.buffer_offsets.push_back(write_buffer(out, raw_def));
            page.buffer_sizes.push_back(raw_def.size());
            page.encoding = descriptor::page_encoding(constant);
        } else {
            bool too_big = false;
            const bool built_ok = build_nested_page(field, values, ser, n, fsst_encoder, plain, built, too_big, error);
            if (!built_ok && !too_big) {
                error = "column '" + field.name + "': " + error;
                return false;
            }
            const auto bytes_per_row = std::max<std::size_t>(1U, built.payload.size() / static_cast<std::size_t>(n));
            if (bytes_per_row >= kLargeRowBytes) {
                page_bytes = kLargeRowPageBytes;
            }
            if (too_big || (built.payload.size() > page_bytes && n > 1U)) {
                if (n == 1U) {
                    error = "column '" + field.name + "': row " + std::to_string(row) +
                            " holds more list entries than one page chunk can describe";
                    return false;
                }
                try_rows = too_big ? std::max<std::uint64_t>(1U, n / 2U)
                                   : std::clamp<std::uint64_t>(page_bytes / bytes_per_row, 1U, n - 1U);
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
        // The next page: as many rows as the one just built says fit, a little under the limit. (A
        // page of only empty or null lists has no payload to measure: keep the row count.)
        if (!ser.items.empty()) {
            const auto per_row = std::max<std::size_t>(1U, built.payload.size() / static_cast<std::size_t>(n));
            try_rows = std::clamp<std::uint64_t>(page_bytes * 7U / 8U / per_row, 1U, kMaxRowsPerPage);
        }
    }
    return true;
}

/// Values at least this big on average are written as FullZip pages (see write_full_zip_variable_column).
constexpr std::uint64_t kFullZipMinAverageValueBytes = 4096U;

/// A column of large values -- images, audio clips, documents -- as FullZip pages, the layout Lance
/// itself writes for them: each row's bytes stored whole, one after the other, behind a per-row index.
///
/// A row is [definition byte, when the page has nulls][length, 4 or 8 bytes][the value]; a null is
/// the definition byte alone. The page's second buffer is the repetition index -- rows + 1 byte
/// offsets where each row starts (Lance requires one for variable-width FullZip pages) -- which is what
/// lets a reader fetch one row without reading the page: a shuffled mini-batch of 64 images out of
/// thousands reads 64 images, not the column. Pages are cut at ~8 MiB of values, at least one row.
bool write_full_zip_variable_column(std::ostream& out, const ColumnValues& values, std::uint64_t rows,
                                    pb::ColumnMetadata& column, std::string& error) {
    constexpr std::size_t kPageBytes = std::size_t{8} << 20U;
    const bool large = values.variable.large;
    const std::size_t length_bytes = large ? 8U : 4U;
    column.encoding = column_encoding_bytes();
    std::vector<std::uint8_t> data;
    std::vector<std::uint64_t> starts;
    std::uint64_t row = 0;
    while (row < rows) {
        const auto first = row;
        std::uint64_t end = row;
        std::size_t bytes = 0;
        bool nulls = false;
        while (end < rows && (end == first || bytes < kPageBytes)) {
            if (validity_bit(values.validity, end)) {
                bytes += static_cast<std::size_t>(variable_offset(values, end + 1U) - variable_offset(values, end)) +
                         length_bytes;
            } else {
                nulls = true;
            }
            ++end;
        }
        data.clear();
        data.reserve(bytes + static_cast<std::size_t>(end - first));
        starts.clear();
        for (auto r = first; r < end; ++r) {
            starts.push_back(data.size());
            const bool valid = validity_bit(values.validity, r);
            const auto control = full_zip::control_bytes(0U, nulls ? 1U : 0U);
            if (!valid) {
                full_zip::append_level(data, control, 1U, nullptr, 0U, length_bytes);  // definition 1: a null item
                continue;
            }
            const auto begin = variable_offset(values, r);
            const auto len = static_cast<std::uint64_t>(variable_offset(values, r + 1U) - begin);
            if (!large && len > std::numeric_limits<std::uint32_t>::max()) {
                error = "a utf8/binary value over 4 GiB";
                return false;
            }
            static constexpr std::uint8_t kEmpty = 0U;
            full_zip::append_level(data, control, 0U, len != 0U ? values.variable.data.data() + begin : &kEmpty,
                                   static_cast<std::size_t>(len), length_bytes);
        }
        starts.push_back(data.size());
        const auto rep_index = full_zip::repetition_index(starts);

        descriptor::FullZip layout;
        layout.bits_def = nulls ? 1U : 0U;
        layout.bits_per_offset = length_bytes * 8U;
        layout.num_items = end - first;
        layout.num_visible_items = end - first;
        layout.value_compression = descriptor::variable(descriptor::flat(length_bytes * 8U));
        layout.layers = {nulls ? static_cast<std::uint8_t>(repdef::kNullableItem)
                               : static_cast<std::uint8_t>(repdef::kAllValidItem)};

        pb::ColumnPage page;
        page.buffer_offsets.push_back(write_buffer(out, data));
        page.buffer_sizes.push_back(data.size());
        page.buffer_offsets.push_back(write_buffer(out, rep_index));
        page.buffer_sizes.push_back(rep_index.size());
        page.length = end - first;
        page.priority = first;
        page.encoding = descriptor::page_encoding(layout);
        column.pages.push_back(std::move(page));
        row = end;
    }
    return true;
}

/// A Blob v2 column inside structs or lists, as Lance writes one: a FullZip page of the packed
/// descriptors, each level's control word (`rep << bits_def | def`) followed -- for a valid item --
/// by the item's descriptor (its u32 length, then kind..uri); a null item, and an empty or null list,
/// is the control word alone. The second buffer gives each row's end, as for a flat blob page.
bool write_nested_blob_column(std::ostream& out, const LanceField& field, const ColumnValues& values,
                              std::uint64_t rows, pb::ColumnMetadata& column, std::string& error) {
    std::vector<repdef::SerializeLayer> layers;
    for (const auto& layer : values.layers) {
        layers.push_back({layer.is_list, &layer.offsets, &layer.validity});
    }
    repdef::Serialized ser;
    if (!repdef::serialize(layers, values.validity, 0, rows, ser, error)) {
        error = "column '" + field.name + "': " + error;
        return false;
    }
    // Control word widths, from the deepest level the layers can express (Lance's choice, not the
    // deepest one these rows use).
    std::uint32_t max_def = 0;
    std::uint32_t max_rep = 0;
    for (const auto layer : ser.layers) {
        max_def += layer == repdef::kAllValidItem || layer == repdef::kAllValidList ? 0U
                   : layer == repdef::kNullAndEmptyList                             ? 2U
                                                                                     : 1U;
        max_rep += repdef::is_list_layer(layer) ? 1U : 0U;
    }
    const auto bits_for = [](std::uint32_t v) {
        std::uint32_t bits = 0;
        while (v != 0U) {
            ++bits;
            v >>= 1U;
        }
        return bits;
    };
    const auto bits_def = ser.has_def ? bits_for(max_def) : 0U;
    const auto bits_rep = ser.has_rep ? bits_for(max_rep) : 0U;
    const std::size_t control_bytes = full_zip::control_bytes(bits_rep, bits_def);
    const auto levels = ser.has_rep || ser.has_def ? std::max(ser.rep.size(), ser.def.size())
                                                    : static_cast<std::size_t>(ser.items.size());

    // Each item's descriptor row, by item index.
    std::vector<std::size_t> starts(values.blob_v2.row_packed_sizes.size() + 1U, 0U);
    for (std::size_t i = 0; i < values.blob_v2.row_packed_sizes.size(); ++i) {
        starts[i + 1U] = starts[i] + values.blob_v2.row_packed_sizes[i];
    }
    std::vector<std::uint8_t> data;
    data.reserve(values.blob_v2.packed_payload.size() + levels * control_bytes);
    std::vector<std::uint32_t> row_sizes;
    std::size_t row_start = 0;
    std::size_t slot = 0;
    for (std::size_t level = 0; level < levels; ++level) {
        const std::uint32_t rep = ser.has_rep ? ser.rep[level] : 0U;
        const std::uint32_t def = ser.has_def ? ser.def[level] : 0U;
        if (level != 0U && (!ser.has_rep || rep == max_rep)) {
            row_sizes.push_back(static_cast<std::uint32_t>(data.size() - row_start));  // a new row starts
            row_start = data.size();
        }
        full_zip::append_level(data, control_bytes, (rep << bits_def) | def, nullptr, 0U, 0U);
        const bool has_slot = ser.is_slot.empty() || ser.is_slot[level] != 0U;
        if (!has_slot) {
            continue;  // an empty or null list
        }
        const auto item = static_cast<std::size_t>(ser.items[slot++]);
        if (def != 0U) {
            continue;  // a null item, or a null struct above it
        }
        if (item + 1U >= starts.size() || starts[item + 1U] == starts[item]) {
            error = "column '" + field.name + "': a valid blob item has no descriptor";
            return false;
        }
        data.insert(data.end(), values.blob_v2.packed_payload.begin() + static_cast<std::ptrdiff_t>(starts[item]),
                    values.blob_v2.packed_payload.begin() + static_cast<std::ptrdiff_t>(starts[item + 1U]));
    }
    if (levels != 0U) {
        row_sizes.push_back(static_cast<std::uint32_t>(data.size() - row_start));
    }
    if (row_sizes.size() != rows) {
        error = "column '" + field.name + "': its levels make " + std::to_string(row_sizes.size()) + " rows, not " +
                std::to_string(rows);
        return false;
    }
    const auto control = blob_v2_build_control_buffer(row_sizes);
    column.encoding = column_encoding_bytes();
    pb::ColumnPage page;
    align64(out);
    page.buffer_offsets.push_back(pos(out));
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    page.buffer_sizes.push_back(data.size());
    align64(out);
    page.buffer_offsets.push_back(pos(out));
    out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));
    page.buffer_sizes.push_back(control.size());
    page.length = rows;
    page.priority = 0;
    page.encoding = blob_v2_descriptor_page_encoding(bits_rep, bits_def, levels, slot,
                                                     std::vector<std::uint8_t>(ser.layers.begin(), ser.layers.end()));
    column.pages.push_back(std::move(page));
    return true;
}

namespace {

/// The state the steps of write_lance_data_file share: the physical fields, each column's values and the
/// values it is written from (a blob column's placed copy), and the row count and compression options.
struct FileWriteState {
    const std::vector<const LanceField*>& physical_fields;
    const std::vector<ColumnValues>& column_values;
    std::vector<const ColumnValues*> column_source;
    std::vector<ColumnValues> placed_blobs;
    std::uint64_t rows;
    int compression_level;
    bool compress;
};

/// One column's inputs for the per-encoding writers below: its field and values, the row count, what
/// encode_column has already decided about it (its compression and its nulls), and its nanolance:packing tag.
/// The per-column general compression a field asks for with `lance-encoding:compression` (pylance's
/// option). kAbsent: the field says nothing, and nanolance chooses as it always has.
enum class ColumnCodec { kAbsent, kUncompressed, kLz4, kZstd };

struct ColumnEncodeContext {
    const LanceField& field;
    const ColumnValues& values;
    std::uint64_t rows;
    int compression_level;
    bool compress;
    bool zstd_requested;
    bool compress_column;
    bool column_has_nulls;
    decltype(LanceField::metadata)::const_iterator packing_it;
    ColumnCodec codec;
};

/// The fixed-width encoding picked for a column: bit-packed, bool, byte-stream-split + zstd, or flat.
struct FixedWidthChoice {
    bool bitpack;
    bool bool_pack;
    bool bss_general;  // byte-stream-split, then General(general_scheme) per chunk
    std::uint64_t general_scheme;  // descriptor::kZstd or descriptor::kLz4 (bss_general only)
    std::size_t bytes_per_value;
};

/// Writes a blob-v2 column: a nested blob column with its layers, or packed blob descriptions.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_blob_v2_column(const ColumnEncodeContext& ctx, std::ostream& out, std::vector<pb::ColumnMetadata>& columns,
                          std::string& error) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const auto rows = ctx.rows;
    const bool column_has_nulls = ctx.column_has_nulls;

    if (values.kind == ColumnValues::Kind::BlobV2External && !values.layers.empty()) {
        pb::ColumnMetadata column;
        if (!write_nested_blob_column(out, field, values, rows, column, error)) {
            return false;
        }
        columns.push_back(std::move(column));
        return true;
    }
    const auto& blob = values.blob_v2;
    if (blob.row_packed_sizes.size() != static_cast<std::size_t>(rows)) {
        error = "blob v2 row count does not match fragment row count for ";
        error += field.name;
        return false;
    }
    // With nulls, each row starts with its definition level (1: null), a null row being that
    // byte alone -- Lance's FullZip with one definition layer.
    const bool nullable = column_has_nulls;
    std::vector<std::uint8_t> with_levels;
    std::vector<std::uint32_t> level_sizes;
    if (nullable) {
        with_levels.reserve(blob.packed_payload.size() + blob.row_packed_sizes.size());
        level_sizes.reserve(blob.row_packed_sizes.size());
        std::size_t at = 0;
        for (std::size_t r = 0; r < blob.row_packed_sizes.size(); ++r) {
            const bool valid = validity_bit(values.validity, r);
            const auto n = blob.row_packed_sizes[r];
            with_levels.push_back(valid ? 0U : 1U);
            if (valid) {
                with_levels.insert(with_levels.end(), blob.packed_payload.begin() + static_cast<std::ptrdiff_t>(at),
                                   blob.packed_payload.begin() + static_cast<std::ptrdiff_t>(at + n));
            }
            at += n;
            level_sizes.push_back(valid ? n + 1U : 1U);
        }
    }
    const auto& payload = nullable ? with_levels : blob.packed_payload;
    const auto control = blob_v2_build_control_buffer(nullable ? level_sizes : blob.row_packed_sizes);

    align64(out);
    const auto values_offset = pos(out);
    out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));

    align64(out);
    const auto control_offset = pos(out);
    out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));

    pb::ColumnMetadata column;
    column.encoding = column_encoding_bytes();
    pb::ColumnPage page;
    page.buffer_offsets.push_back(values_offset);
    page.buffer_offsets.push_back(control_offset);
    page.buffer_sizes.push_back(payload.size());
    page.buffer_sizes.push_back(control.size());
    page.length = rows;
    page.priority = 0;
    page.encoding = blob_v2_descriptor_page_encoding(rows, nullable);
    column.pages.push_back(std::move(page));
    columns.push_back(std::move(column));
    return true;
}

/// Writes an Arrow null column, or an empty struct: every row is null and no value is stored.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_all_null_column(const ColumnEncodeContext& ctx, std::vector<pb::ColumnMetadata>& columns) {
    const auto rows = ctx.rows;

    pb::ColumnMetadata column;
    column.encoding = column_encoding_bytes();
    pb::ColumnPage page;
    page.length = rows;
    page.priority = 0;
    page.encoding = all_null_constant_layout_message();
    column.pages.push_back(std::move(page));
    columns.push_back(std::move(column));
    return true;
}

/// Writes an Arrow dictionary column as Lance's dictionary page: the indices, and the dictionary.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_dictionary_column(const ColumnEncodeContext& ctx, std::ostream& out,
                             std::vector<pb::ColumnMetadata>& columns) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const auto rows = ctx.rows;
    const bool column_has_nulls = ctx.column_has_nulls;

    const bool variable = values.kind == ColumnValues::Kind::VariableWidth;
    const bool large = values.variable.large;
    const auto& format = field.dictionary_index_format;
    const std::size_t iw = format == "c" || format == "C" ? 1U : format == "s" || format == "S" ? 2U
                           : format == "i" || format == "I"                                  ? 4U
                                                                                             : 8U;
    const std::size_t width = variable ? 0U : lance_logical_type_value_bytes(field.logical_type);
    const auto value_at = [&](std::uint64_t r) -> std::string_view {
        if (!variable) {
            return {reinterpret_cast<const char*>(values.fixed_data() + r * width), width};
        }
        std::uint64_t s = 0;
        std::uint64_t e = 0;
        if (large) {
            std::memcpy(&s, values.variable.offsets.data() + r * 8U, 8U);
            std::memcpy(&e, values.variable.offsets.data() + (r + 1U) * 8U, 8U);
        } else {
            std::uint32_t s32 = 0;
            std::uint32_t e32 = 0;
            std::memcpy(&s32, values.variable.offsets.data() + r * 4U, 4U);
            std::memcpy(&e32, values.variable.offsets.data() + (r + 1U) * 4U, 4U);
            s = s32;
            e = e32;
        }
        return {reinterpret_cast<const char*>(values.variable.data.data() + s), static_cast<std::size_t>(e - s)};
    };
    std::vector<std::string> entries;
    if (values.dictionary != nullptr) {
        entries = *values.dictionary;
    }
    std::unordered_map<std::string_view, std::uint64_t> lookup;
    for (std::size_t k = 0; k < entries.size(); ++k) {
        lookup.emplace(std::string_view(entries[k]), k);
    }
    std::deque<std::string> extra;  // values the dictionary lacks (none, from an Arrow dictionary)
    std::vector<std::uint8_t> indices(static_cast<std::size_t>(rows) * iw, 0U);
    for (std::uint64_t r = 0; r < rows; ++r) {
        if (!values.validity.empty() && ((values.validity[r >> 3U] >> (r & 7U)) & 1U) == 0U) {
            continue;
        }
        const auto v = value_at(r);
        auto it = lookup.find(v);
        if (it == lookup.end()) {
            extra.emplace_back(v);
            it = lookup.emplace(std::string_view(extra.back()), entries.size() + extra.size() - 1U).first;
        }
        std::memcpy(indices.data() + r * iw, &it->second, iw);
    }
    // (A deque: `lookup` holds views into its strings, which must not move.)
    for (auto& e : extra) {
        entries.push_back(e);
    }
    std::vector<std::uint8_t> dict_block;
    std::vector<std::uint8_t> dict_encoding;
    if (variable) {
        std::vector<std::string_view> views(entries.begin(), entries.end());
        dict_block = dictionary::encode_variable(views, large);
        dict_encoding = descriptor::variable(descriptor::flat(offset_bits_token(large)));
    } else {
        dict_block = dictionary::encode_fixed(entries);
        dict_encoding = descriptor::flat(static_cast<std::uint32_t>(width * 8U));
    }
    const auto by_bytes = max_values_per_uncompressed_chunk(iw);
    const auto multi = multichunk_values(column_has_nulls ? std::min<std::size_t>(1024U, by_bytes) : by_bytes);
    const auto max_chunk = multi != 0U ? multi : std::size_t{1};
    pb::ColumnMetadata column;
    column.encoding = column_encoding_bytes();
    MiniblockPageWriter page_writer(out);
    for (std::size_t off = 0; off < rows;) {
        const auto count = std::min<std::size_t>(max_chunk, rows - off);
        const auto def = column_has_nulls ? pack_definition_levels(values.validity, off, count)
                                          : std::vector<std::uint8_t>{};
        page_writer.begin_chunk();
        const auto footprint = stream_miniblock_chunk(out, count, def, {{indices.data() + off * iw, count * iw}});
        page_writer.end_chunk(footprint, count);
        off += count;
    }
    pb::ColumnPage page;
    if (rows > 0U) {
        page_writer.finish(page);
    } else {
        page.length = 0;
    }
    align64(out);
    page.buffer_offsets.push_back(pos(out));
    page.buffer_sizes.push_back(dict_block.size());
    out.write(reinterpret_cast<const char*>(dict_block.data()), static_cast<std::streamsize>(dict_block.size()));
    descriptor::MiniBlock m;
    m.value_compression = descriptor::flat(static_cast<std::uint32_t>(iw * 8U));
    m.dictionary = dict_encoding;
    m.num_dictionary_items = entries.size();
    if (column_has_nulls) {
        m.def_compression = repdef_encoding_bytes();
    }
    mini_block_tail(m, rows, 1U, column_has_nulls);
    page.encoding = descriptor::page_encoding(m);
    column.pages.push_back(std::move(page));
    columns.push_back(std::move(column));
    return true;
}

/// Lance 12's rule for fixed-width values (`MINIBLOCK_MAX_BYTE_LENGTH_PER_VALUE` in primitive.rs): a column
/// whose values are this wide or wider is written as FullZip pages, a narrower one as mini-block chunks. The
/// width counts the values only; element validity is not in it (measured on pylance 12.0.0:
/// FixedSizeList{64 x float32, validity} is FullZip, FixedSizeList{63 x float32, validity} is mini-block).
constexpr std::uint64_t kFullZipMinFixedRowBytes = 256U;

/// The FullZip value encoding of a fixed-width column: a flat fixed-width value, or a fixed_size_list of one
/// (`FixedSizeList{N x Flat(b)}`, with `has_validity` when its items carry validity). False for a type this writer
/// does not lay out as FullZip: bool (not whole bytes as a list element), a list of lists, variable width.
bool fixed_full_zip_value_encoding(const LanceField& field, bool item_validity, std::vector<std::uint8_t>& encoding) {
    std::string element;
    std::uint64_t items = 0;
    if (lance_fixed_size_list_parts(field.logical_type, element, items)) {
        if (element == "bool" || element.rfind("fixed_size_list:", 0) == 0 || lance_field_is_variable_width(element)) {
            return false;
        }
        const auto element_bytes = lance_logical_type_value_bytes(element);
        if (element_bytes == 0U) {
            return false;
        }
        encoding = descriptor::fixed_size_list(items, descriptor::flat(static_cast<std::uint64_t>(element_bytes) * 8U),
                                               item_validity);
        return true;
    }
    if (field.logical_type == "bool" || lance_field_is_variable_width(field.logical_type)) {
        return false;
    }
    const auto bytes = lance_logical_type_value_bytes(field.logical_type);
    if (bytes == 0U) {
        return false;
    }
    encoding = descriptor::flat(static_cast<std::uint64_t>(bytes) * 8U);
    return true;
}

/// Whether a fixed-width column is written as FullZip pages. The default is Lance's rule (256 bytes or more per
/// value); `lance-encoding:structural-encoding=fullzip` forces FullZip on any type that can take it (a 64-bit
/// integer included) and `miniblock` keeps mini-block. A fullzip request this writer cannot lay out is refused by
/// name. A list of bools is refused with pylance's own message (measured); a bool column and a list of lists
/// pylance writes, which nanolance does not yet.
bool fixed_column_wants_full_zip(const LanceField& field, bool item_validity, bool& wanted, std::string& error) {
    wanted = false;
    const auto option = field.metadata.find("lance-encoding:structural-encoding");
    const std::string text = option == field.metadata.end() ? std::string() : option->second;
    if (text == "miniblock") {
        return true;
    }
    std::vector<std::uint8_t> encoding;
    if (fixed_full_zip_value_encoding(field, item_validity, encoding)) {
        wanted = text == "fullzip" || lance_logical_type_value_bytes(field.logical_type) >= kFullZipMinFixedRowBytes;
        return true;
    }
    if (text != "fullzip") {
        return true;  // the default rule: this type stays on its mini-block / flat path
    }
    error = "lance-encoding:structural-encoding=fullzip on column '" + field.name + "' of type " + field.logical_type +
            " is not written yet by nanolance";
    return false;
}

/// A fixed-width column as FullZip pages, the layout Lance writes for values of 256 bytes or more: each row
/// stored whole -- [control word when the column has nulls][the item validity bytes, when the items carry
/// validity][the row's values] -- and no repetition index, since every row has the same width. A null row keeps
/// its slot (zeros), as Lance's fixed-width FullZip does. Pages hold whole rows, cut at kFullZipFixedPageBytes.
bool write_full_zip_fixed_column(const ColumnEncodeContext& ctx, std::ostream& out,
                                 std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    constexpr std::uint64_t kFullZipFixedPageBytes = std::uint64_t{1} << 20U;
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const bool nulls = ctx.column_has_nulls;
    const bool item_validity = !values.item_validity.empty();
    std::string element;
    std::uint64_t items = 0;
    if (item_validity && !lance_fixed_size_list_parts(field.logical_type, element, items)) {
        error = "element validity on a column that is not a fixed_size_list: " + field.name;
        return false;
    }
    const std::size_t value_bytes = value_width_bytes(field);
    const std::size_t validity_bytes = item_validity ? static_cast<std::size_t>((items + 7U) / 8U) : 0U;
    const std::size_t row_bytes = validity_bytes + value_bytes;
    std::vector<std::uint8_t> encoding;
    if (value_bytes == 0U || !fixed_full_zip_value_encoding(field, item_validity, encoding)) {
        error = "column '" + field.name + "' cannot be written as FullZip pages";
        return false;
    }
    const std::size_t control = full_zip::control_bytes(0U, nulls ? 1U : 0U);
    const std::uint64_t rows = ctx.rows;
    const std::uint64_t per_page = std::max<std::uint64_t>(1U, kFullZipFixedPageBytes / (control + row_bytes));
    const std::vector<std::uint8_t> zeros(row_bytes, 0U);
    std::vector<std::uint8_t> row(row_bytes);
    pb::ColumnMetadata column;
    column.encoding = column_encoding_bytes();
    std::vector<std::uint8_t> data;
    for (std::uint64_t first = 0; first < rows;) {
        const auto end = std::min(rows, first + per_page);
        data.clear();
        data.reserve(static_cast<std::size_t>(end - first) * (control + row_bytes));
        for (auto r = first; r < end; ++r) {
            const bool valid = !nulls || validity_bit(values.validity, r);
            full_zip::append_level(data, control, valid ? 0U : 1U, nullptr, 0U, 0U);
            if (!valid) {
                data.insert(data.end(), zeros.begin(), zeros.end());
            } else if (!item_validity) {
                const auto* bytes = values.fixed_data() + static_cast<std::size_t>(r) * value_bytes;
                data.insert(data.end(), bytes, bytes + value_bytes);
            } else {
                // Element validity: bit j of the row is item j's bit, LSB-first, as mini-block writes it.
                std::fill(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(validity_bytes), 0U);
                for (std::uint64_t j = 0; j < items; ++j) {
                    const auto at = r * items + j;
                    if ((values.item_validity[at >> 3U] >> (at & 7U)) & 1U) {
                        row[j >> 3U] |= static_cast<std::uint8_t>(1U << (j & 7U));
                    }
                }
                const auto* bytes = values.fixed_data() + static_cast<std::size_t>(r) * value_bytes;
                std::copy(bytes, bytes + value_bytes, row.begin() + static_cast<std::ptrdiff_t>(validity_bytes));
                data.insert(data.end(), row.begin(), row.end());
            }
        }
        descriptor::FullZip layout;
        layout.bits_def = nulls ? 1U : 0U;
        layout.bits_per_value = static_cast<std::uint64_t>(row_bytes) * 8U;
        layout.num_items = end - first;
        layout.num_visible_items = end - first;
        layout.value_compression = encoding;
        layout.layers = {nulls ? static_cast<std::uint8_t>(repdef::kNullableItem)
                               : static_cast<std::uint8_t>(repdef::kAllValidItem)};
        pb::ColumnPage page;
        page.buffer_offsets.push_back(write_buffer(out, data));
        page.buffer_sizes.push_back(data.size());
        page.length = end - first;
        page.priority = first;
        page.encoding = descriptor::page_encoding(layout);
        column.pages.push_back(std::move(page));
        first = end;
    }
    columns.push_back(std::move(column));
    return true;
}

/// Writes a fixed_size_list column with null elements: validity bits, then the values.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_fixed_size_list_column(const ColumnEncodeContext& ctx, std::ostream& out,
                                  std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const bool column_has_nulls = ctx.column_has_nulls;

    std::string element;
    std::uint64_t items = 0;
    if (!lance_fixed_size_list_parts(field.logical_type, element, items) || items == 0U) {
        error = "element validity on a column that is not a fixed_size_list: " + field.name;
        return false;
    }
    const auto element_bytes = lance_logical_type_value_bytes(element);
    const auto row_bytes = element_bytes * items;
    const auto total = values.fixed_size() / row_bytes;
    const auto by_bytes = max_values_per_uncompressed_chunk(row_bytes + (items + 7U) / 8U + 1U);
    const auto multi = multichunk_values(column_has_nulls ? std::min<std::size_t>(1024U, by_bytes) : by_bytes);
    const auto max_chunk_rows = multi != 0U ? multi : std::size_t{1};
    pb::ColumnMetadata column;
    column.encoding = column_encoding_bytes();
    MiniblockPageWriter page_writer(out);
    std::vector<std::uint8_t> bits;
    for (std::size_t off = 0; off < total;) {
        const auto count = std::min<std::size_t>(max_chunk_rows, static_cast<std::size_t>(total - off));
        const auto first = static_cast<std::uint64_t>(off) * items;
        const auto n = static_cast<std::uint64_t>(count) * items;
        bits.assign(static_cast<std::size_t>((n + 7U) / 8U), 0U);
        for (std::uint64_t k = 0; k < n; ++k) {
            const auto at = first + k;
            if ((values.item_validity[at >> 3U] >> (at & 7U)) & 1U) {
                bits[k >> 3U] |= static_cast<std::uint8_t>(1U << (k & 7U));
            }
        }
        const auto def = column_has_nulls ? pack_definition_levels(values.validity, off, count)
                                          : std::vector<std::uint8_t>{};
        page_writer.begin_chunk();
        const auto footprint = stream_miniblock_chunk(
            out, count, def, {{bits.data(), bits.size()}, {values.fixed_data() + off * row_bytes, count * row_bytes}});
        page_writer.end_chunk(footprint, count);
        off += count;
        if (off < total && multi != 0U && page_writer.payload_bytes() < kTargetMiniblockPageBytes) {
            continue;
        }
        pb::ColumnPage page;
        const auto page_rows = page_writer.rows();
        page_writer.finish(page);
        descriptor::MiniBlock m;
        m.value_compression =
            descriptor::fixed_size_list(items, descriptor::flat(static_cast<std::uint32_t>(element_bytes * 8U)), true);
        if (column_has_nulls) {
            m.def_compression = repdef_encoding_bytes();
        }
        mini_block_tail(m, page_rows, 2U, column_has_nulls);
        page.encoding = descriptor::page_encoding(m);
        column.pages.push_back(std::move(page));
    }
    columns.push_back(std::move(column));
    return true;
}

/// Writes a constant column: the single value stored once, inline or in one data buffer.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_constant_column(const ColumnEncodeContext& ctx, std::ostream& out, std::vector<pb::ColumnMetadata>& columns,
                           std::string& error) {
    const auto& field = ctx.field;
    const auto rows = ctx.rows;

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
        const auto scalar_buffer = scalar_value::encode_variable(value, large);
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
    return true;
}

/// The mini-block chunks of a run-length encoded column (W1q, and the indices of a dictionary, W1p2): one chunk per
/// rle::chunk_rows entry, each with its run values (`bpv` bytes each, read from `data`) and 8-bit run lengths.
struct RleChunks {
    std::vector<std::uint8_t> bytes;
    std::vector<miniblock::ChunkMeta> metas;
};

/// `validity`, when given, adds each chunk's definition levels (a nullable dictionary's indices, W1p3).
RleChunks encode_rle_chunks(const std::uint8_t* data, std::size_t bpv,
                            const std::vector<std::pair<std::size_t, std::uint64_t>>& runs,
                            const std::vector<std::uint8_t>* validity = nullptr) {
    // A run that a chunk ends inside is cut there, and each chunk carries its own run values and run lengths.
    std::vector<std::uint64_t> run_lengths_in_rows;
    run_lengths_in_rows.reserve(runs.size());
    for (const auto& run : runs) {
        run_lengths_in_rows.push_back(run.second);
    }
    // Definition levels pack in one FastLanes block, so a chunk that carries them holds at most 1,024 rows.
    const auto rows_per_chunk = rle::chunk_rows(run_lengths_in_rows, bpv, validity != nullptr ? 1024U : 4096U);
    RleChunks out;
    out.metas.reserve(rows_per_chunk.size());
    std::size_t run_index = 0;
    std::uint64_t run_offset = 0;
    std::size_t chunk_start = 0;
    for (const std::size_t chunk_rows_count : rows_per_chunk) {
        std::vector<std::uint8_t> run_values;
        std::vector<std::uint8_t> run_lengths;  // Lance requires 8-bit run lengths
        std::uint64_t left = chunk_rows_count;
        while (left > 0U) {
            const std::size_t row = runs[run_index].first;
            const std::uint64_t run_len = runs[run_index].second;
            const std::uint64_t take = std::min(run_len - run_offset, left);
            rle::append_run(std::span<const std::uint8_t>(data + (row + run_offset) * bpv, bpv), take, run_values,
                            run_lengths);
            left -= take;
            run_offset += take;
            if (run_offset == run_len) {
                ++run_index;
                run_offset = 0U;
            }
        }
        const std::span<const std::uint8_t> run_buffers[2] = {run_values, run_lengths};
        const std::size_t before = out.bytes.size();
        if (validity != nullptr) {
            const auto def = pack_definition_levels(*validity, chunk_start, chunk_rows_count);
            miniblock::append_chunk(out.bytes, chunk_rows_count, def, run_buffers);
        } else {
            miniblock::append_chunk(out.bytes, 0U, {}, run_buffers);
        }
        out.metas.push_back({out.bytes.size() - before, chunk_rows_count});
        chunk_start += chunk_rows_count;
    }
    return out;
}

/// Writes a run-length encoded fixed-width column: one chunk of run values and run lengths.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_rle_column(const ColumnEncodeContext& ctx, std::ostream& out, std::vector<pb::ColumnMetadata>& columns) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const auto rows = ctx.rows;

    const auto bpv = value_width_bytes(field);
    const std::size_t n = values.fixed_size() / bpv;
    // (row, length) of every run: the write-side "is RLE beneficial?" scan already found them, or scan here.
    std::vector<std::pair<std::size_t, std::uint64_t>> runs;
    if (values.fixed_rle_plan.computed) {
        runs = values.fixed_rle_plan.runs;
    } else {
        std::size_t i = 0;
        while (i < n) {
            std::size_t run = 1;
            while (i + run < n && std::memcmp(values.fixed_data() + (i + run) * bpv,
                                              values.fixed_data() + i * bpv, bpv) == 0) {
                ++run;
            }
            runs.emplace_back(i, run);
            i += run;
        }
    }
    // One mini-block chunk per rle::chunk_rows entry, as pylance writes them (W1q).
    const std::size_t length_bytes = 1U;
    const auto chunks = encode_rle_chunks(values.fixed_data(), bpv, runs);
    // u32 metadata words, has_large_chunk as every nanolance mini-block page: nanolance writes data storage
    // version 2.2, whose validator refuses u16 words (control_buffer_for). Pylance 12 writes u16 words in 2.1
    // files; the chunk sizes and values are the same.
    const auto control = miniblock::control_buffer(chunks.metas);

    align64(out);
    const auto control_offset = pos(out);
    out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));
    align64(out);
    const auto data_offset = pos(out);
    out.write(reinterpret_cast<const char*>(chunks.bytes.data()), static_cast<std::streamsize>(chunks.bytes.size()));

    pb::ColumnMetadata column;
    column.encoding = column_encoding_bytes();
    pb::ColumnPage page;
    page.buffer_offsets.push_back(control_offset);
    page.buffer_offsets.push_back(data_offset);
    page.buffer_sizes.push_back(control.size());
    page.buffer_sizes.push_back(chunks.bytes.size());
    page.length = rows;
    page.priority = 0;
    page.encoding = page_layout_bytes_rle(static_cast<std::uint8_t>(bpv * 8U),
                                          static_cast<std::uint8_t>(length_bytes * 8U), rows);
    column.pages.push_back(std::move(page));
    columns.push_back(std::move(column));
    return true;
}

/// Writes a dictionary + RLE variable-width column: the distinct values, and RLE'd indices.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_dict_rle_column(const ColumnEncodeContext& ctx, std::ostream& out, std::vector<pb::ColumnMetadata>& columns,
                           std::string& error) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const auto rows = ctx.rows;
    const auto compression_level = ctx.compression_level;

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
            rle::append_index_run(index, run_length, run_values, run_lengths);
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
            rle::append_index_run(indices[r], run, run_values, run_lengths);
            r += run;
        }
    }
    std::vector<std::uint8_t> chunk_bytes;
    const std::span<const std::uint8_t> run_buffers[2] = {run_values, run_lengths};
    miniblock::append_chunk(chunk_bytes, 0U, {}, run_buffers);
    const miniblock::ChunkMeta meta[1] = {{chunk_bytes.size(), static_cast<std::size_t>(rows)}};
    const auto control = miniblock::control_buffer(meta);
    std::vector<std::uint8_t> dict_frame;
    if (!general::zstd_frame(dictionary::encode_variable(distinct, values.variable.large), compression_level,
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
    return true;
}

/// Writes a structural dictionary column: bitpacked indices, and an uncompressed dictionary block.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_dict_column(const ColumnEncodeContext& ctx, std::ostream& out, std::vector<pb::ColumnMetadata>& columns) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const auto rows = ctx.rows;

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
    // The indices' layout follows lance-encoding:compression, as pylance's does for a dictionary column
    // (measured on 12.0.0): none -> Flat(32); lz4 / zstd -> General{codec, ByteStreamSplit{Flat(32)}}, each
    // chunk byte-split then framed. Absent keeps the bit-packed indices nanolance has always written.
    const DictIndexLayout index_layout = ctx.codec == ColumnCodec::kUncompressed ? DictIndexLayout::kFlat
                                         : (ctx.codec == ColumnCodec::kLz4 || ctx.codec == ColumnCodec::kZstd)
                                             ? DictIndexLayout::kGeneral
                                             : DictIndexLayout::kBitpacked;
    const std::uint64_t index_scheme = ctx.codec == ColumnCodec::kLz4 ? descriptor::kLz4 : descriptor::kZstd;
    std::vector<MiniblockChunk> index_chunks;
    std::vector<std::uint8_t> planes;  // kGeneral: one chunk's byte-split indices, before framing
    for (std::size_t off = 0; off < indices.size(); off += 1024U) {
        const auto count = std::min<std::size_t>(1024U, indices.size() - off);
        MiniblockChunk chunk;
        chunk.value_count = count;
        const auto* raw = reinterpret_cast<const std::uint8_t*>(indices.data() + off);
        if (index_layout == DictIndexLayout::kBitpacked) {
            bitpack::encode_block(raw, count, 4U, chunk.bytes);
        } else if (index_layout == DictIndexLayout::kFlat) {
            chunk.bytes.assign(raw, raw + count * 4U);
        } else {
            planes.resize(count * 4U);
            (void)byte_stream_split::encode(std::as_bytes(std::span(raw, count * 4U)), 4U,
                                            std::as_writable_bytes(std::span(planes)));
            std::string error;
            const bool framed_ok = index_scheme == descriptor::kLz4
                                       ? general::lz4_frame(planes, chunk.bytes, error)
                                       : general::zstd_frame(planes, ctx.compression_level, chunk.bytes, error);
            if (!framed_ok) {
                return false;
            }
        }
        index_chunks.push_back(std::move(chunk));
    }
    const auto payload = miniblock_payload(index_chunks);
    const auto control = control_buffer_for(index_chunks);
    // lance-encoding:dict-values-compression = none | lz4 | zstd (+ dict-values-compression-level) wraps the whole
    // dictionary buffer in General. Absent keeps the plain buffer, nanolance's default, unchanged.
    std::uint64_t dict_scheme = 0U;
    std::vector<std::uint8_t> dict_block = dictionary::encode_variable(distinct, values.variable.large);
    if (const auto it = field.metadata.find("lance-encoding:dict-values-compression"); it != field.metadata.end()) {
        int level = ctx.compression_level;
        if (const auto lv = field.metadata.find("lance-encoding:dict-values-compression-level");
            lv != field.metadata.end()) {
            int parsed = 0;
            const auto [end, ec] = std::from_chars(lv->second.data(), lv->second.data() + lv->second.size(), parsed);
            if (ec == std::errc{} && end == lv->second.data() + lv->second.size()) {
                level = parsed;
            }
        }
        // The value was checked by read_dict_values_option before any page was written: none, lz4 or zstd.
        std::vector<std::uint8_t> framed;
        std::string error;
        if (it->second == "zstd") {
            dict_scheme = descriptor::kZstd;
            if (!general::zstd_frame(dict_block, level, framed, error)) {
                return false;
            }
            dict_block = std::move(framed);
        } else if (it->second == "lz4") {
            dict_scheme = descriptor::kLz4;
            if (!general::lz4_frame(dict_block, framed, error)) {
                return false;
            }
            dict_block = std::move(framed);
        }
    }

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
        page_layout_bytes_dict(static_cast<std::uint32_t>(distinct.size()), rows, values.variable.large, dict_scheme,
                               index_layout, index_scheme);
    column.pages.push_back(std::move(page));
    columns.push_back(std::move(column));
    return true;
}

/// Writes a 64-bit fixed-width structural dictionary (W1p): the indices as write_dict_column writes them
/// (InlineBitpacking(32) chunks), and the dictionary block the decision stored in the plan, with the
/// descriptor Lance uses for it: InlineBitpacking(64), OutOfLineBitpacking(64, Flat(width)), Flat(64), or
/// General{LZ4, Flat(64)} when bitpacking did not pay.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_dict_fixed_column(const ColumnEncodeContext& ctx, std::ostream& out, std::vector<pb::ColumnMetadata>& columns) {
    const auto& plan = ctx.values.structural_dict_plan;
    const auto rows = ctx.rows;
    const auto& indices = plan.indices;
    // The indices: InlineBitpacking(32) chunks, or (W1p2, when the decision found runs) RLE'd u32 values in the W1q
    // chunks, as pylance writes a sorted or run-heavy dictionary column (`Rle{Flat(32),Flat(8)}`).
    std::vector<MiniblockChunk> index_chunks;
    RleChunks index_rle;
    // A nullable column (W1p3) carries each chunk's definition levels, as pylance's repdef=Bitpacked(16,Flat(1)).
    const bool nullable = ctx.column_has_nulls;
    if (plan.index_rle) {
        index_rle = encode_rle_chunks(reinterpret_cast<const std::uint8_t*>(indices.data()), 4U, plan.index_runs,
                                      nullable ? &ctx.values.validity : nullptr);
    } else {
        for (std::size_t off = 0; off < indices.size(); off += 1024U) {
            const auto count = std::min<std::size_t>(1024U, indices.size() - off);
            MiniblockChunk chunk;
            chunk.value_count = count;
            if (nullable) {
                chunk.repdef = pack_definition_levels(ctx.values.validity, off, count);
            }
            bitpack::encode_block(reinterpret_cast<const std::uint8_t*>(indices.data() + off), count, 4U, chunk.bytes);
            index_chunks.push_back(std::move(chunk));
        }
    }
    const auto control = plan.index_rle ? miniblock::control_buffer(index_rle.metas) : control_buffer_for(index_chunks);
    const auto payload = plan.index_rle ? std::move(index_rle.bytes) : miniblock_payload(index_chunks);

    using FixedDictValues = StructuralDictPlan::FixedDictValues;
    descriptor::Bytes dict_desc;
    switch (plan.fixed_values) {
        case FixedDictValues::kInlineBitpacking:
            dict_desc = descriptor::inline_bitpacking(64U);
            break;
        case FixedDictValues::kOutOfLineBitpacking:
            dict_desc = descriptor::out_of_line_bitpacking(64U, descriptor::flat(plan.fixed_width));
            break;
        case FixedDictValues::kFlat:
            dict_desc = descriptor::flat(64U);
            break;
        case FixedDictValues::kLz4:
            dict_desc = descriptor::general(descriptor::kLz4, descriptor::flat(64U));
            break;
    }

    align64(out);
    const auto control_offset = pos(out);
    out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));
    align64(out);
    const auto data_offset = pos(out);
    out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    align64(out);
    const auto dict_offset = pos(out);
    out.write(reinterpret_cast<const char*>(plan.fixed_block.data()),
              static_cast<std::streamsize>(plan.fixed_block.size()));

    pb::ColumnMetadata column;
    column.encoding = column_encoding_bytes();
    pb::ColumnPage page;
    page.buffer_offsets.push_back(control_offset);
    page.buffer_offsets.push_back(data_offset);
    page.buffer_offsets.push_back(dict_offset);
    page.buffer_sizes.push_back(control.size());
    page.buffer_sizes.push_back(payload.size());
    page.buffer_sizes.push_back(plan.fixed_block.size());
    page.length = rows;
    page.priority = 0;
    descriptor::MiniBlock m;
    m.value_compression = plan.index_rle ? descriptor::rle(descriptor::flat(32U), descriptor::flat(8U))
                                         : descriptor::inline_bitpacking(32U);
    m.dictionary = dict_desc;
    m.num_dictionary_items = static_cast<std::uint32_t>(plan.fixed_distinct.size());
    if (nullable) {
        m.def_compression = repdef_encoding_bytes();
    }
    mini_block_tail(m, rows, plan.index_rle ? 2U : 1U, nullable);  // RLE: run values and run lengths are two buffers
    page.encoding = descriptor::page_encoding(m);
    column.pages.push_back(std::move(page));
    columns.push_back(std::move(column));
    return true;
}

/// Writes a flat (uncompressed) fixed-width column, streaming each miniblock chunk from the values.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_flat_fixed_column(const ColumnEncodeContext& ctx, const FixedWidthChoice& fixed, std::ostream& out,
                             std::vector<pb::ColumnMetadata>& columns) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const bool column_has_nulls = ctx.column_has_nulls;
    const auto fixed_bytes_per_value = fixed.bytes_per_value;

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
            stream_miniblock_chunk(out, count, chunk_repdef,
                                   values.fixed_data() + off * fixed_bytes_per_value, chunk_bytes);
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
    return true;
}

/// Writes a bitpacked, bool or byte-stream-split + zstd fixed-width column, one chunk at a time.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_packed_fixed_column(const ColumnEncodeContext& ctx, const FixedWidthChoice& fixed, std::ostream& out,
                               std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const auto compression_level = ctx.compression_level;
    const bool column_has_nulls = ctx.column_has_nulls;
    const bool bitpack = fixed.bitpack;
    const bool bool_pack = fixed.bool_pack;
    const bool bss_general = fixed.bss_general;
    const auto general_scheme = fixed.general_scheme;
    const auto fixed_bytes_per_value = fixed.bytes_per_value;

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
    std::vector<std::uint8_t> framed;   // General-compressed chunk (bss only), reused across chunks
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
        return page_layout_bytes_bss_general(static_cast<std::uint8_t>(flat_bits_per_value(field)), count,
                                             general_scheme, column_has_nulls);
    };
    std::uint64_t cached_rows = 0;
    std::vector<std::uint8_t> cached_encoding;
    MiniblockPageWriter page_writer(out);
    for (std::size_t off = 0; off < total;) {
        const auto count = std::min(step, total - off);
        if (bitpack) {
            // Each chunk buffer = [bit_width][FastLanes packed 1024 values].
            bitpack::encode_block(values.fixed_data() + off * fixed_bytes_per_value, count,
                                  fixed_bytes_per_value, scratch);
        } else if (bool_pack) {
            // Bit-pack LSB-first, ceil(count/8) bytes; no zstd on top (stock Lance doesn't
            // compress 1-bit-packed bool either). values.fixed is one byte per value here.
            boolpack::pack_lsb_first(values.fixed_data() + off, count, scratch);
        } else {
            // Byte-transpose (mantissa/exponent bytes grouped) so the zstd frame compresses
            // meaningfully, then frame it: bytes become [u64 raw size][zstd].
            scratch.resize(fixed_bytes_per_value * count);
            (void)byte_stream_split::encode(
                std::as_bytes(std::span(values.fixed_data() + off * fixed_bytes_per_value,
                                        fixed_bytes_per_value * count)),
                fixed_bytes_per_value, std::as_writable_bytes(std::span(scratch)));
            const bool framed_ok = general_scheme == descriptor::kLz4
                                       ? general::lz4_frame(scratch, framed, error)
                                       : general::zstd_frame(scratch, compression_level, framed, error);
            if (!framed_ok) {
                return false;
            }
        }
        const auto& chunk_bytes = bss_general ? framed : scratch;
        const auto chunk_repdef =
            column_has_nulls ? pack_definition_levels(values.validity, off, count)
                             : std::vector<std::uint8_t>{};
        page_writer.begin_chunk();
        const auto footprint =
            stream_miniblock_chunk(out, count, chunk_repdef, chunk_bytes.data(), chunk_bytes.size());
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
    return true;
}

/// Writes a plain variable-width column: FullZip pages for large values, MiniBlock chunks otherwise.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_plain_variable_column(const ColumnEncodeContext& ctx, std::ostream& out,
                                 std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const auto rows = ctx.rows;

    pb::ColumnMetadata column;
    // Large values get FullZip pages (row-addressable, as Lance writes them); the rest
    // MiniBlock chunks.
    std::uint64_t valid_rows = 0;
    for (std::uint64_t r = 0; r < rows; ++r) {
        valid_rows += validity_bit(values.validity, r) ? 1U : 0U;
    }
    const auto total_bytes = static_cast<std::uint64_t>(variable_offset(values, rows) - variable_offset(values, 0));
    const bool full_zip = valid_rows != 0U && total_bytes / valid_rows >= kFullZipMinAverageValueBytes;
    if (full_zip ? !write_full_zip_variable_column(out, values, rows, column, error)
                 : !write_nested_column(out, field, values, rows, column, error, nullptr, /*plain=*/true)) {
        return false;
    }
    columns.push_back(std::move(column));
    return true;
}

/// Writes a variable-width column as MiniBlock chunks, each zstd-compressed when the column is compressed.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_zstd_variable_column(const ColumnEncodeContext& ctx, std::ostream& out,
                                std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    const auto& values = ctx.values;
    const auto compression_level = ctx.compression_level;
    const bool compress_column = ctx.compress_column;
    const bool column_has_nulls = ctx.column_has_nulls;

    std::vector<MiniblockChunk> chunks;

    // zstd variable-width columns keep the two-phase build (chunks are unequal-sized, driven by
    // the offsets math in build_variable_chunks_for_column). A nullable column additionally caps
    // each chunk at one FastLanes block, since that is what a definition-level buffer covers.
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

    // lz4 (`lance-encoding:compression=lz4`) is kept only when it shrinks the first chunk: pylance writes
    // the plain layout for a column LZ4 does not shrink (measured on 12-letter random strings). zstd
    // always applies where the column asks for it.
    const bool lz4_requested = ctx.codec == ColumnCodec::kLz4;
    const std::uint64_t general_scheme = lz4_requested ? descriptor::kLz4 : descriptor::kZstd;
    std::vector<std::uint8_t> framed;
    if (lz4_requested && !chunks.empty()) {
        if (!general::lz4_frame(chunks.front().bytes, framed, error)) {
            return false;
        }
        if (framed.size() >= chunks.front().bytes.size()) {
            return write_plain_variable_column(ctx, out, columns, error);
        }
    }
    const bool zstd_variable = lz4_requested || compress_column;
    pb::ColumnMetadata column;
    column.encoding = column_encoding_bytes();
    // Hoisted out of the loop (not freshly declared per chunk): general::zstd_frame's resize() only
    // avoids re-zeroing already-there bytes when the buffer is reused across calls of similar size,
    // which requires swap (not move) below so `framed`'s capacity survives being handed off.
    std::uint64_t first_row = 0;
    for (auto& chunk : chunks) {
        if (zstd_variable) {
            const bool framed_ok = lz4_requested
                                       ? general::lz4_frame(chunk.bytes, framed, error)
                                       : general::zstd_frame(chunk.bytes, compression_level, framed, error);
            if (!framed_ok) {
                return false;
            }
            std::swap(chunk.bytes, framed);  // value_count unchanged; bytes are now [u32 or u64 size][frame]
        }
        const auto control = control_buffer_for({&chunk, 1});
        const auto payload = miniblock_payload({&chunk, 1});

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
        page.priority = first_row;
        first_row += chunk.value_count;
        const auto bits_token =
            values.variable.large ? static_cast<std::uint8_t>(0x40U) : static_cast<std::uint8_t>(0x20U);
        page.encoding = zstd_variable
                            ? page_layout_bytes_variable_zstd(bits_token, chunk.value_count,
                                                              column_has_nulls, general_scheme)
                            : page_layout_bytes(bits_token, chunk.value_count, true, column_has_nulls);
        column.pages.push_back(std::move(page));
    }
    columns.push_back(std::move(column));
    return true;
}

/// Writes a fixed-width column: flat, or bitpacked / bool / byte-stream-split + zstd, after its value-size checks.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_fixed_width_column(const ColumnEncodeContext& ctx, std::ostream& out,
                              std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const auto rows = ctx.rows;
    const auto packing_it = ctx.packing_it;

    const bool is_variable = values.kind == ColumnValues::Kind::VariableWidth;
    // Bitpacking is a structural encoding, signalled by the writer's packing tag (independent of the
    // zstd `compress` flag, which now gates only the variable-width zstd path below).
    const bool bitpack = !is_variable && packing_it != field.metadata.end() &&
                         packing_it->second == "bitpack" &&
                         (lance_logical_type_is_bitpackable_integer(field.logical_type) ||
                          lance_logical_type_is_float(field.logical_type));
    // Byte-stream-split + General: float/double, tagged when set_compression(true) (writer.cpp), or any
    // 32/64-bit column with `lance-encoding:compression=lz4|zstd` (pylance's layout for that option).
    const bool codec_general = !is_variable && (ctx.codec == ColumnCodec::kLz4 || ctx.codec == ColumnCodec::kZstd) &&
                               lance_logical_type_is_bitpackable_integer(field.logical_type) &&
                               value_width_bytes(field) == 8U;
    const bool bss_general = !is_variable && field.logical_type != "bool" &&
                             (codec_general || (packing_it != field.metadata.end() &&
                                                packing_it->second == "bss-zstd"));
    const std::uint64_t general_scheme = ctx.codec == ColumnCodec::kLz4 ? descriptor::kLz4 : descriptor::kZstd;
    // bool is always stored bit-packed (1 bit/value, LSB-first) on disk, matching stock Lance's own
    // Flat{bits_per_value:1} representation (verified empirically) -- not gated by any opt-in flag.
    const bool bool_pack = !is_variable && field.logical_type == "bool";
    const auto fixed_bytes_per_value = value_width_bytes(field);
    const FixedWidthChoice fixed{bitpack, bool_pack, bss_general, general_scheme, fixed_bytes_per_value};

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
    if (!is_variable && !bitpack && !bss_general && !bool_pack) {
        return write_flat_fixed_column(ctx, fixed, out, columns);
    }

    // Bitpack / bool / byte-stream-split+zstd fixed-width columns: STREAM one chunk at a time
    // through hoisted, loop-reused scratch buffers (build chunk -> write it -> reuse); after the
    // first chunk resize() touches nothing, since chunks are equal-sized. Pages gather chunks as
    // the flat path above does.
    return write_packed_fixed_column(ctx, fixed, out, columns, error);
}

/// Writes a variable-width column: an FSST page, plain pages, or zstd-compressed chunks.
/// One branch of encode_column, which writes one column (step 2 of write_lance_data_file).
bool write_variable_column(const ColumnEncodeContext& ctx, std::ostream& out,
                           std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    const auto& field = ctx.field;
    const auto& values = ctx.values;
    const auto rows = ctx.rows;
    const auto packing_it = ctx.packing_it;
    const bool zstd_requested = ctx.zstd_requested;
    const bool compress = ctx.compress;
    const bool compress_column = ctx.compress_column;

    // A string column FSST can compress (roadmap F2) goes to the multi-chunk page writer: one
    // symbol table per page, over 1024-value chunks. The single-chunk pages below would pay the
    // 2,312-byte table every 32 KiB. build_nested_page still declines FSST page by page when it
    // does not pay, writing plain Variable chunks instead.
    // Only when the writer's structural detection tagged the column: FSST is one of those
    // encodings, and set_structural_encoding(false) must still write plain pages.
    fsst::Encoder fsst_encoder;
    // A column that names its codec (lz4, zstd or none) is laid out as that codec says: no FSST.
    if (packing_it != field.metadata.end() && packing_it->second == "fsst" && ctx.codec == ColumnCodec::kAbsent &&
        train_column_fsst(field, values, fsst_encoder) &&
        !zstd_requested && (!compress || fsst_pays_on_first_page(fsst_encoder, values, rows))) {
        pb::ColumnMetadata column;
        if (!write_nested_column(out, field, values, rows, column, error, &fsst_encoder)) {
            return false;
        }
        columns.push_back(std::move(column));
        return true;
    }

    // Every other string or binary column goes to the multi-chunk page writer too, in its plain
    // mode (Variable chunks, no dictionary, no FSST). The single-chunk pages below describe a
    // chunk's value bytes in a u16, so one value over ~32 KB -- a JPEG, an audio clip -- wrote a
    // file Rust Lance could not read, and over 64 KB one nanolance could not read either; they
    // also cost Rust a page set-up per 32 KB. The multi-chunk writer's chunks carry u32 sizes.
    // What stays below is zstd (compress), which only these pages carry, and only while every
    // value fits a chunk.
    bool value_too_big_for_a_chunk = false;
    {
        const auto width = values.variable.large ? 8U : 4U;
        const auto entries = values.variable.offsets.size() / width;
        for (std::size_t i = 0; i + 1U < entries && !value_too_big_for_a_chunk; ++i) {
            value_too_big_for_a_chunk =
                variable_offset(values, i + 1U) - variable_offset(values, i) > kMaxVariableMiniblockBytes / 2U;
        }
    }

    if ((!compress_column && ctx.codec != ColumnCodec::kLz4) || value_too_big_for_a_chunk) {
        return write_plain_variable_column(ctx, out, columns, error);
    }
    return write_zstd_variable_column(ctx, out, columns, error);
}

/// Reads a field's `lance-encoding:compression`. An unknown scheme is refused, as pylance refuses it
/// (it spells the three names in lower case and rejects anything else on a scalar column).
bool read_column_codec(const LanceField& field, ColumnCodec& codec, std::string& error) {
    const auto it = field.metadata.find("lance-encoding:compression");
    if (it == field.metadata.end()) {
        codec = ColumnCodec::kAbsent;
        return true;
    }
    if (it->second == "lz4") {
        codec = ColumnCodec::kLz4;
    } else if (it->second == "zstd") {
        codec = ColumnCodec::kZstd;
    } else if (it->second == "none") {
        codec = ColumnCodec::kUncompressed;
    } else {
        error = "unknown lance-encoding:compression '" + it->second + "' on column '" + field.name +
                "' (expected lz4, zstd or none)";
        return false;
    }
    return true;
}

/// Writes one column's pages to `out` and its metadata to `columns`, choosing its encoding.
/// Step 2 of write_lance_data_file, per column: called by encode_columns.
bool encode_column(const FileWriteState& state, std::size_t field_index, std::ostream& out,
                   std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    const auto& field = *state.physical_fields[field_index];
    const auto& values = *state.column_source[field_index];
    const auto rows = state.rows;
    const bool compress = state.compress;
    int compression_level = state.compression_level;

    // `lance-encoding:compression` on one column (pylance's per-column option): lz4 or zstd compresses that
    // column's values alone, as the writer-wide `compress` flag does for every string column, and none
    // stores them uncompressed. Such a column skips FSST, which would otherwise be chosen where it pays.
    ColumnCodec codec = ColumnCodec::kAbsent;
    if (!values.needs_nested_pages() && !read_column_codec(field, codec, error)) {
        return false;
    }
    // lance-encoding:dict-values-compression names the dictionary buffer's codec; refused here, by name, so no
    // page is written before the value is known to be one the writer can frame. Only a column written as a
    // dictionary is checked: pylance accepts (and ignores) any value on a column it does not dictionary-encode.
    const auto dict_packing = field.metadata.find("nanolance:packing");
    const bool dictionary_column = !field.dictionary_index_format.empty() ||
                                   (dict_packing != field.metadata.end() &&
                                    (dict_packing->second == "dict" || dict_packing->second == "dict-rle"));
    if (const auto dv = field.metadata.find("lance-encoding:dict-values-compression");
        dictionary_column && dv != field.metadata.end() && dv->second != "none" && dv->second != "lz4" &&
        dv->second != "zstd") {
        error = "unknown lance-encoding:dict-values-compression '" + dv->second + "' on column '" + field.name +
                "' (expected lz4, zstd or none)";
        return false;
    }
    // lance-encoding:structural-encoding is fullzip or miniblock; anything else is refused with pylance's message
    // (measured on pylance 12.0.0), before any page is written.
    if (const auto so = field.metadata.find("lance-encoding:structural-encoding");
        so != field.metadata.end() && so->second != "fullzip" && so->second != "miniblock") {
        error = "Invalid user input: Cannot determine structural encoding for field " + field.name +
                ".  This typically indicates an invalid value of the field metadata key "
                "lance-encoding:structural-encoding";
        return false;
    }
    // `lance-encoding:compression-level` sets the zstd level of a zstd column (pylance: a number is the
    // level, zstd clamps it to its range; anything else keeps the writer's level). lz4 has no level.
    if (codec == ColumnCodec::kZstd) {
        const auto level_option = field.metadata.find("lance-encoding:compression-level");
        if (level_option != field.metadata.end()) {
            const auto& text = level_option->second;
            int parsed = 0;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (ec == std::errc{} && end == text.data() + text.size()) {
                compression_level = parsed;
            }
        }
    }
    const bool zstd_requested = codec == ColumnCodec::kZstd;
    const bool compress_column = codec == ColumnCodec::kAbsent ? compress : zstd_requested;

    // Nulls are encodable only on the paths that have been taught the definition-level layer.
    // Everything else must refuse rather than drop them: ingest no longer rejects a null batch,
    // so this is the backstop that keeps a column with nulls from being written as if it had
    // none -- which is exactly the silent [10, null, 30] -> [10, 0, 30] corruption this whole
    // area is here to prevent.
    const bool column_has_nulls = values.null_count != 0U;
    if (column_has_nulls) {
        if (values.validity.empty()) {
            error = "column '" + field.name + "' reports " + std::to_string(values.null_count) +
                    " nulls but carries no validity bitmap";
            return false;
        }
    }

    // A fixed-width column that names its codec is laid out by that option alone: nanolance's own
    // structural tags (bitpacking, constant, RLE, byte-stream-split) were chosen for the default layout,
    // and pylance's option replaces them (`none` is a flat page, lz4/zstd a General-wrapped one).
    // Measured on pylance 12.0.0: `none` is Flat for every fixed-width type; lz4 and zstd change only a
    // 64-bit integer column (General{codec, ByteStreamSplit{Flat(64)}}); other types keep their layout.
    const bool wide_integer = values.kind == ColumnValues::Kind::FixedWidth &&
                              lance_logical_type_is_bitpackable_integer(field.logical_type) &&
                              value_width_bytes(field) == 8U;
    const bool codec_lays_out_values =
        codec == ColumnCodec::kUncompressed ||
        ((codec == ColumnCodec::kLz4 || codec == ColumnCodec::kZstd) && wide_integer);
    // A structural dictionary keeps its tag under any codec: its indices take the codec's layout in write_dict_column.
    const auto tag_it = field.metadata.find("nanolance:packing");
    const bool dict_tagged = tag_it != field.metadata.end() && tag_it->second == "dict" &&
                             (values.kind == ColumnValues::Kind::VariableWidth ||
                              values.kind == ColumnValues::Kind::FixedWidth);
    const auto packing_it =
        codec_lays_out_values && !dict_tagged ? field.metadata.end() : tag_it;
    const ColumnEncodeContext ctx{field,           values,      rows,          compression_level,
                                  compress,        zstd_requested, compress_column, column_has_nulls,
                                  packing_it,      codec};

    if (values.kind == ColumnValues::Kind::BlobV2External) {
        return write_blob_v2_column(ctx, out, columns, error);
    }

    // A leaf under a struct with nulls of its own (or a dictionary, or element nulls) is written with the nested
    // pages even where the struct adds no layer: the top-level writer's flat levels are not what Lance reads back
    // under a struct (a pylance read panics on them). A dictionary leaf takes this path always, for pylance's shape
    // (repdef levels, InlineBitpacking indices, a General dictionary) without nulls too.
    const bool nested_leaf = field.parent_id >= 0 && (column_has_nulls || !values.item_validity.empty() ||
                                                      !field.dictionary_index_format.empty());
    if (values.needs_nested_pages() || nested_leaf) {
        pb::ColumnMetadata column;
        if (!write_nested_column(out, field, values, rows, column, error)) {
            return false;
        }
        columns.push_back(std::move(column));
        return true;
    }

    // Arrow's null type: every row is null and there is no value to store at all. An empty struct
    // (a physical "struct" column) is written the same way, as Lance writes it: the page says
    // nothing a reader needs, the type says every row is a valid empty struct.
    if (field.logical_type == "null" || field.logical_type == "struct") {
        return write_all_null_column(ctx, columns);
    }

    // An Arrow dictionary column: Lance's dictionary page -- the indices, at the index type's
    // width, in mini-block chunks, and the dictionary it was given (unused entries and order
    // kept) as the page's third buffer.
    if (!field.dictionary_index_format.empty()) {
        return write_dictionary_column(ctx, out, columns);
    }

    // A fixed_size_list with null elements inside valid rows: Lance's mini-block chunks of two
    // buffers, the elements' validity bits then the values, under FixedSizeList{has_validity}. From 256
    // bytes of values on (or when asked with structural-encoding=fullzip) it is FullZip instead, with the
    // validity bits in each row's slot.
    if (!values.item_validity.empty()) {
        bool full_zip_items = false;
        if (!fixed_column_wants_full_zip(field, true, full_zip_items, error)) {
            return false;
        }
        if (full_zip_items) {
            return write_full_zip_fixed_column(ctx, out, columns, error);
        }
        return write_fixed_size_list_column(ctx, out, columns, error);
    }

    // A fixed-width column of 256-byte values, or one asked for with structural-encoding=fullzip, as FullZip
    // pages (see write_full_zip_fixed_column). A constant column stays Constant even when fullzip is asked for
    // (measured on pylance 12.0.0). Fixed-width columns with element validity took the path above.
    const bool constant_tagged = packing_it != field.metadata.end() && packing_it->second == "constant";
    if (values.kind == ColumnValues::Kind::FixedWidth && !constant_tagged) {
        bool full_zip_fixed = false;
        if (!fixed_column_wants_full_zip(field, false, full_zip_fixed, error)) {
            return false;
        }
        if (full_zip_fixed) {
            return write_full_zip_fixed_column(ctx, out, columns, error);
        }
    }

    // Constant column (tagged by the writer): ConstantLayout. The single value comes from the
    // field metadata. Fixed-width stores it inline (zero data buffers); variable-width stores it
    // in one data buffer.
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
        return write_constant_column(ctx, out, columns, error);
    }

    // Run-length encoded fixed-width column (tagged by the writer): one chunk with two buffers
    // (run values + run lengths) -> Rle PageLayout.
    if (!column_has_nulls && packing_it != field.metadata.end() && packing_it->second == "rle" &&
        values.kind == ColumnValues::Kind::FixedWidth) {
        return write_rle_column(ctx, out, columns);
    }

    // Dictionary + RLE for a low-cardinality variable-width column: distinct values in buffer[2],
    // per-row u32 indices RLE'd in buffer[1].
    if (!column_has_nulls && packing_it != field.metadata.end() && packing_it->second == "dict-rle" &&
        values.kind == ColumnValues::Kind::VariableWidth) {
        return write_dict_rle_column(ctx, out, columns, error);
    }

    // Structural dictionary for scattered low-cardinality strings: flat bitpacked u32 indices in
    // buffer[1], uncompressed dictionary variable block in buffer[2].
    if (!column_has_nulls && packing_it != field.metadata.end() && packing_it->second == "dict" &&
        values.kind == ColumnValues::Kind::VariableWidth) {
        return write_dict_column(ctx, out, columns);
    }
    // A 64-bit fixed-width dictionary (W1p): the decision (encoding_choice::fixed_column_dict_beneficial) stored it.
    // A slice drops the plan (column_slice.cpp); without one the column is written flat, which is correct.
    if (packing_it != field.metadata.end() && packing_it->second == "dict" &&
        values.kind == ColumnValues::Kind::FixedWidth && values.structural_dict_plan.computed) {
        return write_dict_fixed_column(ctx, out, columns);
    }

    const bool is_variable = values.kind == ColumnValues::Kind::VariableWidth;
    if (!is_variable) {
        return write_fixed_width_column(ctx, out, columns, error);
    }
    return write_variable_column(ctx, out, columns, error);
}

/// Encodes every column in order: straight into `out`, or with several threads each column into a buffer
/// that is appended in order. Step 2 of write_lance_data_file.
bool encode_columns(const FileWriteState& state, bool parallel_columns, std::ostream& out,
                    std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    const auto& physical_fields = state.physical_fields;
    const auto& column_values = state.column_values;

    // Columns side by side only when no one column is most of the data: a column encoded into
    // memory is copied again into the file, and for the column that dominates (COCO's images, 800 of
    // 828 MB; Speech Commands' waveforms) that costs more than the others' encoding saves -- measured
    // 1.95 s streaming against 3.28 s buffered. Such a column still uses threads inside its pages.
    const auto raw_bytes = [&](std::size_t i) {
        const auto& v = column_values[i];
        return static_cast<std::uint64_t>(v.fixed_size() + v.variable.data.size() + v.variable.offsets.size() +
                                          v.blob_v2.packed_payload.size());
    };
    std::uint64_t total_raw = 0;
    std::uint64_t largest_raw = 0;
    for (std::size_t i = 0; i < physical_fields.size(); ++i) {
        total_raw += raw_bytes(i);
        largest_raw = std::max(largest_raw, raw_bytes(i));
    }
    const bool one_column_dominates = largest_raw * 2U > total_raw;
    if (!parallel_columns || parallel::threads() <= 1U || physical_fields.size() <= 1U || one_column_dominates) {
        for (std::size_t field_index = 0; field_index < physical_fields.size(); ++field_index) {
            if (!encode_column(state, field_index, out, columns, error)) {
                return false;
            }
        }
    } else {
        // Columns encoded side by side into memory, then appended in order, each at a 64-byte
        // boundary as the sequential writer places them, its page offsets moved by where it lands.
        // Costs holding the fragment's encoded columns at once; one thread keeps the streaming path.
        struct Encoded {
            VectorStreambuf bytes;
            std::vector<pb::ColumnMetadata> columns;
            std::string error;
            bool ok = false;
        };
        std::vector<Encoded> encoded(physical_fields.size());
        parallel::for_each(encoded.size(), [&](std::size_t field_index) {
            auto& e = encoded[field_index];
            e.bytes.reserve(static_cast<std::size_t>(raw_bytes(field_index) + (raw_bytes(field_index) >> 4U) + 4096U));
            std::ostream sink(&e.bytes);
            e.ok = encode_column(state, field_index, sink, e.columns, e.error) && sink.good();
            if (!e.ok && e.error.empty()) {
                e.error = "failed to buffer column '" + physical_fields[field_index]->name + "'";
            }
        });
        for (auto& e : encoded) {
            if (!e.ok) {
                error = e.error;
                return false;
            }
        }
        work_stats::add(work_stats::counters().parallel_column_writes, 1U);
        for (auto& e : encoded) {
            work_stats::add(work_stats::counters().write_buffered_bytes, e.bytes.data().size());
            align64(out);
            const auto base = pos(out);
            const auto& bytes = e.bytes.data();
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            for (auto& column : e.columns) {
                for (auto& page : column.pages) {
                    for (auto& offset : page.buffer_offsets) {
                        offset += base;
                    }
                }
                columns.push_back(std::move(column));
            }
            e.bytes.release();
        }
    }

    return true;
}

/// Places the blobs given as bytes: inline ones at the head of the file, packed and dedicated ones in sidecar
/// files beside it. Step 1 of write_lance_data_file.
bool place_inline_blobs(FileWriteState& state, const std::filesystem::path& full_path,
                        const LanceFileExtras* extras, std::ostream& out, std::string& error) {
    const auto& physical_fields = state.physical_fields;
    const auto& column_values = state.column_values;
    auto& column_source = state.column_source;
    auto& placed_blobs = state.placed_blobs;

    BlobV2Placer placer(full_path, extras != nullptr ? extras->blob_pack_file_size : 0U);
    std::size_t blob_columns = 0;
    for (const auto& v : column_values) {
        blob_columns += v.kind == ColumnValues::Kind::BlobV2External ? 1U : 0U;
    }
    placed_blobs.reserve(blob_columns);
    for (std::size_t i = 0; i < physical_fields.size(); ++i) {
        column_source[i] = &column_values[i];
        if (column_values[i].kind != ColumnValues::Kind::BlobV2External ||
            !blob_v2_has_pending(column_values[i])) {
            continue;  // external references (or nulls) only: stored as they are
        }
        BlobV2Thresholds thresholds;
        if (!blob_v2_thresholds(*physical_fields[i], thresholds, error)) {
            return false;
        }
        placed_blobs.emplace_back();
        if (!placer.place(column_values[i], thresholds, out, placed_blobs.back(), error)) {
            return false;
        }
        column_source[i] = &placed_blobs.back();
    }
    if (!placer.finish(error)) {
        return false;
    }

    return true;
}

/// Builds the file's schema descriptor from the mapping: a Blob v2 field as Lance stores it. Step 3 of
/// write_lance_data_file.
pb::FileDescriptor build_file_descriptor(const LanceSchemaMapping& mapping, std::uint64_t rows,
                                         const LanceFileExtras* extras) {
    pb::FileDescriptor descriptor;
    descriptor.length = rows;
    // A Blob v2 field is stored as Lance stores it: the file's schema has the descriptor struct --
    // marked packed and blob, its children (kind, position, size, blob_id, blob_uri) without ids of
    // their own -- where the dataset's has the field's logical children.
    const auto blob_ids = blob_v2_parent_ids(mapping);
    const auto logical_blob = [&](std::int32_t id) {
        return std::find(blob_ids.begin(), blob_ids.end(), id) != blob_ids.end() &&
               std::none_of(mapping.fields.begin(), mapping.fields.end(),
                            [&](const LanceField& f) { return f.parent_id == id && f.name == "kind"; });
    };
    for (const auto& mapped_field : mapping.fields) {
        if (mapped_field.parent_id >= 0 && logical_blob(mapped_field.parent_id)) {
            continue;
        }
        if (logical_blob(mapped_field.id)) {
            pb::Field parent;
            parent.name = mapped_field.name;
            parent.logical_type = "struct";
            parent.id = mapped_field.id;
            parent.parent_id = mapped_field.parent_id;
            parent.type = 0;
            parent.nullable = mapped_field.nullable;
            parent.encoding = 0;
            parent.metadata["lance-encoding:blob"] = {'t', 'r', 'u', 'e'};
            parent.metadata["lance-encoding:packed"] = {'t', 'r', 'u', 'e'};
            descriptor.fields.push_back(std::move(parent));
            const std::pair<const char*, const char*> children[] = {
                {"kind", "uint8"}, {"position", "uint64"}, {"size", "uint64"}, {"blob_id", "uint32"},
                {"blob_uri", "string"}};
            for (const auto& [name, type] : children) {
                pb::Field child;
                child.name = name;
                child.logical_type = type;
                child.id = -1;
                child.parent_id = -1;
                child.type = 0;
                child.nullable = false;
                child.encoding = lance_on_disk_field_encoding(type);
                descriptor.fields.push_back(std::move(child));
            }
            continue;
        }
        pb::Field field;
        field.name = mapped_field.name;
        field.logical_type = lance_field_disk_logical_type(mapped_field);
        field.id = mapped_field.id;
        field.parent_id = mapped_field.parent_id;
        field.type = 2;
        field.nullable = mapped_field.nullable;
        field.encoding = mapped_field.dictionary_index_format.empty() ? lance_on_disk_field_encoding(mapped_field.logical_type)
                                                                      : 3;
        for (const auto& kv : mapped_field.metadata) {
            field.metadata[kv.first] = std::vector<std::uint8_t>(kv.second.begin(), kv.second.end());
        }
        descriptor.fields.push_back(std::move(field));
    }
    if (extras != nullptr) {
        descriptor.schema_metadata = extras->schema_metadata;
    }

    return descriptor;
}

/// Writes the column metadata, the offset tables and the footer, then flushes the file. Step 5 of
/// write_lance_data_file.
bool write_footer(std::ostream& out, const std::vector<pb::ColumnMetadata>& columns,
                  const std::vector<std::pair<std::uint64_t, std::uint64_t>>& extra_buffers,
                  std::uint64_t global_buffer_offset, const std::vector<std::uint8_t>& descriptor_bytes,
                  std::string& error) {
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
    for (const auto& [offset, size] : extra_buffers) {
        write_le64(out, offset);
        write_le64(out, size);
    }

    write_le64(out, column_metadata_start);
    write_le64(out, column_offsets_start);
    write_le64(out, global_offsets_start);
    write_le32(out, static_cast<std::uint32_t>(1U + extra_buffers.size()));
    write_le32(out, static_cast<std::uint32_t>(columns.size()));
    write_le16(out, 2);
    write_le16(out, 2);
    out.write("LANC", 4);
    out.flush();
    if (!out) {
        error = "failed to write Lance data file";
        return false;
    }

    return true;
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
                           std::string& error,
                           bool parallel_columns,
                           const LanceFileExtras* extras) {
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

    const auto relative_path =
        extras != nullptr && !extras->path.empty() ? extras->path : std::filesystem::path("data") / file_name;
    const auto full_path = extras != nullptr && !extras->path.empty() ? extras->path : dataset_path / relative_path;
    work_stats::WrittenFileTally tally(full_path);
    // A local file (its directory created), or an object uploaded on commit() (io::open_output).
    auto output = io::open_output(full_path.string(), error);
    if (!output) {
        return false;
    }
    std::ostream& out = *output;

    FileWriteState state{physical_fields, column_values,
                         std::vector<const ColumnValues*>(physical_fields.size()), {}, rows, compression_level,
                         compress};

    // Lance's blob preprocessing, now that the file's name is known: blobs given as bytes are placed --
    // inline ones here at the head of the file, before any column (their descriptors hold absolute
    // positions, which a column buffered on its own thread could not), packed and dedicated ones in
    // sidecar files beside it.
    if (!place_inline_blobs(state, full_path, extras, out, error)) {
        return false;
    }

    std::vector<pb::ColumnMetadata> columns;
    columns.reserve(physical_fields.size());
    if (!encode_columns(state, parallel_columns, out, columns, error)) {
        return false;
    }

    pb::FileDescriptor descriptor = build_file_descriptor(mapping, rows, extras);

    // Global buffers past the schema's: written before it, listed after it.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> extra_buffers;
    if (extras != nullptr) {
        for (const auto& buffer : extras->global_buffers) {
            align64(out);
            extra_buffers.emplace_back(pos(out), buffer.size());
            out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        }
    }

    const auto descriptor_bytes = pb::encode_file_descriptor(descriptor);
    align64(out);
    const auto global_buffer_offset = pos(out);
    out.write(reinterpret_cast<const char*>(descriptor_bytes.data()),
              static_cast<std::streamsize>(descriptor_bytes.size()));

    if (!write_footer(out, columns, extra_buffers, global_buffer_offset, descriptor_bytes, error)) {
        return false;
    }
    const auto file_size = pos(out);
    if (!output->commit(error)) {
        return false;
    }

    result.relative_path = relative_path;
    result.file_size_bytes = file_size;
    return true;
}

}  // namespace nano_lance
