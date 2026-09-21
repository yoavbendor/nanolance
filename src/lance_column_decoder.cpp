// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/lance_column_decoder.hpp"

#include "nanolance/page_layout.hpp"

#include "nanolance/blob_v2_external.hpp"
#include "nanolance/bool_bitpack.hpp"
#include "nanolance/byte_stream_split.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/fastlanes_bitpack.hpp"
#include "nanolance/read_safety.hpp"
#include "nanolance/schema_mapper.hpp"

#include <zstd.h>

#include <cstring>
#include <limits>
#include <algorithm>
#include <memory>
#include <optional>
#include <memory>
#include <utility>

namespace nano_lance {
namespace {

bool field_metadata_is_true(const pb::Field& field, const char* key) {
    const auto it = field.metadata.find(key);
    if (it == field.metadata.end()) {
        return false;
    }
    static const std::vector<std::uint8_t> kTrue{'t', 'r', 'u', 'e'};
    return it->second == kTrue;
}

bool field_metadata_equals(const pb::Field& field, const char* key, const char* value) {
    const auto it = field.metadata.find(key);
    if (it == field.metadata.end()) {
        return false;
    }
    const std::string v(it->second.begin(), it->second.end());
    return v == value;
}

const std::vector<std::uint8_t>* field_metadata_bytes(const pb::Field& field, const char* key) {
    const auto it = field.metadata.find(key);
    return it == field.metadata.end() ? nullptr : &it->second;
}

// Append `count` copies of an vlen-byte value via one resize + tight memcpy loop. Much leaner than
// count separate std::vector::insert calls (whose per-call machinery dominated the read profile).
// Returns false (without touching `out`) if the resulting size would overflow — `count` can derive
// from untrusted on-disk run/row counts, so the multiply must not wrap into a small allocation.
[[nodiscard]] bool append_repeated_value(std::vector<std::uint8_t>& out, const std::uint8_t* val,
                                         std::size_t vlen, std::size_t count) {
    if (count == 0U || vlen == 0U) {
        return true;
    }
    std::uint64_t added = 0;
    std::uint64_t new_size = 0;
    if (!checked_mul(vlen, count, added) || !checked_add(out.size(), added, new_size) ||
        !fits_size_t(new_size)) {
        return false;
    }
    const std::size_t base = out.size();
    out.resize(static_cast<std::size_t>(new_size));
    std::uint8_t* dst = out.data() + base;
    for (std::size_t i = 0; i < count; ++i) {
        std::memcpy(dst, val, vlen);
        dst += vlen;
    }
    return true;
}

// Inverse of zstd_frame_buffer: [u64 LE uncompressed size][zstd frame] -> raw bytes.
bool zstd_unframe_buffer(const std::vector<std::uint8_t>& framed, std::vector<std::uint8_t>& out, std::string& error) {
    if (framed.size() < 8U) {
        error = "zstd frame shorter than size header";
        return false;
    }
    const std::uint64_t uncompressed = load_le<std::uint64_t>(framed.data());
    // The declared uncompressed size is attacker-controlled: cap it (DoS/OOM) and never allocate more
    // than the platform can index. Also cross-check it against the zstd frame's own content size when
    // the frame records one, so a lie in the header can't drive a giant allocation.
    if (uncompressed > default_read_limits().max_uncompressed_bytes || !fits_size_t(uncompressed)) {
        error = "zstd uncompressed size exceeds safety limit";
        return false;
    }
    const unsigned long long content =
        ZSTD_getFrameContentSize(framed.data() + 8U, framed.size() - 8U);
    if (content != ZSTD_CONTENTSIZE_UNKNOWN && content != ZSTD_CONTENTSIZE_ERROR &&
        content != uncompressed) {
        error = "zstd frame content size disagrees with declared size";
        return false;
    }
    // resize() (not assign(n, 0)) so a reused `out` across many page calls of similar size isn't
    // re-zeroed every time — ZSTD_decompress below unconditionally overwrites all out.size() bytes.
    out.resize(static_cast<std::size_t>(uncompressed));
    // Reused per-thread decompression context: one-shot ZSTD_decompress() allocates and zeroes a fresh
    // context on every call; ZSTD_decompressDCtx on a reused context skips that setup per page (same
    // reuse rationale as the writer's zstd_frame_buffer, identical output).
    thread_local std::unique_ptr<ZSTD_DCtx, std::size_t (*)(ZSTD_DCtx*)> dctx(ZSTD_createDCtx(),
                                                                              &ZSTD_freeDCtx);
    const auto got = dctx != nullptr
                         ? ZSTD_decompressDCtx(dctx.get(), out.data(), out.size(), framed.data() + 8U,
                                               framed.size() - 8U)
                         : ZSTD_decompress(out.data(), out.size(), framed.data() + 8U, framed.size() - 8U);
    if (ZSTD_isError(got) != 0U || got != uncompressed) {
        error = "zstd decompress failed for variable-width column";
        return false;
    }
    return true;
}

bool read_le16(const std::uint8_t* p, std::uint16_t& v) {
    v = static_cast<std::uint16_t>(static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8U));
    return true;
}

// A miniblock chunk's 8-byte header is four little-endian u16 slots:
//
//   [0] number of values the repetition/definition layer covers -- 0 when the chunk has no repdef
//   [1] size in bytes of the first buffer
//   [2] size in bytes of the second buffer
//   [3] size in bytes of the third buffer
//
// with 0xFEFE marking a slot that is not in use. When slot 0 is zero the chunk holds values only and
// slot 1 is their size; when it is non-zero the chunk carries a definition-level buffer FIRST
// (slot 1) and the values after it (slot 2). Buffers are laid out back to back straight after the
// header, and the next chunk begins at the next 8-byte boundary.
//
// This was established by walking real pylance 12.0.0 output: the arithmetic below reproduces every
// chunk boundary of a 5000-row column exactly, across the plain, scattered-null and single-null
// cases. The previous reader hardcoded the no-repdef shape (slots 0 and 2 zero, slot 3 == 0xFEFE),
// which is why every nullable stock-Lance column failed with "unexpected miniblock payload prefix".
struct MiniBlockChunkHeader {
    std::uint16_t repdef_values = 0;
    std::uint16_t slots[3] = {0, 0, 0};

    bool has_repdef() const { return repdef_values != 0U; }
    /// Byte offset of the values buffer relative to the end of the header.
    std::size_t values_offset() const { return has_repdef() ? slots[0] : 0U; }
    std::size_t values_size() const { return has_repdef() ? slots[1] : slots[0]; }
    std::size_t repdef_size() const { return has_repdef() ? slots[0] : 0U; }
    /// Total payload bytes the chunk occupies after its header.
    std::size_t data_size() const {
        std::size_t total = 0;
        for (const auto slot : slots) {
            if (slot != 0xFEFEU) {
                total += slot;
            }
        }
        return total;
    }
};

bool read_miniblock_chunk_header(const std::vector<std::uint8_t>& payload, std::size_t offset,
                                 MiniBlockChunkHeader& out, std::string& error) {
    if (offset + 8U > payload.size()) {
        error = "truncated miniblock payload header";
        return false;
    }
    if (!read_le16(payload.data() + offset, out.repdef_values)) {
        error = "failed to read miniblock repdef value count";
        return false;
    }
    for (std::size_t i = 0; i < 3U; ++i) {
        if (!read_le16(payload.data() + offset + 2U + i * 2U, out.slots[i])) {
            error = "failed to read miniblock chunk size";
            return false;
        }
    }
    if (offset + 8U + out.data_size() > payload.size()) {
        error = "miniblock chunk exceeds payload";
        return false;
    }
    return true;
}

/// One decoded chunk: its values buffer, and its definition levels when it carries any.
struct MiniBlockChunkView {
    std::vector<std::uint8_t> values;
    std::vector<std::uint8_t> repdef;
    std::uint32_t repdef_values = 0;
};

/// Split a page's payload into its chunks without concatenating them.
///
/// A page is NOT one chunk. nanolance's own writer happens to emit exactly one chunk per page, which
/// is why treating the payload as a single chunk worked on its own files; stock Lance packs many
/// (a 5000-row int64 page arrives as five 1024-value chunks), so the concatenated buffer failed the
/// per-chunk size check with "bitpacked chunk size does not match bit width".
bool split_miniblock_payload(const std::vector<std::uint8_t>& payload, std::vector<MiniBlockChunkView>& out,
                             std::string& error) {
    out.clear();
    std::size_t offset = 0;
    while (offset < payload.size()) {
        MiniBlockChunkHeader header;
        if (!read_miniblock_chunk_header(payload, offset, header, error)) {
            return false;
        }
        const auto data_start = offset + 8U;
        MiniBlockChunkView chunk;
        if (header.has_repdef()) {
            chunk.repdef_values = header.repdef_values;
            chunk.repdef.assign(payload.begin() + static_cast<std::ptrdiff_t>(data_start),
                                payload.begin() + static_cast<std::ptrdiff_t>(data_start + header.repdef_size()));
        }
        const auto values_start = data_start + header.values_offset();
        chunk.values.assign(payload.begin() + static_cast<std::ptrdiff_t>(values_start),
                            payload.begin() + static_cast<std::ptrdiff_t>(values_start + header.values_size()));
        out.push_back(std::move(chunk));
        offset = (data_start + header.data_size() + 7U) & ~static_cast<std::size_t>(7U);
    }
    return true;
}

/// Concatenate every chunk's VALUES buffer, and (when `out_repdef` is given) every chunk's
/// definition-level buffer alongside it, with one entry per chunk in `out_repdef_counts` so the
/// caller knows how many values each repdef buffer covers.
bool parse_miniblock_payload_chunks(const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& out,
                                    std::string& error,
                                    std::vector<std::vector<std::uint8_t>>* out_repdef = nullptr,
                                    std::vector<std::uint32_t>* out_repdef_counts = nullptr) {
    out.clear();
    if (out_repdef != nullptr) {
        out_repdef->clear();
    }
    if (out_repdef_counts != nullptr) {
        out_repdef_counts->clear();
    }
    std::size_t offset = 0;
    while (offset < payload.size()) {
        MiniBlockChunkHeader header;
        if (!read_miniblock_chunk_header(payload, offset, header, error)) {
            return false;
        }
        const auto data_start = offset + 8U;
        if (header.has_repdef()) {
            if (out_repdef == nullptr) {
                error = "miniblock chunk carries definition levels on a path that cannot use them";
                return false;
            }
            out_repdef->emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(data_start),
                                     payload.begin() +
                                         static_cast<std::ptrdiff_t>(data_start + header.repdef_size()));
            if (out_repdef_counts != nullptr) {
                out_repdef_counts->push_back(header.repdef_values);
            }
        }
        const auto values_start = data_start + header.values_offset();
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(values_start),
                   payload.begin() + static_cast<std::ptrdiff_t>(values_start + header.values_size()));
        offset = (data_start + header.data_size() + 7U) & ~static_cast<std::size_t>(7U);
    }
    return true;
}

bool parse_miniblock_payload_chunk_list(const std::vector<std::uint8_t>& payload,
                                        std::vector<std::vector<std::uint8_t>>& out, std::string& error) {
    out.clear();
    std::size_t offset = 0;
    while (offset < payload.size()) {
        if (offset + 8U > payload.size()) {
            error = "truncated miniblock payload header";
            return false;
        }
        if (payload[offset] != 0U || payload[offset + 1U] != 0U) {
            error = "unexpected miniblock payload prefix";
            return false;
        }
        std::uint16_t chunk_size = 0;
        if (!read_le16(payload.data() + offset + 2U, chunk_size)) {
            error = "failed to read miniblock chunk size";
            return false;
        }
        if (payload[offset + 6U] != 0xFEU || payload[offset + 7U] != 0xFEU) {
            error = "unexpected miniblock payload marker";
            return false;
        }
        const auto data_start = offset + 8U;
        const auto data_end = data_start + static_cast<std::size_t>(chunk_size);
        if (data_end > payload.size()) {
            error = "miniblock chunk exceeds payload";
            return false;
        }
        out.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(data_start),
                         payload.begin() + static_cast<std::ptrdiff_t>(data_end));
        offset = (data_end + 7U) & ~static_cast<std::size_t>(7U);
    }
    return true;
}

auto read_list_offset = [](const std::vector<std::uint8_t>& bytes, std::size_t index, bool large) -> std::int64_t {
    const auto offset_width = large ? 8U : 4U;
    const auto* src = bytes.data() + index * offset_width;
    if (large) {
        std::int64_t v = 0;
        std::memcpy(&v, src, sizeof(v));
        return v;
    }
    std::int32_t v = 0;
    std::memcpy(&v, src, sizeof(v));
    return static_cast<std::int64_t>(v);
};

void append_list_offset(std::vector<std::uint8_t>& out_offsets, std::int64_t value, bool large) {
    if (large) {
        const auto write_at = out_offsets.size();
        out_offsets.resize(write_at + 8U);
        std::memcpy(out_offsets.data() + write_at, &value, sizeof(value));
        return;
    }
    const auto write_at = out_offsets.size();
    out_offsets.resize(write_at + 4U);
    const auto stored = static_cast<std::int32_t>(value);
    std::memcpy(out_offsets.data() + write_at, &stored, sizeof(stored));
}

bool decode_variable_width_page(const std::vector<std::uint8_t>& chunk_bytes, const std::uint64_t num_values,
                                const bool large, std::vector<std::uint8_t>& out_offsets,
                                std::vector<std::uint8_t>& out_data, std::string& error) {
    if (num_values == 0U) {
        return true;
    }
    const auto offset_width = large ? 8U : 4U;
    // num_values comes from the untrusted page.length: compute the offset-table size with overflow-safe
    // math so a hostile count can't wrap `(num_values + 1) * offset_width` into a small value that then
    // passes the bounds check below and lets read_list_offset() over-read the chunk.
    std::uint64_t offset_entries = 0;
    std::uint64_t offsets_bytes64 = 0;
    if (!checked_add(num_values, 1U, offset_entries) ||
        !checked_mul(offset_entries, offset_width, offsets_bytes64) ||
        !fits_size_t(offsets_bytes64)) {
        error = "variable-width offset table size overflow";
        return false;
    }
    const auto offsets_bytes = static_cast<std::size_t>(offsets_bytes64);
    if (chunk_bytes.size() < offsets_bytes) {
        error = "variable-width chunk smaller than offset table";
        return false;
    }

    const auto data_base_in_chunk = read_list_offset(chunk_bytes, 0, large);
    if (data_base_in_chunk < 0 ||
        static_cast<std::size_t>(data_base_in_chunk) > chunk_bytes.size()) {
        error = "variable-width chunk data base out of range";
        return false;
    }
    // Append only the value bytes [data_base, terminal_offset); the chunk is padded to 8 bytes at the
    // end, and including that padding would misalign every subsequent page's data.
    const auto data_end_in_chunk = read_list_offset(chunk_bytes, num_values, large);
    if (data_end_in_chunk < data_base_in_chunk ||
        static_cast<std::size_t>(data_end_in_chunk) > chunk_bytes.size()) {
        error = "variable-width chunk terminal offset out of range";
        return false;
    }

    // Validate every row's [start,end) lies within [data_base_in_chunk, data_end_in_chunk) and is
    // non-decreasing (same checks a per-row copy loop would make), as a pre-pass so the actual byte copy
    // below can be one bulk insert instead of one insert per row -- the per-row insert dominated the
    // read profile for every page after a variable-width column's first (only the first page took a
    // bulk-copy fast path; every later page fell into the slow per-row loop).
    std::int64_t prev = data_base_in_chunk;
    for (std::uint64_t i = 1; i <= num_values; ++i) {
        const auto off = read_list_offset(chunk_bytes, i, large);
        if (off < prev || static_cast<std::size_t>(off) > chunk_bytes.size()) {
            error = "variable-width chunk string bounds out of range";
            return false;
        }
        prev = off;
    }

    const auto cumulative_base = static_cast<std::int64_t>(out_data.size());
    out_data.insert(out_data.end(), chunk_bytes.begin() + static_cast<std::ptrdiff_t>(data_base_in_chunk),
                    chunk_bytes.begin() + static_cast<std::ptrdiff_t>(data_end_in_chunk));

    // First page for this column: also emit the leading offset 0 (i=0); later pages continue an
    // already-started offsets buffer, so only the per-row terminal offsets (i=1..num_values) are new.
    for (std::uint64_t i = (out_offsets.empty() ? 0U : 1U); i <= num_values; ++i) {
        const auto off = read_list_offset(chunk_bytes, i, large) - data_base_in_chunk;
        append_list_offset(out_offsets, cumulative_base + off, large);
    }
    return true;
}

// Decode one bitpacked page chunk ([bit_width word][FastLanes packed 1024]) into `num_values`
// little-endian fixed-width values appended to out_fixed.
template <class T>
bool unpack_bitpacked_page(const std::vector<std::uint8_t>& chunk, std::uint64_t num_values,
                           std::vector<std::uint8_t>& out_fixed, std::string& error) {
    if (chunk.size() < sizeof(T)) {
        error = "bitpacked chunk shorter than width header";
        return false;
    }
    T width_word = 0;
    std::memcpy(&width_word, chunk.data(), sizeof(T));
    const unsigned width = static_cast<unsigned>(width_word);
    if (width > sizeof(T) * 8U) {
        error = "bitpacked chunk has invalid bit width";
        return false;
    }
    const auto packed_words = nano_lance::fastlanes::packed_words_1024<T>(width);
    if (chunk.size() != sizeof(T) * (1U + packed_words)) {
        error = "bitpacked chunk size does not match bit width";
        return false;
    }
    // thread_local + resize (not a fresh (packed_words, 0)-initialized vector every call): packed_words is
    // bounded by <=1024 (one FastLanes block), so this reused buffer's capacity converges after the first
    // max-sized call, and resize() only zero-inits a growing delta rather than the whole buffer every time
    // — the memcpy right below unconditionally overwrites all packed_words elements regardless.
    thread_local std::vector<T> packed;
    packed.resize(packed_words);
    if (packed_words != 0U) {
        std::memcpy(packed.data(), chunk.data() + sizeof(T), packed_words * sizeof(T));
    }
    // A FastLanes chunk unpacks into exactly 1024 values. `num_values` is attacker-controlled (it comes
    // from page.length on the untrusted read path), so it MUST NOT exceed the block size — otherwise the
    // copy below over-reads the `values` stack buffer. The last chunk legitimately emits fewer.
    if (num_values > 1024U) {
        error = "bitpacked chunk value count exceeds FastLanes block size";
        return false;
    }
    T values[1024];
    nano_lance::fastlanes::unpack_1024<T>(width, packed.data(), values);
    const auto bytes = static_cast<std::size_t>(num_values) * sizeof(T);
    const auto* p = reinterpret_cast<const std::uint8_t*>(values);
    out_fixed.insert(out_fixed.end(), p, p + bytes);
    return true;
}

bool unpack_bitpacked_page_dispatch(const std::vector<std::uint8_t>& chunk, std::uint64_t num_values,
                                    std::size_t bytes_per_value, std::vector<std::uint8_t>& out_fixed,
                                    std::string& error) {
    switch (bytes_per_value) {
        case 1U:
            return unpack_bitpacked_page<std::uint8_t>(chunk, num_values, out_fixed, error);
        case 2U:
            return unpack_bitpacked_page<std::uint16_t>(chunk, num_values, out_fixed, error);
        case 4U:
            return unpack_bitpacked_page<std::uint32_t>(chunk, num_values, out_fixed, error);
        default:
            return unpack_bitpacked_page<std::uint64_t>(chunk, num_values, out_fixed, error);
    }
}



/// Inverse of the writer's encode_scalar_variable_value: a Lance scalar value buffer holding a
/// length-1 string/binary array, laid out as [u32 num_buffers][u32 buffer_len ...][buffers]. For
/// utf8/binary that is two buffers -- offsets [0, len] and the data -- and the value we want is the
/// data buffer whole. Every length is untrusted, so each is bounds-checked before use.
[[nodiscard]] bool decode_scalar_variable_value(const std::vector<std::uint8_t>& buffer,
                                                std::vector<std::uint8_t>& out, std::string& error) {
    if (buffer.size() < 4U) {
        error = "constant value buffer shorter than its header";
        return false;
    }
    const auto num_buffers = load_le<std::uint32_t>(buffer.data());
    if (num_buffers != 2U) {
        error = "constant value buffer declares " + std::to_string(num_buffers) +
                " buffers; expected 2 (offsets + data)";
        return false;
    }
    std::uint64_t header = 0;
    if (!checked_mul(num_buffers, 4U, header) || !checked_add(header, 4U, header) || header > buffer.size()) {
        error = "constant value buffer header exceeds the buffer";
        return false;
    }
    const auto offsets_len = load_le<std::uint32_t>(buffer.data() + 4U);
    const auto data_len = load_le<std::uint32_t>(buffer.data() + 8U);
    std::uint64_t data_start = 0;
    std::uint64_t data_end = 0;
    if (!checked_add(header, offsets_len, data_start) || !checked_add(data_start, data_len, data_end) ||
        data_end > buffer.size()) {
        error = "constant value buffer's data range exceeds the buffer";
        return false;
    }
    out.assign(buffer.begin() + static_cast<std::ptrdiff_t>(data_start),
               buffer.begin() + static_cast<std::ptrdiff_t>(data_end));
    return true;
}


/// Decode one chunk's definition-level buffer, appending one bit per row to `out_validity` (an
/// Arrow-convention bitmap: LSB-first, bit SET means VALID) and counting the nulls.
///
/// Lance stores one definition level per value, and for a simple nullable column **level 1 means
/// NULL** -- the opposite polarity to Arrow's validity bit, hence the inversion. The levels are
/// FastLanes-bit-packed at the width named by the descriptor's inner Flat(N), which is 1 when the
/// only levels are 0 and 1. That is the same kernel integer columns already use, so no new decoding
/// happens here; what was missing was knowing where to point it.
///
/// Verified against pylance 12.0.0: this reproduces a 1024-row alternating null pattern bit for bit,
/// and a scattered 10%-null pattern both bit for bit and in count (103 of 1024).
[[nodiscard]] bool append_definition_levels(const std::vector<std::uint8_t>& repdef,
                                            const page_layout::Compressive& encoding, std::uint32_t count,
                                            std::uint64_t rows_already_appended,
                                            std::vector<std::uint8_t>& out_validity,
                                            std::uint64_t& out_null_count, std::string& error) {
    if (count > 1024U) {
        error = "definition-level chunk covers more than one FastLanes block";
        return false;
    }
    if (encoding.kind != page_layout::CompressiveKind::kBitpacked || encoding.values == nullptr ||
        encoding.values->kind != page_layout::CompressiveKind::kFlat) {
        error = "unsupported definition-level encoding (only a bit-packed level buffer is decoded "
                "today; run-length-encoded levels are not)";
        return false;
    }
    const auto width = encoding.values->bits_per_value;
    if (width == 0U || width > 16U) {
        error = "definition levels declare an unsupported width of " + std::to_string(width) + " bits";
        return false;
    }
    const auto packed_words = nano_lance::fastlanes::packed_words_1024<std::uint16_t>(width);
    if (repdef.size() != packed_words * sizeof(std::uint16_t)) {
        error = "definition-level buffer is " + std::to_string(repdef.size()) + " bytes, expected " +
                std::to_string(packed_words * sizeof(std::uint16_t));
        return false;
    }
    std::vector<std::uint16_t> packed(packed_words);
    std::memcpy(packed.data(), repdef.data(), repdef.size());
    std::uint16_t levels[1024];
    nano_lance::fastlanes::unpack_1024<std::uint16_t>(width, packed.data(), levels);

    // Rows accumulate across chunks into one contiguous bitmap, so a chunk whose row count is not a
    // multiple of 8 leaves the next chunk starting mid-byte. Grow to cover the new rows, then set
    // each valid bit at its absolute row index.
    const auto total_rows = rows_already_appended + count;
    out_validity.resize(static_cast<std::size_t>((total_rows + 7U) / 8U), 0U);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (levels[i] != 0U) {
            ++out_null_count;  // level 1 == null; the bit stays clear
            continue;
        }
        const auto row = rows_already_appended + i;
        out_validity[static_cast<std::size_t>(row >> 3U)] |= static_cast<std::uint8_t>(1U << (row & 7U));
    }
    return true;
}

// ── Encoding selection ───────────────────────────────────────────────────────────────────────────
//
// Which decode path a column takes used to be read off nanolance-private field metadata
// (`nanolance:packing`). The writer also records the same fact in each page's
// /lance.encodings21.PageLayout descriptor, in the form every Lance implementation uses -- and a file
// from the Rust lance crate carries ONLY that. Selecting from the descriptor is therefore what makes
// those files reachable; tests/test_page_layout.cpp is the differential oracle proving the two
// signals agree across the writer's whole output space.
//
// The metadata remains the fallback for a page with no descriptor, so files written before the
// descriptor was read back keep decoding unchanged.

enum class ColumnEncodingKind {
    kBlobPacked,
    kConstant,
    kRle,
    kDictRle,
    kDict,
    kBssZstd,
    kBoolPacked,
    kVariable,
    kBitpack,
    kFlat,
    kUnsupported,
};

struct ColumnEncodingPlan {
    ColumnEncodingKind kind = ColumnEncodingKind::kFlat;
    bool zstd = false;  // variable-width only: value bytes are [u64 len][zstd frame]
    /// kConstant: the repeated value, when the descriptor carried it inline. A fixed-width constant
    /// page stores its value here and has no data buffers at all; a variable-width one leaves this
    /// empty and puts the value in the page's single buffer. Reading it from the descriptor rather
    /// than from `nanolance:const-value` is what lets a constant column from any writer decode.
    std::optional<std::vector<std::uint8_t>> constant_inline_value;
    /// kMiniBlock with a definition-level layer: how those levels are encoded. Null when the column
    /// has no nulls, which is the common case and costs nothing.
    std::shared_ptr<page_layout::Compressive> repdef;
    /// kConstant whose layers declare definition levels and which carries no value: Lance's spelling
    /// of a column where every row is null.
    bool constant_all_null = false;
    /// Set when the descriptor named something this build does not model, so the error can say what.
    std::string unsupported_reason;
};

/// Classify from the page descriptor alone. Returns false when the column carries no descriptor (an
/// older nanolance file), leaving the caller to fall back to the field metadata.
bool classify_from_descriptor(const pb::ColumnMetadata& column_metadata, ColumnEncodingPlan& out) {
    if (column_metadata.pages.empty() || column_metadata.pages.front().encoding.empty()) {
        return false;
    }
    page_layout::PageLayout layout;
    std::string parse_error;
    if (!page_layout::decode_page_layout(column_metadata.pages.front().encoding, layout, parse_error)) {
        out.kind = ColumnEncodingKind::kUnsupported;
        out.unsupported_reason = parse_error;
        return true;
    }
    if (layout.kind == page_layout::LayoutKind::kConstant) {
        out.kind = ColumnEncodingKind::kConstant;
        out.constant_inline_value = layout.constant.inline_value;
        // A ConstantLayout that declares definition levels and holds no value is how Lance writes a
        // column whose every row is null -- there is no repeated value to store, only the fact that
        // there is none.
        out.constant_all_null =
            !layout.constant.inline_value && page_layout::layers_have_definition_levels(layout.constant.layers);
        return true;
    }
    if (layout.kind != page_layout::LayoutKind::kMiniBlock) {
        out.kind = ColumnEncodingKind::kUnsupported;
        out.unsupported_reason = "unsupported page layout: " + page_layout::describe(layout);
        return true;
    }

    if (page_layout::layers_have_definition_levels(layout.mini_block.layers)) {
        if (layout.mini_block.repdef_compression == nullptr) {
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason = "page declares definition levels but no encoding for them";
            return true;
        }
        out.repdef = std::shared_ptr<page_layout::Compressive>(layout.mini_block.repdef_compression.release());
    }

    const auto* values = layout.mini_block.value_compression.get();
    if (values == nullptr) {
        out.kind = ColumnEncodingKind::kUnsupported;
        out.unsupported_reason = "page layout has no value compression";
        return true;
    }
    const bool has_dictionary = layout.mini_block.dictionary != nullptr;

    // General{scheme, inner} is a wrapper: unwrap it and remember whether it compresses.
    const page_layout::Compressive* inner = values;
    if (inner->kind == page_layout::CompressiveKind::kGeneral) {
        out.zstd = inner->scheme == page_layout::BufferScheme::kZstd;
        if (inner->values == nullptr) {
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason = "General encoding has no inner values";
            return true;
        }
        if (inner->values->kind == page_layout::CompressiveKind::kByteStreamSplit) {
            out.kind = ColumnEncodingKind::kBssZstd;
            return true;
        }
        inner = inner->values.get();
    }

    switch (inner->kind) {
        case page_layout::CompressiveKind::kRle:
            out.kind = has_dictionary ? ColumnEncodingKind::kDictRle : ColumnEncodingKind::kRle;
            return true;
        case page_layout::CompressiveKind::kInlineBitpacking:
            out.kind = has_dictionary ? ColumnEncodingKind::kDict : ColumnEncodingKind::kBitpack;
            return true;
        case page_layout::CompressiveKind::kVariable:
            out.kind = ColumnEncodingKind::kVariable;
            return true;
        case page_layout::CompressiveKind::kFlat:
            // bool is Flat{bits_per_value: 1}; that IS how stock Lance represents it, so the
            // descriptor distinguishes it from a byte-wide flat column with no help from metadata.
            out.kind = inner->bits_per_value == 1U ? ColumnEncodingKind::kBoolPacked : ColumnEncodingKind::kFlat;
            return true;
        default:
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason =
                "unsupported encoding " + page_layout::describe(layout) +
                " (CompressiveEncoding variant " + std::to_string(inner->wire_field) + ")";
            return true;
    }
}

/// The legacy classification, from nanolance-private field metadata. Kept as the fallback.
ColumnEncodingPlan classify_from_metadata(const pb::Field& field) {
    ColumnEncodingPlan plan;
    if (field_metadata_equals(field, "nanolance:packing", "constant")) {
        plan.kind = ColumnEncodingKind::kConstant;
    } else if (field_metadata_equals(field, "nanolance:packing", "rle")) {
        plan.kind = ColumnEncodingKind::kRle;
    } else if (field_metadata_equals(field, "nanolance:packing", "dict-rle")) {
        plan.kind = ColumnEncodingKind::kDictRle;
    } else if (field_metadata_equals(field, "nanolance:packing", "dict")) {
        plan.kind = ColumnEncodingKind::kDict;
    } else if (field_metadata_equals(field, "nanolance:packing", "bss-zstd")) {
        plan.kind = ColumnEncodingKind::kBssZstd;
    } else if (field.logical_type == "bool") {
        plan.kind = ColumnEncodingKind::kBoolPacked;
    } else if (field.encoding == 2) {
        plan.kind = ColumnEncodingKind::kVariable;
        plan.zstd = field_metadata_equals(field, "lance-encoding:compression", "zstd");
    } else if (field_metadata_equals(field, "nanolance:packing", "bitpack")) {
        plan.kind = ColumnEncodingKind::kBitpack;
    } else {
        plan.kind = ColumnEncodingKind::kFlat;
    }
    return plan;
}

ColumnEncodingPlan classify_column_encoding(const pb::Field& field, const pb::ColumnMetadata& column_metadata) {
    ColumnEncodingPlan plan;
    if (classify_from_descriptor(column_metadata, plan)) {
        return plan;
    }
    return classify_from_metadata(field);
}

bool read_page_buffers(const std::filesystem::path& path, const pb::ColumnPage& page, bool blob_layout,
                       std::vector<std::uint8_t>& first, std::vector<std::uint8_t>& second, std::string& error) {
    if (page.buffer_offsets.size() < 2U || page.buffer_sizes.size() < 2U) {
        error = "column page is missing buffer offsets";
        return false;
    }
    const auto first_offset = page.buffer_offsets[blob_layout ? 1U : 0U];
    const auto first_size = page.buffer_sizes[blob_layout ? 1U : 0U];
    const auto second_offset = page.buffer_offsets[blob_layout ? 0U : 1U];
    const auto second_size = page.buffer_sizes[blob_layout ? 0U : 1U];
    if (!read_lance_data_file_bytes(path, first_offset, first_size, first, error)) {
        return false;
    }
    return read_lance_data_file_bytes(path, second_offset, second_size, second, error);
}


/// Read one page's chunks, appending each chunk's definition levels to `out` when the column has
/// them. Returns the chunks so the caller can decode their values.
[[nodiscard]] bool read_page_chunks_with_validity(const std::vector<std::uint8_t>& payload,
                                                  const ColumnEncodingPlan& plan, std::uint64_t page_rows,
                                                  std::vector<MiniBlockChunkView>& chunks,
                                                  std::uint64_t& validity_rows, ColumnValues& out,
                                                  std::string& error) {
    if (!split_miniblock_payload(payload, chunks, error)) {
        return false;
    }
    if (plan.repdef == nullptr) {
        return true;
    }
    std::uint64_t covered = 0;
    for (const auto& chunk : chunks) {
        if (chunk.repdef.empty()) {
            error = "column declares definition levels but a chunk carries none";
            return false;
        }
        if (!append_definition_levels(chunk.repdef, *plan.repdef, chunk.repdef_values, validity_rows,
                                      out.validity, out.null_count, error)) {
            return false;
        }
        validity_rows += chunk.repdef_values;
        covered += chunk.repdef_values;
    }
    if (covered != page_rows) {
        error = "definition levels cover " + std::to_string(covered) + " of the page's " +
                std::to_string(page_rows) + " rows";
        return false;
    }
    return true;
}

}  // namespace

bool decode_lance_physical_column(const std::filesystem::path& data_file_path, const pb::Field& on_disk_field,
                                  const pb::ColumnMetadata& column_metadata, ColumnValues& out, std::string& error) {
    error.clear();
    out = ColumnValues{};

    // The per-page row count comes from the untrusted protobuf. Bound the declared total up front
    // (overflow-safe) so no decode branch below can be tricked into a runaway allocation; the inner
    // loops then run without re-checking.
    std::uint64_t declared_rows = 0;
    for (const auto& page : column_metadata.pages) {
        if (!checked_add(declared_rows, page.length, declared_rows)) {
            error = "column page row count overflows";
            return false;
        }
    }
    if (declared_rows > default_read_limits().max_rows_per_column) {
        error = "column row count exceeds safety limit";
        return false;
    }

    // `lance-encoding:blob` is checked FIRST and stays a metadata check. It is a Lance-standard key
    // (stock Lance writes it too) describing the COLUMN's role rather than a page's encoding, and a
    // blob-v2 packed page uses FullZipLayout (PageLayout field 3), which the descriptor classifier
    // below does not model -- classifying first would refuse the library's headline feature.
    const bool blob_packed = field_metadata_is_true(on_disk_field, "lance-encoding:blob");
    if (blob_packed) {
        out.kind = ColumnValues::Kind::BlobV2External;
        // Hoisted out of the loop (not freshly declared per page): read_lance_data_file_bytes reuses
        // whatever capacity/bytes are already here rather than re-zeroing a fresh buffer every page.
        std::vector<std::uint8_t> control;
        std::vector<std::uint8_t> values;
        for (const auto& page : column_metadata.pages) {
            if (!read_page_buffers(data_file_path, page, true, control, values, error)) {
                return false;
            }
            std::vector<std::uint32_t> row_sizes;
            if (!blob_v2_control_buffer_to_row_sizes(control, page.length, row_sizes, error)) {
                return false;
            }
            std::size_t offset = 0;
            for (const auto row_size : row_sizes) {
                if (offset + row_size > values.size()) {
                    error = "blob packed row exceeds values buffer";
                    return false;
                }
                out.blob_v2.packed_payload.insert(
                    out.blob_v2.packed_payload.end(),
                    values.begin() + static_cast<std::ptrdiff_t>(offset),
                    values.begin() + static_cast<std::ptrdiff_t>(offset + row_size));
                out.blob_v2.row_packed_sizes.push_back(row_size);
                offset += row_size;
            }
            if (offset != values.size()) {
                error = "blob values buffer has trailing bytes";
                return false;
            }
        }
        return true;
    }

    // Constant column: the single value is stored once in field metadata; expand to one value per row.
    // Selected from the page descriptor when there is one, from the legacy field metadata otherwise.
    const auto encoding_plan = classify_column_encoding(on_disk_field, column_metadata);
    if (encoding_plan.kind == ColumnEncodingKind::kUnsupported) {
        // Refuse by name, before a single buffer byte is interpreted. The old code had no way to do
        // this: with no descriptor read, an encoding it did not implement fell through to the flat
        // path and was misparsed, surfacing later as a size mismatch -- which is why a 3-row
        // stock-Lance table decoded and a 5000-row one did not.
        error = "column '" + on_disk_field.name + "': " + encoding_plan.unsupported_reason;
        return false;
    }

    if (encoding_plan.kind == ColumnEncodingKind::kConstant && encoding_plan.constant_all_null) {
        // Every row is null. There are no value bytes on disk, so materialize the column's zeroed
        // storage and an all-clear validity bitmap.
        std::uint64_t total_rows = 0;
        for (const auto& page : column_metadata.pages) {
            total_rows += page.length;
        }
        const bool variable = lance_field_is_variable_width(on_disk_field.logical_type);
        out.kind = variable ? ColumnValues::Kind::VariableWidth : ColumnValues::Kind::FixedWidth;
        if (variable) {
            out.variable.large = lance_logical_type_has_large_offsets(on_disk_field.logical_type);
            const auto offset_width = out.variable.large ? 8U : 4U;
            out.variable.offsets.assign(static_cast<std::size_t>(total_rows + 1U) * offset_width, 0U);
        } else {
            std::string internal = on_disk_field.logical_type;
            if (internal == "string") {
                internal = "utf8";
            }
            const auto bpv = lance_logical_type_value_bytes(internal);
            std::uint64_t bytes = 0;
            if (!checked_mul(total_rows, static_cast<std::uint64_t>(bpv), bytes) || !fits_size_t(bytes)) {
                error = "all-null column size overflows";
                return false;
            }
            out.fixed.assign(static_cast<std::size_t>(bytes), 0U);
        }
        out.validity.assign(static_cast<std::size_t>((total_rows + 7U) / 8U), 0U);
        out.null_count = total_rows;
        return true;
    }

    if (encoding_plan.kind == ColumnEncodingKind::kConstant) {
        // Resolve the repeated value, preferring the sources any writer produces over nanolance's
        // own metadata: the descriptor's inline value (fixed-width), then the page's single data
        // buffer (variable-width), then `nanolance:const-value` for files predating the descriptor
        // being read back.
        std::vector<std::uint8_t> constant_value;
        const std::vector<std::uint8_t>* value = nullptr;
        if (encoding_plan.constant_inline_value) {
            constant_value = *encoding_plan.constant_inline_value;
            value = &constant_value;
        } else if (!column_metadata.pages.empty() && !column_metadata.pages.front().buffer_offsets.empty() &&
                   !column_metadata.pages.front().buffer_sizes.empty()) {
            const auto& page = column_metadata.pages.front();
            std::vector<std::uint8_t> scalar_buffer;
            if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[0], page.buffer_sizes[0],
                                            scalar_buffer, error) ||
                !decode_scalar_variable_value(scalar_buffer, constant_value, error)) {
                return false;
            }
            value = &constant_value;
        } else {
            value = field_metadata_bytes(on_disk_field, "nanolance:const-value");
        }
        if (value == nullptr) {
            error = "constant column '" + on_disk_field.name +
                    "' has no value: the page descriptor carries none inline, the page has no data "
                    "buffer, and nanolance:const-value is absent";
            return false;
        }
        std::uint64_t total_rows = 0;
        for (const auto& page : column_metadata.pages) {
            total_rows += page.length;
        }
        if (on_disk_field.encoding == 2) {  // variable-width
            out.kind = ColumnValues::Kind::VariableWidth;
            out.variable.large = lance_logical_type_has_large_offsets(on_disk_field.logical_type);
            const std::size_t len = value->size();
            const auto rows = static_cast<std::size_t>(total_rows);
            if (!append_repeated_value(out.variable.data, value->data(), len, rows)) {  // every row = value
                error = "constant column expansion overflows";
                return false;
            }
            // offsets are arithmetic (0, len, 2*len, ...); build typed then one bulk copy.
            if (out.variable.large) {
                std::vector<std::uint64_t> offs(rows + 1U);
                for (std::size_t i = 0; i <= rows; ++i) offs[i] = static_cast<std::uint64_t>(i) * len;
                out.variable.offsets.resize(offs.size() * 8U);
                std::memcpy(out.variable.offsets.data(), offs.data(), offs.size() * 8U);
            } else {
                std::vector<std::uint32_t> offs(rows + 1U);
                for (std::size_t i = 0; i <= rows; ++i) offs[i] = static_cast<std::uint32_t>(i * len);
                out.variable.offsets.resize(offs.size() * 4U);
                std::memcpy(out.variable.offsets.data(), offs.data(), offs.size() * 4U);
            }
            return true;
        }
        out.kind = ColumnValues::Kind::FixedWidth;
        if (!append_repeated_value(out.fixed, value->data(), value->size(),
                                   static_cast<std::size_t>(total_rows))) {
            error = "constant column expansion overflows";
            return false;
        }
        return true;
    }

    // Run-length encoded fixed-width column: one chunk with two buffers (run values + run lengths).
    if (encoding_plan.kind == ColumnEncodingKind::kRle) {
        out.kind = ColumnValues::Kind::FixedWidth;
        std::string internal = on_disk_field.logical_type;
        if (internal == "string") {
            internal = "utf8";
        }
        const auto bpv = lance_logical_type_value_bytes(internal);
        const std::size_t length_bytes = 1U;  // Lance RLE uses 8-bit run lengths
        // Reserve the whole column upfront: without this, each run's append_repeated_value() call
        // resize()s out.fixed to an exact new size (no growth slack), so libstdc++ reallocates and
        // re-copies everything already written on essentially every run -- O(n^2) memcpy for a column
        // with many runs. declared_rows is already validated against the safety limit above.
        std::uint64_t reserve_bytes = 0;
        if (checked_mul(declared_rows, static_cast<std::uint64_t>(bpv), reserve_bytes) && fits_size_t(reserve_bytes)) {
            out.fixed.reserve(static_cast<std::size_t>(reserve_bytes));
        }
        std::vector<std::uint8_t> control;
        std::vector<std::uint8_t> data;
        for (const auto& page : column_metadata.pages) {
            if (!read_page_buffers(data_file_path, page, false, control, data, error)) {
                return false;
            }
            if (data.size() < 10U) {
                error = "rle chunk too short";
                return false;
            }
            std::uint32_t size0 = 0;
            std::uint32_t size1 = 0;
            std::memcpy(&size0, data.data() + 2U, 4U);
            std::memcpy(&size1, data.data() + 6U, 4U);
            std::size_t off = 10U;
            off += (8U - (off % 8U)) % 8U;  // pad to 8 after the [num_levels][size0][size1] header
            const std::size_t values_off = off;
            std::size_t lengths_off = values_off + size0;
            lengths_off += (8U - (lengths_off % 8U)) % 8U;
            if (lengths_off + size1 > data.size() || size0 % bpv != 0U || length_bytes == 0U ||
                size1 % length_bytes != 0U) {
                error = "rle chunk buffer sizes invalid";
                return false;
            }
            const std::size_t num_runs = size0 / bpv;
            if (num_runs != size1 / length_bytes) {
                error = "rle run count mismatch between values and lengths";
                return false;
            }
            auto run_length_at = [&](std::size_t r) -> std::uint64_t {
                std::uint64_t run = 0;
                for (std::size_t k = 0; k < length_bytes; ++k) {
                    run |= static_cast<std::uint64_t>(data[lengths_off + r * length_bytes + k]) << (8U * k);
                }
                return run;
            };
            for (std::size_t r = 0; r < num_runs; ++r) {
                const std::uint64_t run = run_length_at(r);
                const auto* vptr = data.data() + values_off + r * bpv;
                if (!append_repeated_value(out.fixed, vptr, bpv, static_cast<std::size_t>(run))) {
                    error = "rle run expansion overflows";
                    return false;
                }
            }
        }
        return true;
    }

    // Dictionary + RLE variable-width column: buffer[1] = RLE'd u32 indices, buffer[2] = dictionary.
    if (encoding_plan.kind == ColumnEncodingKind::kDictRle) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = lance_logical_type_has_large_offsets(on_disk_field.logical_type);
        std::vector<std::uint8_t> data;       // buffer[1]: RLE chunk of indices
        std::vector<std::uint8_t> dict_frame;  // buffer[2]: dictionary
        std::vector<std::uint8_t> dict_block;
        for (const auto& page : column_metadata.pages) {
            if (page.buffer_offsets.size() < 3U || page.buffer_sizes.size() < 3U) {
                error = "dict-rle page missing buffers";
                return false;
            }
            if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[1], page.buffer_sizes[1], data, error) ||
                !read_lance_data_file_bytes(data_file_path, page.buffer_offsets[2], page.buffer_sizes[2], dict_frame,
                                            error)) {
                return false;
            }
            // Decode the dictionary: un-zstd -> [u32 32][u32 bytes_start][u32 offsets][data].
            if (!zstd_unframe_buffer(dict_frame, dict_block, error)) {
                return false;
            }
            if (dict_block.size() < 8U) {
                error = "dict block too short";
                return false;
            }
            std::uint32_t bytes_start = 0;
            std::memcpy(&bytes_start, dict_block.data() + 4U, 4U);
            if (bytes_start < 12U || bytes_start > dict_block.size() || (bytes_start - 8U) % 4U != 0U) {
                error = "dict block header invalid";
                return false;
            }
            const std::size_t num_dict = (bytes_start - 8U) / 4U - 1U;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> dict_ranges(num_dict);
            for (std::size_t d = 0; d < num_dict; ++d) {
                std::uint32_t a = 0;
                std::uint32_t b = 0;
                std::memcpy(&a, dict_block.data() + 8U + d * 4U, 4U);
                std::memcpy(&b, dict_block.data() + 8U + (d + 1U) * 4U, 4U);
                if (bytes_start + b > dict_block.size() || b < a) {
                    error = "dict offsets out of range";
                    return false;
                }
                dict_ranges[d] = {bytes_start + a, b - a};
            }
            // Decode the RLE chunk of u32 indices (same framing as the fixed-width RLE path).
            if (data.size() < 10U) {
                error = "dict-rle data chunk too short";
                return false;
            }
            std::uint32_t size0 = 0;
            std::uint32_t size1 = 0;
            std::memcpy(&size0, data.data() + 2U, 4U);
            std::memcpy(&size1, data.data() + 6U, 4U);
            std::size_t voff = 10U;
            voff += (8U - (voff % 8U)) % 8U;
            std::size_t loff = voff + size0;
            loff += (8U - (loff % 8U)) % 8U;
            if (loff + size1 > data.size() || size0 % 4U != 0U || size0 / 4U != size1) {
                error = "dict-rle chunk sizes invalid";
                return false;
            }
            const std::size_t num_runs = size1;
            // Pre-pass: validate indices and size the output.
            std::size_t total_rows = 0;
            std::size_t total_data = 0;
            for (std::size_t r = 0; r < num_runs; ++r) {
                std::uint32_t index = 0;
                std::memcpy(&index, data.data() + voff + r * 4U, 4U);
                if (index >= num_dict) {
                    error = "dict-rle index out of range";
                    return false;
                }
                total_rows += data[loff + r];
                total_data += static_cast<std::size_t>(data[loff + r]) * dict_ranges[index].second;
            }
            out.variable.data.reserve(out.variable.data.size() + total_data);
            const bool first_page = out.variable.offsets.empty();
            std::uint64_t cumulative = out.variable.data.size();  // byte offset (continues across pages)

            // Expand: bulk-fill data once per run; collect offsets in a typed temp, then one bulk copy.
            auto expand = [&](auto& offs) -> bool {
                offs.reserve(total_rows + (first_page ? 1U : 0U));
                using OT = typename std::decay_t<decltype(offs)>::value_type;
                if (first_page) {
                    offs.push_back(static_cast<OT>(cumulative));
                }
                for (std::size_t r = 0; r < num_runs; ++r) {
                    std::uint32_t index = 0;
                    std::memcpy(&index, data.data() + voff + r * 4U, 4U);
                    const std::uint8_t run = data[loff + r];
                    const auto [start, len] = dict_ranges[index];
                    if (!append_repeated_value(out.variable.data, dict_block.data() + start, len, run)) {
                        return false;
                    }
                    for (std::uint8_t c = 0; c < run; ++c) {
                        cumulative += len;
                        offs.push_back(static_cast<OT>(cumulative));
                    }
                }
                const std::size_t base = out.variable.offsets.size();
                out.variable.offsets.resize(base + offs.size() * sizeof(OT));
                std::memcpy(out.variable.offsets.data() + base, offs.data(), offs.size() * sizeof(OT));
                return true;
            };
            bool ok = false;
            if (out.variable.large) {
                std::vector<std::uint64_t> offs;
                ok = expand(offs);
            } else {
                std::vector<std::uint32_t> offs;
                ok = expand(offs);
            }
            if (!ok) {
                error = "dict-rle expansion overflows";
                return false;
            }
        }
        return true;
    }

    // Structural dictionary variable-width column: buffer[1] = bitpacked u32 indices, buffer[2] = dictionary.
    if (encoding_plan.kind == ColumnEncodingKind::kDict) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = lance_logical_type_has_large_offsets(on_disk_field.logical_type);
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> dict_block;
        std::vector<std::uint8_t> indices_bytes;
        for (const auto& page : column_metadata.pages) {
            if (page.buffer_offsets.size() < 3U || page.buffer_sizes.size() < 3U) {
                error = "dict page missing buffers";
                return false;
            }
            if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[1], page.buffer_sizes[1], payload,
                                            error) ||
                !read_lance_data_file_bytes(data_file_path, page.buffer_offsets[2], page.buffer_sizes[2], dict_block,
                                            error)) {
                return false;
            }
            if (dict_block.size() < 8U) {
                error = "dict block too short";
                return false;
            }
            std::uint32_t bytes_start = 0;
            std::memcpy(&bytes_start, dict_block.data() + 4U, 4U);
            if (bytes_start < 12U || bytes_start > dict_block.size() || (bytes_start - 8U) % 4U != 0U) {
                error = "dict block header invalid";
                return false;
            }
            const std::size_t num_dict = (bytes_start - 8U) / 4U - 1U;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> dict_ranges(num_dict);
            for (std::size_t d = 0; d < num_dict; ++d) {
                std::uint32_t a = 0;
                std::uint32_t b = 0;
                std::memcpy(&a, dict_block.data() + 8U + d * 4U, 4U);
                std::memcpy(&b, dict_block.data() + 8U + (d + 1U) * 4U, 4U);
                if (bytes_start + b > dict_block.size() || b < a) {
                    error = "dict offsets out of range";
                    return false;
                }
                dict_ranges[d] = {bytes_start + a, b - a};
            }
            std::vector<std::vector<std::uint8_t>> chunks;
            if (!parse_miniblock_payload_chunk_list(payload, chunks, error)) {
                return false;
            }
            if (chunks.empty()) {
                error = "dict page has no index chunks";
                return false;
            }
            indices_bytes.clear();  // hoisted out of the loop; accumulates fresh per page via insert()
            std::uint64_t rows_remaining = page.length;
            for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
                const auto count = std::min<std::uint64_t>(1024U, rows_remaining);
                if (!unpack_bitpacked_page_dispatch(chunks[ci], count, 4U, indices_bytes, error)) {
                    return false;
                }
                rows_remaining -= count;
            }
            if (rows_remaining != 0U || indices_bytes.size() != page.length * 4U) {
                error = "dict index count mismatch";
                return false;
            }
            const bool first_page = out.variable.offsets.empty();
            std::uint64_t cumulative = out.variable.data.size();
            if (first_page) {
                append_list_offset(out.variable.offsets, static_cast<std::int64_t>(cumulative), out.variable.large);
            }
            for (std::uint64_t r = 0; r < page.length; ++r) {
                std::uint32_t index = 0;
                std::memcpy(&index, indices_bytes.data() + r * 4U, 4U);
                if (index >= num_dict) {
                    error = "dict index out of range";
                    return false;
                }
                const auto [start, len] = dict_ranges[index];
                out.variable.data.insert(out.variable.data.end(), dict_block.begin() + static_cast<std::ptrdiff_t>(start),
                                         dict_block.begin() + static_cast<std::ptrdiff_t>(start + len));
                cumulative += len;
                append_list_offset(out.variable.offsets, static_cast<std::int64_t>(cumulative), out.variable.large);
            }
        }
        return true;
    }

    // Byte-stream-split + zstd fixed-width column (float/double): each page's payload is a zstd frame
    // of vlen contiguous byte-planes; un-zstd then inverse-transpose to reconstruct the original bytes.
    if (encoding_plan.kind == ColumnEncodingKind::kBssZstd) {
        out.kind = ColumnValues::Kind::FixedWidth;
        const auto bytes_per_value = lance_logical_type_value_bytes(on_disk_field.logical_type);
        {
            std::uint64_t reserve_bytes = 0;
            if (checked_mul(declared_rows, static_cast<std::uint64_t>(bytes_per_value), reserve_bytes) &&
                fits_size_t(reserve_bytes)) {
                out.fixed.reserve(static_cast<std::size_t>(reserve_bytes));
            }
        }
        std::vector<std::uint8_t> control;
        std::vector<std::uint8_t> payload;
        std::vector<MiniBlockChunkView> chunks;
        std::vector<std::uint8_t> raw;
        std::uint64_t validity_rows = 0;
        for (const auto& page : column_metadata.pages) {
            if (!read_page_buffers(data_file_path, page, false, control, payload, error)) {
                return false;
            }
            if (!read_page_chunks_with_validity(payload, encoding_plan, page.length, chunks, validity_rows,
                                                out, error)) {
                return false;
            }
            std::uint64_t remaining = page.length;
            for (const auto& chunk : chunks) {
                if (!zstd_unframe_buffer(chunk.values, raw, error)) {
                    return false;
                }
                if (bytes_per_value == 0U || raw.size() % bytes_per_value != 0U) {
                    error = "bss-zstd page byte count mismatch";
                    return false;
                }
                const auto chunk_values = raw.size() / bytes_per_value;
                if (chunk_values == 0U || chunk_values > remaining) {
                    error = "bss-zstd chunk covers more values than the page has left";
                    return false;
                }
                const auto base = out.fixed.size();
                out.fixed.resize(base + raw.size());
                bss::untranspose(raw.data(), bytes_per_value, chunk_values, out.fixed.data() + base);
                remaining -= chunk_values;
            }
            if (remaining != 0U) {
                error = "bss-zstd page chunks cover fewer rows than the page declares";
                return false;
            }
        }
        return true;
    }

    // bool is always bit-packed on disk (1 bit/value, LSB-first), matching stock Lance's own
    // Flat{bits_per_value:1} representation -- the writer never tags it, since it's not opt-in (see
    // data_file_writer.cpp's bool_pack). nanolance's internal representation stays one byte per value.
    if (encoding_plan.kind == ColumnEncodingKind::kBoolPacked) {
        out.kind = ColumnValues::Kind::FixedWidth;
        out.fixed.reserve(static_cast<std::size_t>(declared_rows));
        std::vector<std::uint8_t> control;
        std::vector<std::uint8_t> payload;
        std::vector<MiniBlockChunkView> chunks;
        std::uint64_t validity_rows = 0;
        for (const auto& page : column_metadata.pages) {
            if (!read_page_buffers(data_file_path, page, false, control, payload, error)) {
                return false;
            }
            if (!read_page_chunks_with_validity(payload, encoding_plan, page.length, chunks, validity_rows,
                                                out, error)) {
                return false;
            }
            std::uint64_t remaining = page.length;
            for (const auto& chunk : chunks) {
                // A bool chunk's byte count only bounds its value count (the last byte is partial),
                // so the exact count comes from the levels when present and from what is left in the
                // page otherwise.
                const auto chunk_values = static_cast<std::size_t>(
                    encoding_plan.repdef != nullptr
                        ? chunk.repdef_values
                        : std::min<std::uint64_t>(remaining, chunk.values.size() * 8U));
                if (chunk_values == 0U || chunk_values > remaining ||
                    chunk.values.size() != (chunk_values + 7U) / 8U) {
                    error = "bool page byte count mismatch";
                    return false;
                }
                const auto base = out.fixed.size();
                out.fixed.resize(base + chunk_values);
                boolpack::unpack_lsb_first(chunk.values.data(), chunk_values, out.fixed.data() + base);
                remaining -= chunk_values;
            }
            if (remaining != 0U) {
                error = "bool page chunks cover fewer rows than the page declares";
                return false;
            }
        }
        return true;
    }

    if (encoding_plan.kind == ColumnEncodingKind::kVariable) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = lance_logical_type_has_large_offsets(on_disk_field.logical_type);
        const bool zstd = encoding_plan.zstd;
        std::vector<std::uint8_t> control;
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> chunk_bytes;
        std::vector<std::uint8_t> raw;
        for (const auto& page : column_metadata.pages) {
            if (!read_page_buffers(data_file_path, page, false, control, payload, error)) {
                return false;
            }
            if (!parse_miniblock_payload_chunks(payload, chunk_bytes, error)) {
                return false;
            }
            if (zstd) {
                // One chunk per page in nanolance's writer, so the payload holds one [u64][zstd] frame.
                if (!zstd_unframe_buffer(chunk_bytes, raw, error)) {
                    return false;
                }
                // swap (not move): a move would leave `raw` empty every iteration, discarding its
                // capacity right when the next page's zstd_unframe_buffer call could have reused it.
                std::swap(chunk_bytes, raw);
            }
            if (!decode_variable_width_page(chunk_bytes, page.length, out.variable.large, out.variable.offsets,
                                          out.variable.data, error)) {
                return false;
            }
        }
        return true;
    }

    out.kind = ColumnValues::Kind::FixedWidth;
    std::string internal_type = on_disk_field.logical_type;
    if (internal_type == "string") {
        internal_type = "utf8";
    }
    const auto bytes_per_value = lance_logical_type_value_bytes(internal_type);
    const bool bitpacked = encoding_plan.kind == ColumnEncodingKind::kBitpack;
    // Reserve the whole column upfront: unpack_bitpacked_page() (and the plain-copy branch below) grow
    // out.fixed one FastLanes chunk (<=1024 values) at a time via insert(), so without this a column of
    // many chunks reallocates and re-copies everything already written on almost every chunk.
    std::uint64_t reserve_bytes = 0;
    if (checked_mul(declared_rows, static_cast<std::uint64_t>(bytes_per_value), reserve_bytes) &&
        fits_size_t(reserve_bytes)) {
        out.fixed.reserve(static_cast<std::size_t>(reserve_bytes));
    }
    std::vector<std::uint8_t> control;
    std::vector<std::uint8_t> payload;
    std::vector<MiniBlockChunkView> chunks;
    const bool nullable = encoding_plan.repdef != nullptr;
    std::uint64_t validity_rows = 0;
    for (const auto& page : column_metadata.pages) {
        if (!read_page_buffers(data_file_path, page, false, control, payload, error)) {
            return false;
        }
        if (!split_miniblock_payload(payload, chunks, error)) {
            return false;
        }
        // How many values a chunk holds depends on how it is encoded, and the header only states it
        // when the chunk carries definition levels:
        //   * with definition levels, the header's count is authoritative;
        //   * bit-packed chunks are one FastLanes block each (1024 values), the last one short;
        //   * flat chunks are sized by bytes -- the writer fills them to a byte budget, not to a
        //     value count, so a flat int64 chunk holds whatever fits.
        std::uint64_t remaining = page.length;
        for (const auto& chunk : chunks) {
            std::uint32_t chunk_values = 0;
            if (nullable) {
                chunk_values = chunk.repdef_values;
            } else if (bitpacked) {
                chunk_values = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, 1024U));
            } else {
                if (bytes_per_value == 0U || chunk.values.size() % bytes_per_value != 0U) {
                    error = "fixed-width page byte count mismatch";
                    return false;
                }
                chunk_values = static_cast<std::uint32_t>(chunk.values.size() / bytes_per_value);
            }
            if (chunk_values == 0U || chunk_values > remaining) {
                error = "miniblock chunk covers " + std::to_string(chunk_values) +
                        " values with " + std::to_string(remaining) + " left in the page";
                return false;
            }
            if (nullable) {
                if (chunk.repdef.empty()) {
                    error = "column declares definition levels but a chunk carries none";
                    return false;
                }
                if (!append_definition_levels(chunk.repdef, *encoding_plan.repdef, chunk.repdef_values,
                                              validity_rows, out.validity, out.null_count, error)) {
                    return false;
                }
                validity_rows += chunk.repdef_values;
            }
            if (bitpacked) {
                if (!unpack_bitpacked_page_dispatch(chunk.values, chunk_values, bytes_per_value, out.fixed,
                                                    error)) {
                    return false;
                }
            } else {
                if (chunk.values.size() != static_cast<std::size_t>(chunk_values) * bytes_per_value) {
                    error = "fixed-width page byte count mismatch";
                    return false;
                }
                out.fixed.insert(out.fixed.end(), chunk.values.begin(), chunk.values.end());
            }
            remaining -= chunk_values;
        }
        if (remaining != 0U) {
            error = "miniblock page chunks cover " + std::to_string(page.length - remaining) +
                    " of " + std::to_string(page.length) + " rows";
            return false;
        }
    }
    if (nullable && validity_rows != declared_rows) {
        error = "definition levels cover " + std::to_string(validity_rows) + " rows but the column has " +
                std::to_string(declared_rows);
        return false;
    }
    return true;
}

}  // namespace nano_lance
