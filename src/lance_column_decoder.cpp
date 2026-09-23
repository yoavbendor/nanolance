// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/lance_column_decoder.hpp"

#include "nanolance/page_layout.hpp"

#include "nanolance/blob_v2_external.hpp"
#include "nanolance/bool_bitpack.hpp"
#include "nanolance/byte_stream_split.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/fastlanes_bitpack.hpp"
#include "nanolance/fsst.hpp"
#include "nanolance/lz4_block.hpp"
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

    // Doubling fill: write the value once, then repeatedly copy everything written so far. That is
    // log2(count) memcpys of geometrically growing size instead of `count` memcpys of `vlen` bytes.
    //
    // Measured on a 4M-row constant column: int64 12.3 ms -> 4.9 ms. A constant STRING barely moves
    // (69 ms -> 66 ms) and that is not a shortcoming of this loop -- the cost there is the bytes
    // themselves. Read time scales linearly with the materialized volume (3.8 MiB 4.0 ms, 91.6 MiB
    // 50.8 ms, 381.5 MiB 224.8 ms: ~1.7 GB/s, i.e. memory bandwidth), so the only way to beat it is
    // to not materialize at all -- Arrow REE or a dictionary, which changes the type the caller
    // gets. See docs/OPTIMIZATION_PLAN.md 4.2.
    std::memcpy(dst, val, vlen);
    std::size_t filled = vlen;
    const std::size_t total = static_cast<std::size_t>(added);
    while (filled < total) {
        const std::size_t chunk = std::min(filled, total - filled);
        std::memcpy(dst + filled, dst, chunk);
        filled += chunk;
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

bool read_le32(const std::uint8_t* p, std::uint32_t& v) {
    v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
        (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
    return true;
}

/// Every buffer inside a miniblock chunk is padded up to 8 bytes AFTER it is written, and the header
/// records the unpadded length. The padding was invisible while the only level buffer nanolance
/// produced or met was a full 128-byte packed block; a short chunk's raw level buffer (see
/// append_definition_levels) can be any even size, and then the values start at the next multiple of
/// 8, not immediately after the levels.
constexpr std::size_t kMiniblockAlignment = 8U;

constexpr std::size_t align_to_miniblock(std::size_t bytes) {
    return (bytes + kMiniblockAlignment - 1U) & ~(kMiniblockAlignment - 1U);
}

/// How to read a chunk header, which is not a fixed shape: the slots that exist depend on the page's
/// MiniBlockLayout descriptor.
///
/// Lance's own `decode_miniblock_chunk` (`rust/lance-encoding/src/encodings/logical/primitive.rs`)
/// spells it out:
///
///     [u16 num_levels]
///     [u16 rep_size]                       only when rep_compression is present
///     [u16 def_size]                       only when def_compression is present
///     [num_buffers x (u32 if has_large_chunk else u16)]
///     pad to 8
///     [rep] pad8  [def] pad8  [buffer_0] pad8  [buffer_1] pad8 ...
///
/// This used to be read as a fixed `[u16 num_levels][u16][u16][u16]`, which is the right 8 bytes for
/// exactly one shape: no repetition layer, two value buffers, small chunks. That covers nanolance's
/// own output and most of stock Lance's, and silently misreads the rest -- a `float64` column whose
/// nulls come in runs sets `has_large_chunk`, so its buffer sizes are `u32` and the definition block
/// starts 8 bytes later than the fixed parse expects.
/// The defaults are the shape of a page with no descriptor at all, which is only ever an old
/// nanolance file: no repetition, no definition levels (nullability postdates the writer emitting
/// descriptors, so no such file has any), two u16 buffer sizes. That is exactly what the old fixed
/// 8-byte parse assumed, so those files decode byte-for-byte as they did before.
struct MiniBlockChunkShape {
    bool has_repetition = false;
    bool has_definition = false;
    std::uint32_t num_buffers = 2;
    bool large_buffer_sizes = false;

    std::size_t buffer_size_bytes() const { return large_buffer_sizes ? 4U : 2U; }
    /// Bytes before the padding, i.e. everything the header declares.
    std::size_t declared_bytes() const {
        return 2U + (has_repetition ? 2U : 0U) + (has_definition ? 2U : 0U) +
               static_cast<std::size_t>(num_buffers) * buffer_size_bytes();
    }
};

struct MiniBlockChunkHeader {
    std::uint16_t num_levels = 0;
    std::uint32_t rep_size = 0;
    std::uint32_t def_size = 0;
    std::vector<std::uint32_t> buffer_sizes;
    /// Bytes the whole chunk occupies, padding included -- i.e. where the next chunk starts.
    std::size_t chunk_bytes = 0;
};

bool read_miniblock_chunk_header(const std::vector<std::uint8_t>& payload, std::size_t offset,
                                 const MiniBlockChunkShape& shape, MiniBlockChunkHeader& out,
                                 std::string& error) {
    out.buffer_sizes.clear();
    // num_buffers comes from the untrusted descriptor; cap it so a hostile value cannot make the
    // header arithmetic below run away before any of it is bounds-checked.
    if (shape.num_buffers > 64U) {
        error = "miniblock layout declares " + std::to_string(shape.num_buffers) + " buffers per chunk";
        return false;
    }
    const auto declared = shape.declared_bytes();
    if (offset > payload.size() || payload.size() - offset < declared) {
        error = "truncated miniblock payload header";
        return false;
    }

    std::size_t at = offset;
    const auto take_u16 = [&payload, &at]() {
        std::uint16_t v = 0;
        read_le16(payload.data() + at, v);
        at += 2U;
        return static_cast<std::uint32_t>(v);
    };
    const auto take_u32 = [&payload, &at]() {
        std::uint32_t v = 0;
        read_le32(payload.data() + at, v);
        at += 4U;
        return v;
    };

    out.num_levels = static_cast<std::uint16_t>(take_u16());
    out.rep_size = shape.has_repetition ? take_u16() : 0U;
    out.def_size = shape.has_definition ? take_u16() : 0U;
    out.buffer_sizes.reserve(shape.num_buffers);
    for (std::uint32_t i = 0; i < shape.num_buffers; ++i) {
        out.buffer_sizes.push_back(shape.large_buffer_sizes ? take_u32() : take_u16());
    }

    // Every section is padded to the miniblock alignment, so walk them in order and let each one's
    // end define the next one's start. Sizes are untrusted: accumulate in 64 bits and compare against
    // what is left of the payload at every step rather than summing first and checking once.
    std::uint64_t cursor = align_to_miniblock(declared);
    const auto advance = [&cursor](std::uint64_t size) {
        cursor = (cursor + size + kMiniblockAlignment - 1U) & ~static_cast<std::uint64_t>(kMiniblockAlignment - 1U);
    };
    advance(out.rep_size);
    advance(out.def_size);
    for (const auto size : out.buffer_sizes) {
        advance(size);
    }
    if (cursor > payload.size() - offset) {
        error = "miniblock chunk exceeds payload";
        return false;
    }
    out.chunk_bytes = static_cast<std::size_t>(cursor);
    return true;
}

/// One decoded chunk: its value buffers, and its definition levels when it carries any.
///
/// `values` is the FIRST value buffer, which is the only one most encodings have; `extra_buffers`
/// holds the rest (an RLE value block, for instance, is a values buffer plus a run-lengths buffer).
struct MiniBlockChunkView {
    std::vector<std::uint8_t> values;
    std::vector<std::vector<std::uint8_t>> extra_buffers;
    std::vector<std::uint8_t> repdef;
    std::uint32_t repdef_values = 0;
};

/// Split a page's payload into its chunks without concatenating them.
///
/// A page is NOT one chunk. nanolance's own writer happens to emit exactly one chunk per page, which
/// is why treating the payload as a single chunk worked on its own files; stock Lance packs many
/// (a 5000-row int64 page arrives as five 1024-value chunks), so the concatenated buffer failed the
/// per-chunk size check with "bitpacked chunk size does not match bit width".
bool split_miniblock_payload(const std::vector<std::uint8_t>& payload, const MiniBlockChunkShape& shape,
                             std::vector<MiniBlockChunkView>& out, std::string& error) {
    out.clear();
    std::size_t offset = 0;
    while (offset < payload.size()) {
        MiniBlockChunkHeader header;
        if (!read_miniblock_chunk_header(payload, offset, shape, header, error)) {
            return false;
        }
        if (header.chunk_bytes == 0U) {
            error = "miniblock chunk declares no bytes";
            return false;
        }
        MiniBlockChunkView chunk;
        std::size_t at = offset + align_to_miniblock(shape.declared_bytes());
        at += align_to_miniblock(header.rep_size);  // repetition levels are not decoded yet
        if (shape.has_definition && header.def_size != 0U) {
            chunk.repdef_values = header.num_levels;
            chunk.repdef.assign(payload.begin() + static_cast<std::ptrdiff_t>(at),
                                payload.begin() + static_cast<std::ptrdiff_t>(at + header.def_size));
        }
        at += align_to_miniblock(header.def_size);
        for (std::size_t i = 0; i < header.buffer_sizes.size(); ++i) {
            const auto size = static_cast<std::size_t>(header.buffer_sizes[i]);
            std::vector<std::uint8_t> buffer(payload.begin() + static_cast<std::ptrdiff_t>(at),
                                             payload.begin() + static_cast<std::ptrdiff_t>(at + size));
            if (i == 0U) {
                chunk.values = std::move(buffer);
            } else {
                chunk.extra_buffers.push_back(std::move(buffer));
            }
            at += align_to_miniblock(size);
        }
        out.push_back(std::move(chunk));
        offset += header.chunk_bytes;
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
    //
    // Grown in ONE resize and then written through a typed pointer, rather than one
    // append_list_offset() per row. The per-row shape was 32% of a whole read's instruction count in
    // callgrind -- not the four bytes it copies, but the size/capacity round trip and value-init that
    // vector::resize() does per call. Shifting a run of offsets by a constant is the same work either
    // way; only the bookkeeping around it differs, and here it happens once per page.
    const auto first_index = out_offsets.empty() ? 0U : 1U;
    const auto emit = static_cast<std::size_t>(num_values + 1U - first_index);
    const auto write_at = out_offsets.size();
    out_offsets.resize(write_at + emit * offset_width);
    const auto shift = cumulative_base - data_base_in_chunk;
    const auto* src = chunk_bytes.data() + static_cast<std::size_t>(first_index) * offset_width;
    auto* dst = out_offsets.data() + write_at;
    if (large) {
        for (std::size_t i = 0; i < emit; ++i) {
            std::int64_t v = 0;
            std::memcpy(&v, src + i * 8U, sizeof(v));
            v += shift;
            std::memcpy(dst + i * 8U, &v, sizeof(v));
        }
    } else {
        for (std::size_t i = 0; i < emit; ++i) {
            std::int32_t v = 0;
            std::memcpy(&v, src + i * 4U, sizeof(v));
            const auto out_v = static_cast<std::int32_t>(static_cast<std::int64_t>(v) + shift);
            std::memcpy(dst + i * 4U, &out_v, sizeof(out_v));
        }
    }
    return true;
}

/// Expand one FSST-compressed chunk. `offsets`/`data` are what decode_variable_width_page produced
/// from the COMPRESSED block -- a dense (num_values + 1) offset table starting at 0 -- and this walks
/// them, decompressing each value onto the column's own offsets/data buffers.
///
/// The two offset widths are deliberately separate: the compressed block's offsets index compressed
/// bytes and come from the descriptor (`Fsst.values = Variable{Flat(bits)}`), while the output's
/// index decoded bytes and follow the column's Arrow type.
[[nodiscard]] bool expand_fsst_values(const fsst::SymbolTable& table,
                                      const std::vector<std::uint8_t>& offsets, bool offsets_large,
                                      const std::vector<std::uint8_t>& data, std::uint64_t num_values,
                                      bool out_large, std::vector<std::uint8_t>& out_offsets,
                                      std::vector<std::uint8_t>& out_data, std::string& error) {
    if (num_values == 0U) {
        return true;
    }
    // First chunk of the column also emits the leading 0; later ones continue the buffer.
    if (out_offsets.empty()) {
        append_list_offset(out_offsets, 0, out_large);
    }
    // Each value's decoded length is only known after decompressing it, so unlike the plain
    // variable-width path this cannot be one bulk write -- but the growth can still be one
    // reservation instead of a reallocation every few rows.
    out_offsets.reserve(out_offsets.size() + static_cast<std::size_t>(num_values) * (out_large ? 8U : 4U));
    const auto base = read_list_offset(offsets, 0, offsets_large);
    for (std::uint64_t i = 0; i < num_values; ++i) {
        // decode_variable_width_page already proved this table is non-decreasing and inside `data`.
        const auto start = read_list_offset(offsets, i, offsets_large) - base;
        const auto end = read_list_offset(offsets, i + 1U, offsets_large) - base;
        if (start < 0 || end < start || static_cast<std::size_t>(end) > data.size()) {
            error = "FSST value bounds out of range";
            return false;
        }
        if (!fsst::decompress_value(table, data.data() + start, static_cast<std::size_t>(end - start),
                                    out_data, error)) {
            return false;
        }
        // A 32-bit offsets column cannot address more than 2 GiB of decoded bytes. FSST expands, so
        // this is reachable from a file that was itself well under the limit -- check it per value
        // rather than discovering it as a wrapped negative offset later.
        if (out_data.size() > default_read_limits().max_uncompressed_bytes ||
            (!out_large && out_data.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))) {
            error = "FSST-decoded column exceeds the decoded-size limit";
            return false;
        }
        append_list_offset(out_offsets, static_cast<std::int64_t>(out_data.size()), out_large);
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


/// Turn `count` decoded levels into validity bits. Rows accumulate across chunks into one contiguous
/// bitmap, so a chunk whose row count is not a multiple of 8 leaves the next chunk starting mid-byte;
/// each bit therefore goes at its ABSOLUTE row index rather than at a per-chunk offset.
bool append_levels_to_validity(const std::uint16_t* levels, std::uint32_t count,
                               std::uint64_t rows_already_appended, std::vector<std::uint8_t>& out_validity,
                               std::uint64_t& out_null_count) {
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

/// Run-length-encoded definition levels, which is what Lance picks when the nulls come in runs or
/// are very sparse -- a column with a single null in 5000 rows takes this path, not the bit-packed
/// one. The block is `[u64 LE values_size][run values][run lengths]`, the two widths named by the
/// descriptor's `Rle{ Flat(value_bits), Flat(length_bits) }`. A run longer than the length type can
/// hold is split into several entries carrying the same value, so decoding is a plain expansion.
[[nodiscard]] bool decode_rle_definition_levels(const std::vector<std::uint8_t>& repdef,
                                                const page_layout::Compressive& encoding,
                                                std::uint32_t count, std::uint16_t* levels,
                                                std::string& error) {
    const auto* values_node = encoding.values.get();
    const auto* lengths_node = encoding.lengths.get();
    if (values_node == nullptr || lengths_node == nullptr ||
        values_node->kind != page_layout::CompressiveKind::kFlat ||
        lengths_node->kind != page_layout::CompressiveKind::kFlat) {
        error = "unsupported definition-level encoding: " + page_layout::describe_encoding(encoding);
        return false;
    }
    const auto value_bits = values_node->bits_per_value;
    const auto length_bits = lengths_node->bits_per_value;
    if ((value_bits != 8U && value_bits != 16U) ||
        (length_bits != 8U && length_bits != 16U && length_bits != 32U)) {
        error = "run-length-encoded definition levels declare unsupported widths (" +
                std::to_string(value_bits) + "-bit values, " + std::to_string(length_bits) +
                "-bit run lengths)";
        return false;
    }
    const std::size_t value_bytes = value_bits / 8U;
    const std::size_t length_bytes = length_bits / 8U;

    if (repdef.size() < 8U) {
        error = "run-length definition-level buffer is too short for its header";
        return false;
    }
    const auto values_size64 = load_le<std::uint64_t>(repdef.data());
    if (!fits_size_t(values_size64) || static_cast<std::size_t>(values_size64) > repdef.size() - 8U) {
        error = "run-length definition-level values buffer runs past the end of the block";
        return false;
    }
    const auto values_size = static_cast<std::size_t>(values_size64);
    if (values_size % value_bytes != 0U) {
        error = "run-length definition-level values buffer is not a whole number of values";
        return false;
    }
    const auto runs = values_size / value_bytes;
    const auto lengths_size = repdef.size() - 8U - values_size;
    if (lengths_size != runs * length_bytes) {
        error = "run-length definition-level block has " + std::to_string(runs) + " run values but " +
                std::to_string(lengths_size) + " bytes of run lengths";
        return false;
    }

    const auto* values = repdef.data() + 8U;
    const auto* lengths = values + values_size;
    std::uint64_t written = 0;
    for (std::size_t r = 0; r < runs; ++r) {
        const std::uint16_t level = value_bytes == 1U ? values[r] : load_le<std::uint16_t>(values + r * 2U);
        std::uint64_t run = 0;
        switch (length_bytes) {
            case 1U: run = lengths[r]; break;
            case 2U: run = load_le<std::uint16_t>(lengths + r * 2U); break;
            default: run = load_le<std::uint32_t>(lengths + r * 4U); break;
        }
        if (run > count || written + run > count) {
            error = "run-length definition levels cover more than the chunk's " + std::to_string(count) +
                    " values";
            return false;
        }
        for (std::uint64_t i = 0; i < run; ++i) {
            levels[written + i] = level;
        }
        written += run;
    }
    if (written != count) {
        error = "run-length definition levels cover " + std::to_string(written) + " of the chunk's " +
                std::to_string(count) + " values";
        return false;
    }
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
    // Levels are u16 and `count` comes from the chunk header, so it is bounded by 65535 -- but a
    // chunk can legitimately carry more than one FastLanes block of them. A `bool` column packs
    // densely enough that pylance puts 1025 values in one chunk, which this used to refuse outright.
    thread_local std::vector<std::uint16_t> level_storage;
    level_storage.resize(std::max<std::size_t>(count, 1U));
    std::uint16_t* levels = level_storage.data();
    if (encoding.kind == page_layout::CompressiveKind::kRle) {
        if (!decode_rle_definition_levels(repdef, encoding, count, levels, error)) {
            return false;
        }
        return append_levels_to_validity(levels, count, rows_already_appended, out_validity,
                                         out_null_count);
    }
    // InlineBitpacking(16): the same FastLanes block a bitpacked *value* page carries, with its bit
    // width as the first u16 of the buffer rather than in the descriptor -- which is what "inline"
    // means. Lance picks this over Bitpacked{Flat(bits)} for the definition levels of some page
    // sizes; empirically, a pylance nullable string column of 200..1000 rows lands here while 100 and
    // 2000 do not, so refusing it made a common, unremarkable dataset unreadable.
    //
    // The 16 is the uncompressed element width, and Lance's definition levels are u16, so it is the
    // only width that can appear. Anything else is refused by name rather than guessed at.
    if (encoding.kind == page_layout::CompressiveKind::kInlineBitpacking) {
        if (encoding.bits_per_value != 16U) {
            error = "definition levels declare InlineBitpacking(" +
                    std::to_string(encoding.bits_per_value) + "); only 16-bit levels exist";
            return false;
        }
        thread_local std::vector<std::uint8_t> unpacked;
        unpacked.clear();
        if (!unpack_bitpacked_page<std::uint16_t>(repdef, count, unpacked, error)) {
            return false;
        }
        std::memcpy(levels, unpacked.data(), unpacked.size());
        return append_levels_to_validity(levels, count, rows_already_appended, out_validity,
                                         out_null_count);
    }
    if (encoding.kind != page_layout::CompressiveKind::kBitpacked || encoding.values == nullptr ||
        encoding.values->kind != page_layout::CompressiveKind::kFlat) {
        error = "unsupported definition-level encoding: " + page_layout::describe_encoding(encoding);
        return false;
    }
    const auto width = encoding.values->bits_per_value;
    if (width == 0U || width > 16U) {
        error = "definition levels declare an unsupported width of " + std::to_string(width) + " bits";
        return false;
    }
    // The buffer is a run of whole FastLanes blocks followed by a tail, and the tail has two legal
    // spellings: packed-and-padded, or raw u16 words. Lance's `unpack_out_of_line`
    // (`rust/lance-encoding/src/encodings/physical/bitpacking.rs`) tells them apart by the buffer's
    // total LENGTH -- the encoder pads only when padding costs fewer bits than packing saves -- and
    // this mirrors that arithmetic exactly rather than approximating it:
    //
    //     whole_blocks = count / 1024              tail = count % 1024
    //     raw  <=>  words == whole_blocks * packed_words + tail
    //
    // Two cases this has to keep getting right: a 20000-row nullable binary column from pylance ends
    // in a 32-value chunk whose 64-byte level buffer is raw, and a 1025-row bool column is one whole
    // packed block (128 bytes) plus a single raw u16.
    const auto packed_words = nano_lance::fastlanes::packed_words_1024<std::uint16_t>(width);
    if (repdef.size() % sizeof(std::uint16_t) != 0U) {
        error = "definition-level buffer is not a whole number of 16-bit words";
        return false;
    }
    const auto words = repdef.size() / sizeof(std::uint16_t);
    const std::size_t whole_blocks = count / 1024U;
    const std::size_t tail = count % 1024U;
    const std::size_t full_words = whole_blocks * packed_words;
    const bool tail_is_raw = tail != 0U && words == full_words + tail;
    const std::size_t expected_words = tail_is_raw ? full_words + tail
                                                   : full_words + (tail != 0U ? packed_words : 0U);
    if (words != expected_words) {
        error = "definition-level buffer is " + std::to_string(repdef.size()) + " bytes, expected " +
                std::to_string(expected_words * sizeof(std::uint16_t)) + " for " +
                std::to_string(count) + " levels at " + std::to_string(width) + " bits";
        return false;
    }

    thread_local std::vector<std::uint16_t> packed;
    packed.resize(packed_words != 0U ? packed_words : 1U);
    std::uint16_t block[1024];
    std::size_t word_at = 0;
    for (std::size_t b = 0; b < whole_blocks; ++b) {
        std::memcpy(packed.data(), repdef.data() + word_at * sizeof(std::uint16_t),
                    packed_words * sizeof(std::uint16_t));
        nano_lance::fastlanes::unpack_1024<std::uint16_t>(width, packed.data(), block);
        std::memcpy(levels + b * 1024U, block, 1024U * sizeof(std::uint16_t));
        word_at += packed_words;
    }
    if (tail != 0U) {
        if (tail_is_raw) {
            std::memcpy(levels + whole_blocks * 1024U, repdef.data() + word_at * sizeof(std::uint16_t),
                        tail * sizeof(std::uint16_t));
        } else {
            std::memcpy(packed.data(), repdef.data() + word_at * sizeof(std::uint16_t),
                        packed_words * sizeof(std::uint16_t));
            nano_lance::fastlanes::unpack_1024<std::uint16_t>(width, packed.data(), block);
            std::memcpy(levels + whole_blocks * 1024U, block, tail * sizeof(std::uint16_t));
        }
    }

    return append_levels_to_validity(levels, count, rows_already_appended, out_validity, out_null_count);
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
    /// How the page's VALUE buffer is compressed, from the descriptor's `General{scheme}` wrapper.
    /// kNone means the bytes are the values. Anything this build cannot decompress is refused in
    /// classification, so no decode branch ever has to treat an unknown scheme as raw.
    page_layout::BufferScheme value_scheme = page_layout::BufferScheme::kNone;
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
    /// kVariable written by stock Lance: the page's FSST symbol table, parsed once per column. Empty
    /// `symbol_count` with `passthrough` set is the ordinary case for a small file -- Lance skips FSST
    /// below 32 KiB of input but still wraps the page in the encoding.
    std::optional<fsst::SymbolTable> fsst;
    /// kDict / kDictRle: how the dictionary block itself is compressed. nanolance writes it raw for
    /// `dict` and zstd-framed for `dict-rle`; stock Lance uses LZ4 for a low-cardinality string
    /// column. Reading it from the descriptor is what makes all three decode from the same branch.
    page_layout::BufferScheme dict_scheme = page_layout::BufferScheme::kNone;
    /// kDict: bits per entry when the dictionary block holds FIXED-WIDTH values (`Flat(64)` for a
    /// time64 column, `Flat(128)` for decimal128), and 0 when it holds a variable-width block. The
    /// two are completely different blocks -- a variable-width one starts with an offset header, a
    /// flat one is just the values end to end -- so this is what picks the branch, not a guess from
    /// the column's logical type.
    std::uint32_t dict_value_bits = 0;
    /// kDict: `num_dictionary_items` from the descriptor. Authoritative for a fixed-width dictionary,
    /// whose block carries no count of its own.
    std::uint64_t dict_items = 0;
    /// kVariable: the offset width the descriptor declares for the value block, in bits. Only the
    /// FSST path consults it -- there the block being decoded is the COMPRESSED one, whose offsets
    /// index compressed bytes and need not share the column's own offset width.
    std::uint32_t variable_offset_bits = 0;
    /// Set when the descriptor named something this build does not model, so the error can say what.
    std::string unsupported_reason;
    /// kMiniBlock: how to read each chunk's header. Not a constant -- see MiniBlockChunkShape.
    MiniBlockChunkShape chunk_shape;
};

/// A dictionary block is stored raw, zstd-framed or LZ4-framed depending on who wrote the file:
/// nanolance writes `dict` raw and `dict-rle` zstd-framed, stock Lance uses LZ4 for a
/// low-cardinality string column. The scheme comes from the descriptor (or, for a file with no
/// descriptor, from the fallback classifier), so both dictionary branches share one unwrap.
[[nodiscard]] bool decompress_dictionary_block(page_layout::BufferScheme scheme,
                                               const std::vector<std::uint8_t>& stored,
                                               std::vector<std::uint8_t>& out, std::string& error) {
    switch (scheme) {
        case page_layout::BufferScheme::kNone:
            out.assign(stored.begin(), stored.end());
            return true;
        case page_layout::BufferScheme::kZstd:
            return zstd_unframe_buffer(stored, out, error);
        case page_layout::BufferScheme::kLz4:
            return lz4_block::decompress_sized(stored, out, error);
        default:
            error = "unsupported dictionary buffer compression";
            return false;
    }
}

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

    // The chunk header's shape, taken from the same three descriptor fields Lance's decoder reads it
    // from. `has_definition` keys off the presence of `def_compression` (f2) rather than off `layers`,
    // because that is what decides whether the header carries a `def_size` slot -- read it from the
    // wrong field and every byte after it is misplaced.
    out.chunk_shape.has_repetition = layout.mini_block.has_repetition;
    out.chunk_shape.has_definition = layout.mini_block.repdef_compression != nullptr;
    out.chunk_shape.large_buffer_sizes = layout.mini_block.has_large_chunk;
    // A descriptor that omits `num_buffers` (f7) leaves it 0. Two is what this reader assumed for
    // years and what every page it has been able to read actually has, so keep that rather than
    // deciding a page has no buffers at all.
    out.chunk_shape.num_buffers =
        layout.mini_block.num_buffers != 0U ? layout.mini_block.num_buffers : 2U;

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
    if (has_dictionary) {
        // The dictionary block is read as a variable-width (or flat) block, optionally behind one
        // zstd frame -- that is exactly what nanolance's own `dict` and `dict-rle` pages carry.
        // Stock Lance may compress it with something else: a unicode-heavy string column comes back
        // as General{LZ4, Variable}, and decoding that as raw produced "dict block header invalid",
        // which names the symptom rather than the cause. Refuse by encoding instead.
        const auto* dict = layout.mini_block.dictionary.get();
        if (dict->kind == page_layout::CompressiveKind::kGeneral) {
            switch (dict->scheme) {
                case page_layout::BufferScheme::kNone:
                case page_layout::BufferScheme::kZstd:
                case page_layout::BufferScheme::kLz4:
                    out.dict_scheme = dict->scheme;
                    break;
                default:
                    out.kind = ColumnEncodingKind::kUnsupported;
                    out.unsupported_reason =
                        "unsupported dictionary encoding in " + page_layout::describe(layout);
                    return true;
            }
            dict = dict->values.get();
        }
        if (dict == nullptr || (dict->kind != page_layout::CompressiveKind::kVariable &&
                                dict->kind != page_layout::CompressiveKind::kFlat)) {
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason = "unsupported dictionary encoding in " + page_layout::describe(layout);
            return true;
        }
        if (dict->kind == page_layout::CompressiveKind::kFlat) {
            // A dictionary of fixed-width values. Lance builds these once a temporal or decimal
            // column's cardinality justifies it -- `Flat(64)` for time64 past 1024 rows,
            // `Flat(128)` for decimal128 past 20000 -- and the block is then just the values, with
            // none of the offset header the variable-width path reads.
            if (dict->bits_per_value == 0U || dict->bits_per_value % 8U != 0U) {
                out.kind = ColumnEncodingKind::kUnsupported;
                out.unsupported_reason =
                    "dictionary values are " + std::to_string(dict->bits_per_value) +
                    " bits, which is not a whole number of bytes, in " + page_layout::describe(layout);
                return true;
            }
            out.dict_value_bits = dict->bits_per_value;
            out.dict_items = layout.mini_block.num_dictionary_items;
        }
    }

    // General{scheme, inner} is a wrapper: unwrap it and remember whether it compresses.
    const page_layout::Compressive* inner = values;
    if (inner->kind == page_layout::CompressiveKind::kGeneral) {
        if (inner->scheme == page_layout::BufferScheme::kUnknownScheme) {
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason = "unsupported buffer compression in " + page_layout::describe(layout);
            return true;
        }
        out.value_scheme = inner->scheme;
        out.zstd = inner->scheme == page_layout::BufferScheme::kZstd;
        if (inner->values == nullptr) {
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason = "General encoding has no inner values";
            return true;
        }
        if (inner->values->kind == page_layout::CompressiveKind::kByteStreamSplit) {
            // The byte-stream-split path un-zstds the page itself; it has no other framing to fall
            // back on, so a differently-compressed one is refused rather than read as raw planes.
            if (!out.zstd) {
                out.kind = ColumnEncodingKind::kUnsupported;
                out.unsupported_reason =
                    "unsupported buffer compression in " + page_layout::describe(layout);
                return true;
            }
            out.kind = ColumnEncodingKind::kBssZstd;
            return true;
        }
        inner = inner->values.get();
    }

    // Fsst{symbol_table, values} wraps the real value encoding. Unwrap it the same way General is
    // unwrapped, keeping the symbol table for the decode loop: stock Lance puts every string and
    // binary column inside this, so refusing it was refusing utf8 from pylance outright.
    if (inner->kind == page_layout::CompressiveKind::kFsst) {
        fsst::SymbolTable table;
        std::string table_error;
        if (!fsst::parse_symbol_table(inner->symbol_table, table, table_error)) {
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason = table_error;
            return true;
        }
        out.fsst = table;
        if (inner->values == nullptr) {
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason = "Fsst encoding has no inner values";
            return true;
        }
        inner = inner->values.get();
        // Fsst's own values may in turn be General(zstd)-wrapped.
        if (inner->kind == page_layout::CompressiveKind::kGeneral) {
            if (inner->scheme == page_layout::BufferScheme::kUnknownScheme) {
                out.kind = ColumnEncodingKind::kUnsupported;
                out.unsupported_reason = "unsupported buffer compression in " + page_layout::describe(layout);
                return true;
            }
            out.value_scheme = inner->scheme;
            out.zstd = inner->scheme == page_layout::BufferScheme::kZstd;
            if (inner->values == nullptr) {
                out.kind = ColumnEncodingKind::kUnsupported;
                out.unsupported_reason = "General encoding has no inner values";
                return true;
            }
            inner = inner->values.get();
        }
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
            out.variable_offset_bits = inner->values == nullptr ? 0U : inner->values->bits_per_value;
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
        plan.dict_scheme = page_layout::BufferScheme::kZstd;  // what page_layout_bytes_dict_rle writes
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
    if (!split_miniblock_payload(payload, plan.chunk_shape, chunks, error)) {
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
        std::vector<MiniBlockChunkView> chunks;
        std::uint64_t validity_rows = 0;
        for (const auto& page : column_metadata.pages) {
            if (!read_page_buffers(data_file_path, page, false, control, data, error)) {
                return false;
            }
            // Shared chunk split, not a second hand-rolled one. This branch used to parse the chunk
            // itself as [u16 num_levels][u32 size0][u32 size1], which is the right header only for a
            // NON-nullable RLE column: a nullable one carries a `u16 def_size` slot between the level
            // count and the buffer sizes, so every offset after it was two bytes out and the read died
            // with "rle chunk buffer sizes invalid". A pylance float64 column whose nulls come in runs
            // is exactly that shape.
            if (!read_page_chunks_with_validity(data, encoding_plan, page.length, chunks, validity_rows,
                                                out, error)) {
                return false;
            }
            for (const auto& chunk : chunks) {
                if (chunk.extra_buffers.empty()) {
                    error = "rle chunk is missing its run-lengths buffer";
                    return false;
                }
                const auto& values = chunk.values;
                const auto& lengths = chunk.extra_buffers[0];
                if (bpv == 0U || values.size() % bpv != 0U) {
                    error = "rle chunk buffer sizes invalid";
                    return false;
                }
                const std::size_t num_runs = values.size() / bpv;
                if (num_runs != lengths.size() / length_bytes) {
                    error = "rle run count mismatch between values and lengths";
                    return false;
                }
                for (std::size_t r = 0; r < num_runs; ++r) {
                    const std::uint64_t run = lengths[r];
                    if (!append_repeated_value(out.fixed, values.data() + r * bpv, bpv,
                                               static_cast<std::size_t>(run))) {
                        error = "rle run expansion overflows";
                        return false;
                    }
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
            // Decode the dictionary: unwrap -> [u32 32][u32 bytes_start][u32 offsets][data].
            if (!decompress_dictionary_block(encoding_plan.dict_scheme, dict_frame, dict_block, error)) {
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

    // Structural dictionary column: buffer[1] = bitpacked u32 indices, buffer[2] = dictionary.
    //
    // The dictionary block comes in two shapes and the descriptor says which. `Variable` is a block
    // with an offset header, which is what a low-cardinality string column gets. `Flat(N)` is N-bit
    // values end to end with no header at all, which is what Lance builds for a time64 or decimal128
    // column once its cardinality justifies a dictionary. Reading a flat block as a variable-width
    // one read the first value as an offset header and refused with "dict block header invalid".
    if (encoding_plan.kind == ColumnEncodingKind::kDict) {
        const bool fixed_dict = encoding_plan.dict_value_bits != 0U;
        const std::size_t dict_value_bytes = encoding_plan.dict_value_bits / 8U;
        if (fixed_dict) {
            // The descriptor and the schema have to agree about how wide a value is. They always do
            // in a file Lance wrote; a file where they disagree is the one case where trusting
            // either would silently produce shifted values, so refuse instead.
            const auto schema_bytes = lance_logical_type_value_bytes(on_disk_field.logical_type);
            if (dict_value_bytes != schema_bytes) {
                error = "dictionary declares " + std::to_string(dict_value_bytes) +
                        "-byte values but the column's type is " + std::to_string(schema_bytes) + " bytes wide";
                return false;
            }
            out.kind = ColumnValues::Kind::FixedWidth;
        } else {
            out.kind = ColumnValues::Kind::VariableWidth;
        }
        out.variable.large = lance_logical_type_has_large_offsets(on_disk_field.logical_type);
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> dict_stored;
        std::vector<std::uint8_t> dict_block;
        std::vector<std::uint8_t> indices_bytes;
        std::vector<MiniBlockChunkView> index_chunks;
        std::uint64_t validity_rows = 0;
        for (const auto& page : column_metadata.pages) {
            if (page.buffer_offsets.size() < 3U || page.buffer_sizes.size() < 3U) {
                error = "dict page missing buffers";
                return false;
            }
            if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[1], page.buffer_sizes[1], payload,
                                            error) ||
                !read_lance_data_file_bytes(data_file_path, page.buffer_offsets[2], page.buffer_sizes[2], dict_stored,
                                            error)) {
                return false;
            }
            if (!decompress_dictionary_block(encoding_plan.dict_scheme, dict_stored, dict_block, error)) {
                return false;
            }
            std::size_t num_dict = 0;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> dict_ranges;
            if (fixed_dict) {
                // No header: the block is entries of `dict_value_bytes`, and the descriptor's
                // `num_dictionary_items` is the only statement of how many. Lance pads the block, so
                // it may be longer than the entries need -- it must never be shorter.
                num_dict = static_cast<std::size_t>(encoding_plan.dict_items);
                std::uint64_t needed = 0;
                if (num_dict == 0U ||
                    !checked_mul(encoding_plan.dict_items, static_cast<std::uint64_t>(dict_value_bytes), needed) ||
                    !fits_size_t(needed) || static_cast<std::size_t>(needed) > dict_block.size()) {
                    error = "dictionary declares " + std::to_string(encoding_plan.dict_items) + " entries of " +
                            std::to_string(dict_value_bytes) + " bytes but the block holds " +
                            std::to_string(dict_block.size());
                    return false;
                }
            } else {
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
                num_dict = (bytes_start - 8U) / 4U - 1U;
                dict_ranges.resize(num_dict);
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
            }
            // The index chunks go through the same splitter every other miniblock path uses, so a
            // nullable dictionary column (a categorical column with missing values -- the shape this
            // encoding exists for) carries its definition levels here like anywhere else. The
            // dedicated chunk-list parser this replaced rejected the level buffer outright with
            // "unexpected miniblock payload prefix".
            if (!read_page_chunks_with_validity(payload, encoding_plan, page.length, index_chunks,
                                                validity_rows, out, error)) {
                return false;
            }
            if (index_chunks.empty()) {
                error = "dict page has no index chunks";
                return false;
            }
            indices_bytes.clear();  // hoisted out of the loop; accumulates fresh per page via insert()
            std::uint64_t rows_remaining = page.length;
            for (const auto& chunk : index_chunks) {
                const auto count = encoding_plan.repdef != nullptr
                                       ? static_cast<std::uint64_t>(chunk.repdef_values)
                                       : std::min<std::uint64_t>(1024U, rows_remaining);
                if (count == 0U || count > rows_remaining) {
                    error = "dict index chunk covers " + std::to_string(count) + " values with " +
                            std::to_string(rows_remaining) + " left in the page";
                    return false;
                }
                if (!unpack_bitpacked_page_dispatch(chunk.values, count, 4U, indices_bytes, error)) {
                    return false;
                }
                rows_remaining -= count;
            }
            if (rows_remaining != 0U || indices_bytes.size() != page.length * 4U) {
                error = "dict index count mismatch";
                return false;
            }
            if (fixed_dict) {
                // Every row is the same width, so the destination is sized once and written through
                // a pointer -- no per-row growth, and no offsets to maintain.
                const std::size_t base = out.fixed.size();
                out.fixed.resize(base + static_cast<std::size_t>(page.length) * dict_value_bytes);
                std::uint8_t* dest = out.fixed.data() + base;
                for (std::uint64_t r = 0; r < page.length; ++r) {
                    std::uint32_t index = 0;
                    std::memcpy(&index, indices_bytes.data() + r * 4U, 4U);
                    if (index >= num_dict) {
                        error = "dict index out of range";
                        return false;
                    }
                    std::memcpy(dest, dict_block.data() + static_cast<std::size_t>(index) * dict_value_bytes,
                                dict_value_bytes);
                    dest += dict_value_bytes;
                }
                continue;
            }
            const bool first_page = out.variable.offsets.empty();
            std::uint64_t cumulative = out.variable.data.size();
            // Same reasoning as expand_fsst_values(): a row's length comes from the dictionary entry
            // its index selects, so the offsets are built per row -- but only grown once per page.
            out.variable.offsets.reserve(out.variable.offsets.size() +
                                         static_cast<std::size_t>(page.length) * (out.variable.large ? 8U : 4U));
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
        const auto value_scheme = encoding_plan.value_scheme;
        const bool nullable = encoding_plan.repdef != nullptr;
        // The offset width of the block actually stored in the chunk. Without FSST that is the
        // column's own (utf8 -> 32-bit, large_utf8 -> 64-bit). With FSST the stored block is the
        // COMPRESSED one, whose offsets index compressed bytes and whose width the descriptor
        // declares independently (Fsst.values = Variable{Flat(bits)}).
        const bool stored_offsets_large = encoding_plan.fsst && encoding_plan.variable_offset_bits != 0U
                                              ? encoding_plan.variable_offset_bits == 64U
                                              : out.variable.large;
        const auto offset_width = static_cast<std::uint64_t>(stored_offsets_large ? 8U : 4U);
        std::vector<std::uint8_t> control;
        std::vector<std::uint8_t> payload;
        std::vector<MiniBlockChunkView> chunks;
        std::vector<std::uint8_t> raw;
        // Scratch for the FSST path only; hoisted so their capacity is reused across chunks.
        std::vector<std::uint8_t> fsst_offsets;
        std::vector<std::uint8_t> fsst_data;
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
            for (auto& chunk : chunks) {
                if (value_scheme == page_layout::BufferScheme::kZstd) {
                    // The value buffer is a [u64 uncompressed size][zstd frame] envelope.
                    if (!zstd_unframe_buffer(chunk.values, raw, error)) {
                        return false;
                    }
                    // swap (not move): a move would leave `raw` empty every iteration, discarding its
                    // capacity right when the next chunk's call could have reused it.
                    std::swap(chunk.values, raw);
                } else if (value_scheme == page_layout::BufferScheme::kLz4) {
                    // LZ4's envelope is [u32 uncompressed size][block] -- see lz4_block.hpp.
                    if (!lz4_block::decompress_sized(chunk.values, raw, error)) {
                        return false;
                    }
                    std::swap(chunk.values, raw);
                }
                // How many values the chunk holds. With definition levels the chunk header states it.
                // Without them it is still derivable from the chunk itself rather than assumed: a
                // variable-width chunk starts with an (n+1)-entry offset table and offsets[0] is the
                // byte position where the data begins, i.e. exactly (n+1) * offset_width. Deriving it
                // is what lets a multi-chunk page written by stock Lance decode at all -- the previous
                // code concatenated every chunk and handed the whole page to one offset table, which
                // only ever worked because nanolance's writer emits one chunk per page.
                std::uint64_t chunk_values = 0;
                if (nullable) {
                    chunk_values = chunk.repdef_values;
                } else {
                    const auto data_base = read_list_offset(chunk.values, 0, stored_offsets_large);
                    if (data_base < static_cast<std::int64_t>(offset_width) ||
                        static_cast<std::uint64_t>(data_base) % offset_width != 0U) {
                        error = "variable-width chunk has no usable offset table";
                        return false;
                    }
                    chunk_values = static_cast<std::uint64_t>(data_base) / offset_width - 1U;
                }
                if (chunk_values == 0U || chunk_values > remaining) {
                    error = "variable-width chunk covers " + std::to_string(chunk_values) +
                            " values with " + std::to_string(remaining) + " left in the page";
                    return false;
                }
                if (encoding_plan.fsst) {
                    // The FSST-compressed bytes are themselves a variable-width block, so decode
                    // that into scratch buffers first and then expand each value onto the column's.
                    fsst_offsets.clear();
                    fsst_data.clear();
                    if (!decode_variable_width_page(chunk.values, chunk_values, stored_offsets_large,
                                                    fsst_offsets, fsst_data, error)) {
                        return false;
                    }
                    if (!expand_fsst_values(*encoding_plan.fsst, fsst_offsets, stored_offsets_large,
                                            fsst_data, chunk_values, out.variable.large,
                                            out.variable.offsets, out.variable.data, error)) {
                        return false;
                    }
                } else if (!decode_variable_width_page(chunk.values, chunk_values, out.variable.large,
                                                       out.variable.offsets, out.variable.data, error)) {
                    return false;
                }
                remaining -= chunk_values;
            }
            if (remaining != 0U) {
                error = "variable-width page chunks cover fewer rows than the page declares";
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
        if (!split_miniblock_payload(payload, encoding_plan.chunk_shape, chunks, error)) {
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
