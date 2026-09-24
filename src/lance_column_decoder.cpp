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
#include "nanolance/repdef.hpp"
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
    // The declared size decides the allocation, and it is written by the file -- as is the zstd frame's
    // own content size, so the cross-check below proves only that the two agree, not that either is
    // true. An 899-byte page declaring 8.4 GB passed both and was allocated; found by
    // tests/fuzz/fuzz_column_decode.cpp within 10,000 executions.
    //
    // The bound has to come from bytes that are actually present. zstd cannot expand more than its
    // best block allows: an RLE block is 4 bytes (3-byte header + the byte) for at most 128 KiB of
    // output, so no frame exceeds 32768x its compressed size. Measured: 512 MiB of zeros at level 22
    // compresses 32,732:1. LZ4 has the same kind of bound (255x) in lz4_block.cpp, for the same reason.
    const std::uint64_t compressed = static_cast<std::uint64_t>(framed.size() - 8U);
    if (uncompressed > default_read_limits().max_uncompressed_bytes || !fits_size_t(uncompressed) ||
        uncompressed > compressed * kZstdMaxExpansion) {
        error = "zstd frame declares " + std::to_string(uncompressed) + " uncompressed bytes from " +
                std::to_string(compressed) + " compressed, more than zstd can produce";
        return false;
    }
    const unsigned long long content =
        ZSTD_getFrameContentSize(framed.data() + 8U, framed.size() - 8U);
    if (content != ZSTD_CONTENTSIZE_UNKNOWN && content != ZSTD_CONTENTSIZE_ERROR &&
        content != uncompressed) {
        error = "zstd frame content size disagrees with declared size";
        return false;
    }
    // Past this size the declared length is no longer trusted with an up-front allocation: a frame
    // can legitimately claim 32768x its compressed size, so an 85 KiB page could still ask for
    // 2.8 GB before a byte of it was shown to exist (fuzz_column_decode found that too). Large
    // frames are streamed instead, and the buffer grows only as real output arrives.
    constexpr std::uint64_t kPreallocateUpTo = std::uint64_t{64} << 20U;
    if (uncompressed > kPreallocateUpTo) {
        thread_local std::unique_ptr<ZSTD_DStream, std::size_t (*)(ZSTD_DStream*)> stream(ZSTD_createDStream(),
                                                                                         &ZSTD_freeDStream);
        if (stream == nullptr || ZSTD_isError(ZSTD_initDStream(stream.get())) != 0U) {
            error = "zstd stream could not be created";
            return false;
        }
        out.clear();
        ZSTD_inBuffer in{framed.data() + 8U, framed.size() - 8U, 0};
        std::size_t produced = 0;
        std::size_t ret = 1;
        while (ret != 0U) {
            if (produced == out.size()) {
                if (produced == static_cast<std::size_t>(uncompressed)) {
                    error = "zstd frame decompresses past its declared size";
                    return false;
                }
                const auto grown = std::min<std::uint64_t>(uncompressed, std::max<std::uint64_t>(produced * 2U, kPreallocateUpTo));
                out.resize(static_cast<std::size_t>(grown));
            }
            ZSTD_outBuffer dst{out.data(), out.size(), produced};
            const auto before_in = in.pos;
            ret = ZSTD_decompressStream(stream.get(), &dst, &in);
            if (ZSTD_isError(ret) != 0U) {
                error = "zstd decompress failed";
                return false;
            }
            if (dst.pos == produced && in.pos == before_in && ret != 0U) {
                error = "zstd frame is truncated";
                return false;
            }
            produced = dst.pos;
        }
        if (produced != uncompressed) {
            error = "zstd frame decompressed to " + std::to_string(produced) + " bytes, not the declared " +
                    std::to_string(uncompressed);
            return false;
        }
        out.resize(produced);
        return true;
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
    /// List pages only: the repetition-level buffer, the header's level count, and the chunk's value
    /// count from its metadata word. Levels outnumber values there (an empty or null list takes a
    /// level and no value), so neither count can stand in for the other.
    std::vector<std::uint8_t> rep;
    std::uint32_t num_levels = 0;
    std::uint64_t items = 0;
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
        chunk.num_levels = header.num_levels;
        if (shape.has_repetition && header.rep_size != 0U) {
            chunk.rep.assign(payload.begin() + static_cast<std::ptrdiff_t>(at),
                             payload.begin() + static_cast<std::ptrdiff_t>(at + header.rep_size));
        }
        at += align_to_miniblock(header.rep_size);
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
    reserve_more(out_offsets, static_cast<std::size_t>(num_values) * (out_large ? 8U : 4U));
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



/// Unpack a buffer that is a SERIES of inline-bitpacked FastLanes blocks into `count` values.
///
/// `unpack_bitpacked_page` above decodes exactly one block, because that is what a miniblock chunk
/// carries -- the chunk splitter has already cut the buffer up. A dictionary buffer has no such
/// splitter: it is one buffer holding however many blocks its entries need, each with its own bit
/// width word, so the blocks have to be walked. Widths differ per block, so the block size cannot be
/// computed up front; the position advances as each header is read.
template <class T>
[[nodiscard]] bool unpack_inline_bitpacked_series(const std::vector<std::uint8_t>& buffer, std::size_t count,
                                                  std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    out.reserve(count * sizeof(T));
    thread_local std::vector<T> packed;
    T values[1024];
    std::size_t at = 0;
    std::size_t done = 0;
    while (done < count) {
        if (buffer.size() - at < sizeof(T)) {
            error = "inline-bitpacked dictionary ends before its " + std::to_string(count) + " entries do";
            return false;
        }
        T width_word = 0;
        std::memcpy(&width_word, buffer.data() + at, sizeof(T));
        at += sizeof(T);
        const auto width = static_cast<unsigned>(width_word);
        if (width > sizeof(T) * 8U) {
            error = "inline-bitpacked dictionary block has invalid bit width " + std::to_string(width);
            return false;
        }
        const auto packed_words = nano_lance::fastlanes::packed_words_1024<T>(width);
        const std::size_t block_bytes = packed_words * sizeof(T);
        if (buffer.size() - at < block_bytes) {
            error = "inline-bitpacked dictionary block is truncated";
            return false;
        }
        packed.resize(packed_words != 0U ? packed_words : 1U);
        if (packed_words != 0U) {
            std::memcpy(packed.data(), buffer.data() + at, block_bytes);
        }
        at += block_bytes;
        nano_lance::fastlanes::unpack_1024<T>(width, packed.data(), values);
        // A block always holds 1024 values; the last one is only partly used.
        const std::size_t take = std::min<std::size_t>(1024U, count - done);
        const auto* p = reinterpret_cast<const std::uint8_t*>(values);
        out.insert(out.end(), p, p + take * sizeof(T));
        done += take;
    }
    return true;
}

/// Unpack an OUT-OF-LINE bitpacked buffer: `count` values of `T` packed at `width` bits, with the
/// width taken from the DESCRIPTOR (`Bitpacked{uncompressed_bits, Flat(width)}`) rather than from a
/// word at the head of each block. Writes exactly `count` values to `dest`.
///
/// The buffer is a run of whole FastLanes blocks followed by a tail, and the tail has two legal
/// spellings: packed-and-padded, or raw `T` words. Lance's `unpack_out_of_line`
/// (`rust/lance-encoding/src/encodings/physical/bitpacking.rs`) tells them apart by the buffer's
/// total LENGTH -- the encoder pads only when padding costs fewer bits than packing saves -- and this
/// mirrors that arithmetic exactly rather than approximating it:
///
///     whole_blocks = count / 1024              tail = count % 1024
///     raw  <=>  words == whole_blocks * packed_words + tail
///
/// `what` names the buffer in any error, since both definition levels and dictionaries come here.
template <class T>
[[nodiscard]] bool unpack_out_of_line_bitpacked(const std::vector<std::uint8_t>& buffer, std::size_t count,
                                                unsigned width, T* dest, const char* what, std::string& error) {
    // Width 0 is legal, not a malformed descriptor: it is how an all-zero buffer packs, and Lance
    // has no guard against producing it. `unpack_1024` emits zeros for it and the size arithmetic
    // below stays well defined (`packed_words` is 0), so refusing it would reject a file Lance can
    // write. Only a width wider than the element itself is nonsense.
    if (width > sizeof(T) * 8U) {
        error = std::string(what) + " declares an unsupported width of " + std::to_string(width) + " bits";
        return false;
    }
    if (buffer.size() % sizeof(T) != 0U) {
        error = std::string(what) + " is not a whole number of " + std::to_string(sizeof(T) * 8U) + "-bit words";
        return false;
    }
    const auto packed_words = nano_lance::fastlanes::packed_words_1024<T>(width);
    const auto words = buffer.size() / sizeof(T);
    const std::size_t whole_blocks = count / 1024U;
    const std::size_t tail = count % 1024U;
    const std::size_t full_words = whole_blocks * packed_words;
    const bool tail_is_raw = tail != 0U && words == full_words + tail;
    const std::size_t expected_words =
        tail_is_raw ? full_words + tail : full_words + (tail != 0U ? packed_words : 0U);
    if (words != expected_words) {
        error = std::string(what) + " is " + std::to_string(buffer.size()) + " bytes, expected " +
                std::to_string(expected_words * sizeof(T)) + " for " + std::to_string(count) +
                " values at " + std::to_string(width) + " bits";
        return false;
    }

    thread_local std::vector<T> packed;
    packed.resize(packed_words != 0U ? packed_words : 1U);
    T block[1024];
    std::size_t word_at = 0;
    // At width 0 there are no packed words and the buffer may be empty, with a null data(): memcpy
    // from null is undefined even for zero bytes, so the copies are skipped rather than made empty.
    for (std::size_t b = 0; b < whole_blocks; ++b) {
        if (packed_words != 0U) {
            std::memcpy(packed.data(), buffer.data() + word_at * sizeof(T), packed_words * sizeof(T));
        }
        nano_lance::fastlanes::unpack_1024<T>(width, packed.data(), block);
        std::memcpy(dest + b * 1024U, block, 1024U * sizeof(T));
        word_at += packed_words;
    }
    if (tail != 0U) {
        if (tail_is_raw) {
            std::memcpy(dest + whole_blocks * 1024U, buffer.data() + word_at * sizeof(T), tail * sizeof(T));
        } else {
            if (packed_words != 0U) {
                std::memcpy(packed.data(), buffer.data() + word_at * sizeof(T), packed_words * sizeof(T));
            }
            nano_lance::fastlanes::unpack_1024<T>(width, packed.data(), block);
            std::memcpy(dest + whole_blocks * 1024U, block, tail * sizeof(T));
        }
    }
    return true;
}

[[nodiscard]] bool unpack_out_of_line_bitpacked_dispatch(const std::vector<std::uint8_t>& buffer,
                                                         std::size_t count, unsigned width,
                                                         std::size_t bytes_per_value, std::uint8_t* dest,
                                                         const char* what, std::string& error) {
    switch (bytes_per_value) {
        case 1U:
            return unpack_out_of_line_bitpacked<std::uint8_t>(buffer, count, width, dest, what, error);
        case 2U:
            return unpack_out_of_line_bitpacked<std::uint16_t>(buffer, count, width,
                                                               reinterpret_cast<std::uint16_t*>(dest), what, error);
        case 4U:
            return unpack_out_of_line_bitpacked<std::uint32_t>(buffer, count, width,
                                                               reinterpret_cast<std::uint32_t*>(dest), what, error);
        case 8U:
            return unpack_out_of_line_bitpacked<std::uint64_t>(buffer, count, width,
                                                               reinterpret_cast<std::uint64_t*>(dest), what, error);
        default:
            error = std::string(what) + " has " + std::to_string(bytes_per_value) +
                    "-byte values, which has no FastLanes kernel";
            return false;
    }
}

[[nodiscard]] bool unpack_inline_bitpacked_series_dispatch(const std::vector<std::uint8_t>& buffer,
                                                           std::size_t count, std::size_t bytes_per_value,
                                                           std::vector<std::uint8_t>& out, std::string& error) {
    switch (bytes_per_value) {
        case 1U:
            return unpack_inline_bitpacked_series<std::uint8_t>(buffer, count, out, error);
        case 2U:
            return unpack_inline_bitpacked_series<std::uint16_t>(buffer, count, out, error);
        case 4U:
            return unpack_inline_bitpacked_series<std::uint32_t>(buffer, count, out, error);
        case 8U:
            return unpack_inline_bitpacked_series<std::uint64_t>(buffer, count, out, error);
        default:
            // FastLanes kernels exist for 8/16/32/64-bit lanes only. A 16-byte decimal is never
            // inline-bitpacked by Lance for exactly that reason; refuse by width rather than guess.
            error = "inline-bitpacked dictionary entries are " + std::to_string(bytes_per_value) +
                    " bytes, which has no FastLanes kernel";
            return false;
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
/// Decode one chunk's `count` repetition or definition levels (Lance levels are u16) into `levels`.
/// Handles every spelling Lance uses for them: Rle, InlineBitpacking(16), Bitpacked{16, Flat(bits)},
/// and raw Flat(16).
[[nodiscard]] bool decode_levels(const std::vector<std::uint8_t>& bytes, const page_layout::Compressive& encoding,
                                 std::uint32_t count, std::uint16_t* levels, std::string& error) {
    if (encoding.kind == page_layout::CompressiveKind::kRle) {
        return decode_rle_definition_levels(bytes, encoding, count, levels, error);
    }
    // InlineBitpacking(16): the same FastLanes block a bitpacked *value* page carries, with its bit
    // width as the first u16 of the buffer rather than in the descriptor -- which is what "inline"
    // means. Lance picks this over Bitpacked{Flat(bits)} for the definition levels of some page
    // sizes; empirically, a pylance nullable string column of 200..1000 rows lands here while 100 and
    // 2000 do not, so refusing it made a common, unremarkable dataset unreadable.
    //
    // The 16 is the uncompressed element width, and Lance's levels are u16, so it is the only width
    // that can appear. Anything else is refused by name rather than guessed at.
    if (encoding.kind == page_layout::CompressiveKind::kInlineBitpacking) {
        if (encoding.bits_per_value != 16U) {
            error = "levels declare InlineBitpacking(" + std::to_string(encoding.bits_per_value) +
                    "); only 16-bit levels exist";
            return false;
        }
        thread_local std::vector<std::uint8_t> unpacked;
        unpacked.clear();
        if (!unpack_bitpacked_page<std::uint16_t>(bytes, count, unpacked, error)) {
            return false;
        }
        std::memcpy(levels, unpacked.data(), unpacked.size());
        return true;
    }
    // Raw u16 levels. A small list page's repetition levels arrive like this.
    if (encoding.kind == page_layout::CompressiveKind::kFlat) {
        if (encoding.bits_per_value != 16U || bytes.size() != static_cast<std::size_t>(count) * 2U) {
            error = "flat levels are " + std::to_string(bytes.size()) + " bytes for " + std::to_string(count) +
                    " levels at " + std::to_string(encoding.bits_per_value) + " bits";
            return false;
        }
        if (!bytes.empty()) {
            std::memcpy(levels, bytes.data(), bytes.size());
        }
        return true;
    }
    if (encoding.kind != page_layout::CompressiveKind::kBitpacked || encoding.values == nullptr ||
        encoding.values->kind != page_layout::CompressiveKind::kFlat) {
        error = "unsupported level encoding: " + page_layout::describe_encoding(encoding);
        return false;
    }
    // `unpack_out_of_line_bitpacked` rejects a width of 0 or one wider than the element, so there is
    // no second bound to keep in step here. Two cases this has to keep getting right: a 20000-row
    // nullable binary column from pylance ends in a 32-value chunk whose 64-byte level buffer is raw,
    // and a 1025-row bool column is one whole packed block (128 bytes) plus a single raw u16.
    return unpack_out_of_line_bitpacked<std::uint16_t>(bytes, count, encoding.values->bits_per_value, levels,
                                                       "level buffer", error);
}

/// Decode one chunk's definition-level buffer, appending one bit per row to `out_validity` (an
/// Arrow-convention bitmap: LSB-first, bit SET means VALID) and counting the nulls.
///
/// Lance stores one definition level per value, and for a simple nullable column **level 1 means
/// NULL** -- the opposite polarity to Arrow's validity bit, hence the inversion.
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
    if (!decode_levels(repdef, encoding, count, level_storage.data(), error)) {
        return false;
    }
    return append_levels_to_validity(level_storage.data(), count, rows_already_appended, out_validity,
                                     out_null_count);
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
    kFullZip,
    kUnsupported,
};

/// A list column's values, seen as a flat column of ITEMS (see decode_list_column). Each page's
/// chunk value counts come from its metadata words, computed once by the level pre-pass and consumed
/// here in page order by every value path's chunk split.
struct ItemView {
    std::vector<std::vector<std::uint64_t>> page_chunk_items;
    std::size_t next_page = 0;
};

struct ColumnEncodingPlan {
    ColumnEncodingKind kind = ColumnEncodingKind::kFlat;
    /// Set only while decoding a list column's items; null otherwise.
    std::shared_ptr<ItemView> item_view;
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
    /// kConstant whose layers declare a definition layer. That alone does NOT say the column is
    /// all-null: it is equally how an ordinary nullable constant column is spelled, the same value in
    /// every non-null row. Which of the two a page is depends on whether it carries a value, and that
    /// is a property of the PAGE (its buffer count), not of the descriptor -- so it is decided at
    /// decode time. See `constant_def_buffer_index`.
    bool constant_declares_levels = false;
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
    /// kFlat over FixedSizeList{N, Flat, has_validity}: each chunk's first buffer is N validity bits
    /// per row (one per element, LSB-first, contiguous across rows) and its second the values.
    std::uint64_t fsl_items = 0;
    bool fsl_item_validity = false;
    /// kDict / kDictRle: the fixed-width dictionary's entries are themselves FastLanes bit-packed
    /// rather than stored flat. Lance picks this once the entries are narrow enough for packing to
    /// pay -- an int64 column with runs gets it. It comes in two spellings, and they differ only in
    /// where the bit width is written: `InlineBitpacking(N)` puts it at the head of each block,
    /// `Bitpacked{N, Flat(width)}` puts it in the descriptor. Zero means the entries are flat.
    std::uint32_t dict_packed_width = 0;  // set only for the out-of-line spelling
    bool dict_inline_bitpacked = false;
    bool dict_out_of_line_bitpacked = false;
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

/// A page's dictionary, in whichever of its two shapes the descriptor declared.
///
/// `Variable` is a block with an offset header: [u32][u32 bytes_start][u32 offsets...][bytes]. That
/// is what a low-cardinality string column gets. `Flat(N)` is N-bit entries end to end with NO
/// header, which is what Lance builds for a temporal, decimal or integer column once a dictionary
/// pays for itself. Only the descriptor distinguishes them, and reading a flat block as a
/// variable-width one takes its first entry for an offset header.
///
/// Both dictionary branches -- plain indices and RLE'd indices -- parse the same block, so they
/// share this rather than keeping two copies that can drift apart.
struct DictionaryBlock {
    std::vector<std::uint8_t> bytes;
    /// Variable-width: (start, length) per entry. Empty for a fixed-width dictionary.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    /// Fixed-width: bytes per entry. Zero when the dictionary is variable-width.
    std::size_t value_bytes = 0;
    std::size_t count = 0;

    bool is_fixed_width() const { return value_bytes != 0U; }
    const std::uint8_t* entry(std::size_t index) const {
        return bytes.data() + (is_fixed_width() ? index * value_bytes : ranges[index].first);
    }
    std::size_t entry_size(std::size_t index) const {
        return is_fixed_width() ? value_bytes : ranges[index].second;
    }
};

[[nodiscard]] bool decode_dictionary_block(const ColumnEncodingPlan& plan, const std::string& logical_type,
                                           const std::vector<std::uint8_t>& stored, DictionaryBlock& out,
                                           std::string& error) {
    if (!decompress_dictionary_block(plan.dict_scheme, stored, out.bytes, error)) {
        return false;
    }
    out.ranges.clear();
    if (plan.dict_value_bits != 0U) {
        out.value_bytes = plan.dict_value_bits / 8U;
        // The descriptor and the schema have to agree about how wide a value is. They always do in a
        // file Lance wrote; where they disagree, trusting either one alone would silently shift every
        // value, so refuse instead.
        const auto schema_bytes = lance_logical_type_value_bytes(logical_type);
        if (out.value_bytes != schema_bytes) {
            error = "dictionary declares " + std::to_string(out.value_bytes) +
                    "-byte values but the column's type is " + std::to_string(schema_bytes) + " bytes wide";
            return false;
        }
        // A headerless block states neither its entry width nor its count, so `num_dictionary_items`
        // is the only statement of how many there are. Lance pads the block, so it may be longer than
        // the entries need -- it must never be shorter.
        out.count = static_cast<std::size_t>(plan.dict_items);
        std::uint64_t needed = 0;
        if (out.count == 0U ||
            !checked_mul(plan.dict_items, static_cast<std::uint64_t>(out.value_bytes), needed) ||
            !fits_size_t(needed) || needed > default_read_limits().max_uncompressed_bytes) {
            error = "dictionary declares an implausible " + std::to_string(plan.dict_items) + " entries of " +
                    std::to_string(out.value_bytes) + " bytes";
            return false;
        }
        if (plan.dict_out_of_line_bitpacked) {
            std::vector<std::uint8_t> unpacked(static_cast<std::size_t>(needed));
            if (!unpack_out_of_line_bitpacked_dispatch(out.bytes, out.count, plan.dict_packed_width,
                                                       out.value_bytes, unpacked.data(), "dictionary block",
                                                       error)) {
                return false;
            }
            out.bytes = std::move(unpacked);
            return true;
        }
        if (plan.dict_inline_bitpacked) {
            // The count is what bounds the walk, and the buffer is what bounds the count: a series of
            // packed blocks is far smaller than the values it yields, so the size check below cannot
            // be applied to the stored bytes. `unpack_inline_bitpacked_series` refuses a count the
            // buffer cannot actually supply, which is the same guarantee from the other direction.
            std::vector<std::uint8_t> unpacked;
            if (!unpack_inline_bitpacked_series_dispatch(out.bytes, out.count, out.value_bytes, unpacked, error)) {
                return false;
            }
            out.bytes = std::move(unpacked);
            return true;
        }
        if (static_cast<std::size_t>(needed) > out.bytes.size()) {
            error = "dictionary declares " + std::to_string(plan.dict_items) + " entries of " +
                    std::to_string(out.value_bytes) + " bytes but the block holds " +
                    std::to_string(out.bytes.size());
            return false;
        }
        return true;
    }

    out.value_bytes = 0;
    if (out.bytes.size() < 8U) {
        error = "dict block too short";
        return false;
    }
    std::uint32_t bytes_start = 0;
    std::memcpy(&bytes_start, out.bytes.data() + 4U, 4U);
    if (bytes_start < 12U || bytes_start > out.bytes.size() || (bytes_start - 8U) % 4U != 0U) {
        error = "dict block header invalid";
        return false;
    }
    out.count = (bytes_start - 8U) / 4U - 1U;
    out.ranges.resize(out.count);
    for (std::size_t d = 0; d < out.count; ++d) {
        std::uint32_t a = 0;
        std::uint32_t b = 0;
        std::memcpy(&a, out.bytes.data() + 8U + d * 4U, 4U);
        std::memcpy(&b, out.bytes.data() + 8U + (d + 1U) * 4U, 4U);
        // In 64 bits: `bytes_start + b` in u32 wraps for an offset near 2^32, which passed this check
        // and handed the copy below a ~4 GiB entry length (found by fuzz_column_decode).
        if (static_cast<std::uint64_t>(bytes_start) + b > out.bytes.size() || b < a) {
            error = "dict offsets out of range";
            return false;
        }
        out.ranges[d] = {bytes_start + a, b - a};
    }
    return true;
}

/// Appends `count` fixed_size_list ELEMENT validity bits, read LSB-first from `src` starting at bit
/// `src_bit`, at element `at` of `out.item_validity`. `src == nullptr` appends valid elements. The
/// bitmap is materialized lazily: nothing is stored until the first null element, at which point
/// every element before it is filled in as valid.
void append_item_validity(ColumnValues& out, const std::uint8_t* src, std::uint64_t src_bit, std::uint64_t count,
                          std::uint64_t at) {
    const auto set_valid_upto = [&out](std::uint64_t from, std::uint64_t to) {
        out.item_validity.resize(static_cast<std::size_t>((to + 7U) / 8U), 0U);
        for (std::uint64_t i = from; i < to; ++i) {
            out.item_validity[static_cast<std::size_t>(i >> 3U)] |= static_cast<std::uint8_t>(1U << (i & 7U));
        }
    };
    if (src == nullptr) {
        if (!out.item_validity.empty()) {
            set_valid_upto(at, at + count);
        }
        return;
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto b = src_bit + i;
        const bool valid = ((src[static_cast<std::size_t>(b >> 3U)] >> (b & 7U)) & 1U) != 0U;
        const auto dst = at + i;
        if (out.item_validity.empty()) {
            if (valid) {
                continue;  // still all valid
            }
            set_valid_upto(0, dst);  // materialize: every element before this one was valid
        }
        if (out.item_validity.size() < static_cast<std::size_t>((dst + 8U) / 8U)) {
            out.item_validity.resize(static_cast<std::size_t>((dst + 8U) / 8U), 0U);
        }
        if (valid) {
            out.item_validity[static_cast<std::size_t>(dst >> 3U)] |= static_cast<std::uint8_t>(1U << (dst & 7U));
        } else {
            ++out.item_null_count;
        }
    }
}

/// What decoding one FullZip page needs, derived from THAT page's descriptor. Not hoisted to the
/// column: every page carries its own descriptor, and with it its own FSST symbol table and its own
/// nullability, so the first page's values are not the second page's.
///
/// Byte layout (verified against page buffer sizes; see docs/ROADMAP.md, B1): each row is a control
/// word of 0/1/2/4 bytes holding its definition level, then either a fixed-width value slot -- present
/// even for a null row -- or, for a valid variable-width row only, a length prefix and the bytes.
struct FullZipPageParams {
    std::size_t control_bytes = 0;
    std::uint32_t bits_def = 0;
    std::size_t value_bytes = 0;   // fixed width; 0 for variable width
    std::size_t length_bytes = 0;  // variable width: the length prefix, 4 or 8; 0 for fixed width
    std::optional<fsst::SymbolTable> fsst;
    /// fixed_size_list with element validity: each slot starts with ceil(items/8) bytes of per-element
    /// validity bits, then the items. `value_bytes` counts both.
    std::uint64_t items = 0;
    std::size_t item_validity_bytes = 0;
};

/// Fills `out` from a FullZip layout, or explains why it cannot be read.
bool full_zip_page_params(const page_layout::PageLayout& layout, std::uint64_t page_rows,
                          FullZipPageParams& out, std::string& why) {
    out = FullZipPageParams{};
    if (layout.kind != page_layout::LayoutKind::kFullZip) {
        why = "a FullZip column has a page with a different layout";
        return false;
    }
    const auto& fz = layout.full_zip;
    if (fz.bits_rep != 0U) {
        why = "repeated values (lists) in a FullZip page are not read yet";
        return false;
    }
    if (fz.num_items != page_rows || fz.num_visible_items != page_rows) {
        why = "FullZip page declares " + std::to_string(fz.num_items) + " items (" +
              std::to_string(fz.num_visible_items) + " visible) for " + std::to_string(page_rows) + " rows";
        return false;
    }
    // One layer, describing the value itself: a list would have more.
    if (fz.layers.size() != 1U || (fz.layers[0] != 1U && fz.layers[0] != 3U)) {
        why = "unsupported FullZip layer set";
        return false;
    }
    if (fz.bits_def > 16U || (fz.bits_def != 0U) != (fz.layers[0] == 3U)) {
        why = "FullZip definition bits do not match its layers";
        return false;
    }
    out.bits_def = fz.bits_def;
    const auto total_bits = fz.bits_rep + fz.bits_def;
    out.control_bytes = total_bits == 0U ? 0U : total_bits <= 8U ? 1U : total_bits <= 16U ? 2U : 4U;

    const auto* values = fz.value_compression.get();
    if (values == nullptr) {
        why = "FullZip page has no value compression";
        return false;
    }
    if (fz.bits_per_offset != 0U) {
        if (fz.bits_per_offset != 32U && fz.bits_per_offset != 64U) {
            why = "unsupported FullZip length width of " + std::to_string(fz.bits_per_offset) + " bits";
            return false;
        }
        out.length_bytes = fz.bits_per_offset / 8U;
        // Per-value compression: Variable is the bytes as they are; Fsst compresses each value on
        // its own, against this page's symbol table.
        if (values->kind == page_layout::CompressiveKind::kFsst) {
            fsst::SymbolTable table;
            if (!fsst::parse_symbol_table(values->symbol_table, table, why)) {
                return false;
            }
            out.fsst = table;
            values = values->values.get();
        }
        if (values == nullptr || values->kind != page_layout::CompressiveKind::kVariable) {
            why = "unsupported FullZip value compression";
            return false;
        }
        return true;
    }
    if (fz.bits_per_value == 0U || fz.bits_per_value % 8U != 0U) {
        why = "FullZip value width of " + std::to_string(fz.bits_per_value) + " bits is not whole bytes";
        return false;
    }
    out.value_bytes = fz.bits_per_value / 8U;
    // The slot is either one flat value, or a fixed_size_list's N flat items back to back. Either way
    // the declared width has to be exactly what the value encoding says, or the stride is wrong.
    std::uint64_t described_bits = 0;
    if (values->kind == page_layout::CompressiveKind::kFlat) {
        described_bits = values->bits_per_value;
    } else if (values->kind == page_layout::CompressiveKind::kFixedSizeList && values->values != nullptr &&
               values->values->kind == page_layout::CompressiveKind::kFlat && values->items_per_value != 0U &&
               values->values->bits_per_value % 8U == 0U) {
        // With element validity the slot is [ceil(N/8) bytes of bits][N items]; Lance's
        // bits_per_value counts both (a nullable 768 x float32 vector is 25344 bits).
        const std::uint64_t validity_bytes = values->has_validity ? (values->items_per_value + 7U) / 8U : 0U;
        if (!checked_mul(values->items_per_value, static_cast<std::uint64_t>(values->values->bits_per_value),
                         described_bits) ||
            described_bits > std::numeric_limits<std::uint64_t>::max() - validity_bytes * 8U) {
            described_bits = 0;
        } else {
            described_bits += validity_bytes * 8U;
            out.items = values->items_per_value;
            out.item_validity_bytes = static_cast<std::size_t>(validity_bytes);
        }
    }
    if (described_bits != fz.bits_per_value) {
        why = "unsupported FullZip value compression";
        return false;
    }
    return true;
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
        out.constant_declares_levels = page_layout::layers_have_definition_levels(layout.constant.layers);
        return true;
    }
    if (layout.kind == page_layout::LayoutKind::kFullZip) {
        // Validate against the first page so an unreadable column is refused before anything is
        // read, with the reason. Decode re-derives the parameters page by page.
        FullZipPageParams params;
        std::string why;
        if (!full_zip_page_params(layout, column_metadata.pages.front().length, params, why)) {
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason = "unsupported page layout: " + page_layout::describe(layout) + " (" + why + ")";
            return true;
        }
        out.kind = ColumnEncodingKind::kFullZip;
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
        const bool dict_is_fixed_width = dict != nullptr &&
                                         (dict->kind == page_layout::CompressiveKind::kFlat ||
                                          dict->kind == page_layout::CompressiveKind::kInlineBitpacking ||
                                          dict->kind == page_layout::CompressiveKind::kBitpacked);
        if (dict == nullptr || (dict->kind != page_layout::CompressiveKind::kVariable && !dict_is_fixed_width)) {
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason = "unsupported dictionary encoding in " + page_layout::describe(layout);
            return true;
        }
        if (dict_is_fixed_width) {
            // A dictionary of fixed-width entries, in one of its two spellings. `Flat(N)` is the
            // entries end to end -- `Flat(64)` for time64 past 1024 rows, `Flat(128)` for decimal128
            // past 20000. `InlineBitpacking(N)` is the same entries FastLanes-packed, which Lance
            // picks once they are narrow enough for packing to pay: an int64 column with runs and a
            // couple of hundred distinct values gets it. Neither has the offset header the
            // variable-width path reads.
            if (dict->bits_per_value == 0U || dict->bits_per_value % 8U != 0U) {
                out.kind = ColumnEncodingKind::kUnsupported;
                out.unsupported_reason =
                    "dictionary values are " + std::to_string(dict->bits_per_value) +
                    " bits, which is not a whole number of bytes, in " + page_layout::describe(layout);
                return true;
            }
            out.dict_value_bits = dict->bits_per_value;
            out.dict_items = layout.mini_block.num_dictionary_items;
            out.dict_inline_bitpacked = dict->kind == page_layout::CompressiveKind::kInlineBitpacking;
            if (dict->kind == page_layout::CompressiveKind::kBitpacked) {
                // `Bitpacked{uncompressed_bits, Flat(width)}`: the packed width lives in the
                // descriptor, so there is no width word to read at the head of each block. Lance
                // switches to this spelling from the inline one as the dictionary grows -- an int64
                // column with runs gets InlineBitpacking at a few hundred entries and this past a
                // couple of thousand.
                if (dict->values == nullptr || dict->values->kind != page_layout::CompressiveKind::kFlat) {
                    out.kind = ColumnEncodingKind::kUnsupported;
                    out.unsupported_reason =
                        "unsupported dictionary encoding in " + page_layout::describe(layout);
                    return true;
                }
                out.dict_out_of_line_bitpacked = true;
                out.dict_packed_width = dict->values->bits_per_value;
            }
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
        case page_layout::CompressiveKind::kFixedSizeList:
            // FixedSizeList{N, Flat(b)}: each row is N flat values back to back, which the flat path
            // reads as one N*b-bit value once the column's width is N elements (see
            // lance_logical_type_value_bytes). Item-level validity and packed items are refused by
            // name rather than read as flat bytes.
            // With has_validity the chunk carries two buffers, element bits then values.
            if (!has_dictionary && out.value_scheme == page_layout::BufferScheme::kNone && !out.fsst &&
                inner->values != nullptr && inner->values->kind == page_layout::CompressiveKind::kFlat &&
                inner->values->bits_per_value % 8U == 0U && inner->values->bits_per_value != 0U &&
                inner->items_per_value != 0U &&
                (!inner->has_validity || out.chunk_shape.num_buffers == 2U)) {
                out.kind = ColumnEncodingKind::kFlat;
                out.fsl_items = inner->items_per_value;
                out.fsl_item_validity = inner->has_validity;
                return true;
            }
            out.kind = ColumnEncodingKind::kUnsupported;
            out.unsupported_reason = "unsupported fixed_size_list encoding " + page_layout::describe(layout);
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


/// For a list column's item pass: give each chunk its value count from the metadata words, in place
/// of the level count or byte-size inference the flat paths use. A no-op for any other column.
[[nodiscard]] bool apply_item_view(const ColumnEncodingPlan& plan, std::vector<MiniBlockChunkView>& chunks,
                                   std::string& error) {
    if (!plan.item_view) {
        return true;
    }
    auto& view = *plan.item_view;
    if (view.next_page >= view.page_chunk_items.size()) {
        error = "list column has more pages than its levels were read for";
        return false;
    }
    const auto& items = view.page_chunk_items[view.next_page++];
    if (items.size() != chunks.size()) {
        error = "list page has " + std::to_string(chunks.size()) + " chunks but " + std::to_string(items.size()) +
                " metadata words";
        return false;
    }
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        chunks[i].items = items[i];
    }
    return true;
}

/// Read one page's chunks, appending each chunk's definition levels to `out` when the column has
/// them. Returns the chunks so the caller can decode their values.
[[nodiscard]] bool read_page_chunks_with_validity(const std::vector<std::uint8_t>& payload,
                                                  const ColumnEncodingPlan& plan, std::uint64_t page_rows,
                                                  std::vector<MiniBlockChunkView>& chunks,
                                                  std::uint64_t& validity_rows, ColumnValues& out,
                                                  std::string& error) {
    if (!split_miniblock_payload(payload, plan.chunk_shape, chunks, error) ||
        !apply_item_view(plan, chunks, error)) {
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

/// Which of a constant page's buffers holds its definition levels, or -1 for none.
///
/// Lance decides this from the inline value's presence and the buffer count together
/// (`ConstantPageScheduler::try_new`), and only four combinations are legal:
///
///     inline, 0 buffers  -> the value, no levels
///     inline, 2 buffers  -> the value; buffer 0 = rep, buffer 1 = def
///     no inline, 1       -> buffer 0 = the value, no levels
///     no inline, 3       -> buffer 0 = the value; buffer 1 = rep, buffer 2 = def
///
/// Anything else is malformed and refused rather than guessed at, exactly as Lance refuses it. A
/// zero-length def buffer means the layer is absent even though the slot exists.
[[nodiscard]] bool constant_def_buffer_index(bool has_inline_value, std::size_t buffer_count,
                                             std::ptrdiff_t& out_index, std::string& error) {
    if (has_inline_value && buffer_count == 0U) {
        out_index = -1;
        return true;
    }
    if (has_inline_value && buffer_count == 2U) {
        out_index = 1;
        return true;
    }
    if (!has_inline_value && buffer_count == 1U) {
        out_index = -1;
        return true;
    }
    if (!has_inline_value && buffer_count == 3U) {
        out_index = 2;
        return true;
    }
    error = "constant page has " + std::to_string(buffer_count) + " buffers with" +
            (has_inline_value ? " an" : "out an") + " inline value, which is not a layout Lance writes";
    return false;
}

/// Read a constant page's definition levels and fold them into the column's validity bitmap.
///
/// These levels are RAW u16, one per row, always. `ConstantLayout.def_compression` exists in the
/// proto but applies only to the all-null path (`ComplexAllNullScheduler`); a constant page that
/// carries a value goes through `ConstantPageScheduler`, which borrows the buffer as a u16 slice and
/// nothing else. `num_def_values` is likewise not populated on this path -- pylance leaves it 0 --
/// so the row count is what says how many levels there are.
[[nodiscard]] bool apply_constant_definition_levels(const std::filesystem::path& data_file_path,
                                                    const pb::ColumnPage& page, bool has_inline_value,
                                                    std::uint64_t rows_already_appended, ColumnValues& out,
                                                    std::string& error) {
    std::ptrdiff_t def_index = -1;
    if (!constant_def_buffer_index(has_inline_value, page.buffer_sizes.size(), def_index, error)) {
        return false;
    }
    if (def_index < 0 || page.buffer_sizes[static_cast<std::size_t>(def_index)] == 0U) {
        // The slot exists but the layer does not: every row in this page is valid. Still has to be
        // reflected, because a LATER page may carry levels and the bitmap is built across all of them.
        out.validity.resize(static_cast<std::size_t>((rows_already_appended + page.length + 7U) / 8U), 0U);
        for (std::uint64_t i = 0; i < page.length; ++i) {
            const auto row = rows_already_appended + i;
            out.validity[static_cast<std::size_t>(row >> 3U)] |= static_cast<std::uint8_t>(1U << (row & 7U));
        }
        return true;
    }
    std::uint64_t expected_bytes = 0;
    if (!checked_mul(page.length, 2U, expected_bytes) ||
        page.buffer_sizes[static_cast<std::size_t>(def_index)] != expected_bytes) {
        error = "constant page's definition buffer is " +
                std::to_string(page.buffer_sizes[static_cast<std::size_t>(def_index)]) +
                " bytes, expected " + std::to_string(expected_bytes) + " for " +
                std::to_string(page.length) + " raw 16-bit levels";
        return false;
    }
    std::vector<std::uint8_t> levels;
    if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[static_cast<std::size_t>(def_index)],
                                    page.buffer_sizes[static_cast<std::size_t>(def_index)], levels, error)) {
        return false;
    }
    // The buffer is read into a fresh vector, so its data() carries the alignment operator new gives
    // -- enough for u16 -- and the length was just checked to be exactly two bytes per row.
    const auto* typed = reinterpret_cast<const std::uint16_t*>(levels.data());
    std::uint64_t remaining = page.length;
    std::uint64_t at = 0;
    // append_levels_to_validity takes a u32 count; a page's row count is u64, so walk it in slices
    // rather than assuming it fits.
    while (remaining != 0U) {
        const auto slice = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining, std::numeric_limits<std::uint32_t>::max()));
        if (!append_levels_to_validity(typed + at, slice, rows_already_appended + at, out.validity,
                                       out.null_count)) {
            error = "constant page definition levels could not be applied";
            return false;
        }
        at += slice;
        remaining -= slice;
    }
    return true;
}

}  // namespace

namespace {

/// The whole decoder for one physical column. `item_view` is set only for a list column's second
/// pass (decode_list_column), which reads the leaf values as a flat column of items.
bool decode_column_impl(const std::filesystem::path& data_file_path, const pb::Field& on_disk_field,
                        const pb::ColumnMetadata& column_metadata, ColumnValues& out, std::string& error,
                        const std::shared_ptr<ItemView>& item_view) {
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
    auto encoding_plan = classify_column_encoding(on_disk_field, column_metadata);
    if (item_view) {
        // Item validity comes from the unravelled levels, not from each chunk's definition levels,
        // which here count list layers too. Only MiniBlock value encodings are read inside a list.
        encoding_plan.repdef = nullptr;
        encoding_plan.item_view = item_view;
        if (encoding_plan.kind == ColumnEncodingKind::kConstant || encoding_plan.kind == ColumnEncodingKind::kFullZip) {
            error = "column '" + on_disk_field.name + "': list items in a " +
                    (encoding_plan.kind == ColumnEncodingKind::kConstant ? "constant" : "FullZip") +
                    " page are not read yet";
            return false;
        }
    }
    if (encoding_plan.kind == ColumnEncodingKind::kUnsupported) {
        // Refuse by name, before a single buffer byte is interpreted. The old code had no way to do
        // this: with no descriptor read, an encoding it did not implement fell through to the flat
        // path and was misparsed, surfacing later as a size mismatch -- which is why a 3-row
        // stock-Lance table decoded and a 5000-row one did not.
        error = "column '" + on_disk_field.name + "': " + encoding_plan.unsupported_reason;
        return false;
    }

    // A constant page carries a value when the descriptor has an inline one, or when the page has 1
    // or 3 buffers (value; value + rep + def). 0 or 2 buffers with no inline value is how Lance says
    // every row is null. This is `ConstantPageScheduler::try_new`'s `has_scalar_value` inverted, and
    // it has to be read off the PAGE: a definition layer in the descriptor is equally how a nullable
    // constant column is spelled, so keying "all null" off the layers alone made every nullable
    // string constant read back entirely null. Fixed-width columns escaped it only because they
    // carry their value inline.
    const std::size_t constant_buffer_count =
        column_metadata.pages.empty() ? 0U : column_metadata.pages.front().buffer_sizes.size();
    const bool constant_has_value = encoding_plan.constant_inline_value.has_value() ||
                                    constant_buffer_count == 1U || constant_buffer_count == 3U;
    const bool constant_all_null = encoding_plan.constant_declares_levels && !constant_has_value;

    if (encoding_plan.kind == ColumnEncodingKind::kConstant && constant_all_null) {
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
        if (encoding_plan.constant_declares_levels) {
            std::uint64_t rows_so_far = 0;
            for (const auto& page : column_metadata.pages) {
                if (!apply_constant_definition_levels(data_file_path, page,
                                                      encoding_plan.constant_inline_value.has_value(), rows_so_far,
                                                      out, error)) {
                    return false;
                }
                rows_so_far += page.length;
            }
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

    // Dictionary with RLE'd indices: buffer[1] = the miniblock payload, buffer[2] = the dictionary.
    //
    // This branch used to parse the payload itself as [u16 num_levels][u32 size0][u32 size1] and take
    // the whole page to be one chunk. That is nanolance's own dict-rle page and nothing else. Lance
    // writes the ordinary miniblock grammar -- a chunk header whose shape the descriptor declares,
    // as many chunks as the page needs -- so a pylance column with runs decoded only its first 1024
    // values per chunk read and then failed downstream with a buffer-size mismatch rather than
    // anything naming the cause. A repetitive string column past ~5000 rows is exactly that shape,
    // and so is any integer column with runs, whose dictionary is fixed-width rather than variable.
    if (encoding_plan.kind == ColumnEncodingKind::kDictRle) {
        const bool fixed_dict = encoding_plan.dict_value_bits != 0U;
        out.kind = fixed_dict ? ColumnValues::Kind::FixedWidth : ColumnValues::Kind::VariableWidth;
        out.variable.large = lance_logical_type_has_large_offsets(on_disk_field.logical_type);
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> dict_stored;
        DictionaryBlock dict;
        std::vector<MiniBlockChunkView> chunks;
        std::vector<std::pair<std::uint32_t, std::uint8_t>> runs;  // (dictionary index, run length)
        std::uint64_t validity_rows = 0;
        for (const auto& page : column_metadata.pages) {
            if (page.buffer_offsets.size() < 3U || page.buffer_sizes.size() < 3U) {
                error = "dict-rle page missing buffers";
                return false;
            }
            if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[1], page.buffer_sizes[1], payload,
                                            error) ||
                !read_lance_data_file_bytes(data_file_path, page.buffer_offsets[2], page.buffer_sizes[2], dict_stored,
                                            error)) {
                return false;
            }
            if (!decode_dictionary_block(encoding_plan, on_disk_field.logical_type, dict_stored, dict, error)) {
                return false;
            }
            // Same splitter as every other miniblock path, so a nullable column's definition levels
            // are read here rather than being mistaken for run data.
            if (!read_page_chunks_with_validity(payload, encoding_plan, page.length, chunks, validity_rows, out,
                                                error)) {
                return false;
            }
            runs.clear();
            for (const auto& chunk : chunks) {
                if (chunk.extra_buffers.empty()) {
                    error = "dict-rle chunk is missing its run-lengths buffer";
                    return false;
                }
                const auto& values = chunk.values;    // one u32 dictionary index per run
                const auto& lengths = chunk.extra_buffers[0];  // one u8 run length per run
                if (values.size() % 4U != 0U || values.size() / 4U != lengths.size()) {
                    error = "dict-rle run count mismatch between indices and lengths";
                    return false;
                }
                const std::size_t num_runs = lengths.size();
                reserve_more(runs, num_runs);
                for (std::size_t r = 0; r < num_runs; ++r) {
                    std::uint32_t index = 0;
                    std::memcpy(&index, values.data() + r * 4U, 4U);
                    if (index >= dict.count) {
                        error = "dict-rle index out of range";
                        return false;
                    }
                    runs.emplace_back(index, lengths[r]);
                }
            }

            // Pre-pass over the whole page: how many rows, and how many bytes they expand to.
            std::size_t total_rows = 0;
            std::size_t total_data = 0;
            for (const auto& [index, run] : runs) {
                total_rows += run;
                total_data += static_cast<std::size_t>(run) * dict.entry_size(index);
            }
            if (total_rows != page.length) {
                error = "dict-rle runs cover " + std::to_string(total_rows) + " rows but the page declares " +
                        std::to_string(page.length);
                return false;
            }

            if (fixed_dict) {
                const std::size_t base = out.fixed.size();
                out.fixed.resize(base + total_data);
                std::uint8_t* dest = out.fixed.data() + base;
                for (const auto& [index, run] : runs) {
                    const std::uint8_t* src = dict.entry(index);
                    for (std::uint8_t c = 0; c < run; ++c) {
                        std::memcpy(dest, src, dict.value_bytes);
                        dest += dict.value_bytes;
                    }
                }
                continue;
            }

            reserve_more(out.variable.data, total_data);
            const bool first_page = out.variable.offsets.empty();
            std::uint64_t cumulative = out.variable.data.size();  // byte offset (continues across pages)

            // Expand: bulk-fill data once per run; collect offsets in a typed temp, then one bulk copy.
            auto expand = [&](auto& offs) -> bool {
                offs.reserve(total_rows + (first_page ? 1U : 0U));
                using OT = typename std::decay_t<decltype(offs)>::value_type;
                if (first_page) {
                    offs.push_back(static_cast<OT>(cumulative));
                }
                for (const auto& [index, run] : runs) {
                    const std::size_t len = dict.entry_size(index);
                    if (!append_repeated_value(out.variable.data, dict.entry(index), len, run)) {
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
        out.kind = fixed_dict ? ColumnValues::Kind::FixedWidth : ColumnValues::Kind::VariableWidth;
        out.variable.large = lance_logical_type_has_large_offsets(on_disk_field.logical_type);
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> dict_stored;
        DictionaryBlock dict;
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
            if (!decode_dictionary_block(encoding_plan, on_disk_field.logical_type, dict_stored, dict, error)) {
                return false;
            }
            const std::size_t num_dict = dict.count;
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
                const auto count = chunk.items != 0U ? chunk.items
                                   : encoding_plan.repdef != nullptr
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
                out.fixed.resize(base + static_cast<std::size_t>(page.length) * dict.value_bytes);
                std::uint8_t* dest = out.fixed.data() + base;
                for (std::uint64_t r = 0; r < page.length; ++r) {
                    std::uint32_t index = 0;
                    std::memcpy(&index, indices_bytes.data() + r * 4U, 4U);
                    if (index >= num_dict) {
                        error = "dict index out of range";
                        return false;
                    }
                    std::memcpy(dest, dict.entry(index), dict.value_bytes);
                    dest += dict.value_bytes;
                }
                continue;
            }
            const bool first_page = out.variable.offsets.empty();
            std::uint64_t cumulative = out.variable.data.size();
            // Same reasoning as expand_fsst_values(): a row's length comes from the dictionary entry
            // its index selects, so the offsets are built per row -- but only grown once per page.
            reserve_more(out.variable.offsets,
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
                const std::size_t len = dict.entry_size(index);
                const std::uint8_t* src = dict.entry(index);
                out.variable.data.insert(out.variable.data.end(), src, src + len);
                cumulative += len;
                append_list_offset(out.variable.offsets, static_cast<std::int64_t>(cumulative), out.variable.large);
            }
        }
        return true;
    }

    // FullZip: values stored row by row, each behind a control word. What Lance writes when a page's
    // longest value is 256 bytes or more -- a long string, or a float32 fixed_size_list of 64+ dims.
    if (encoding_plan.kind == ColumnEncodingKind::kFullZip) {
        std::vector<std::uint8_t> data;
        std::vector<std::uint16_t> levels;
        std::uint64_t rows_so_far = 0;
        bool variable = false;
        for (std::size_t page_index = 0; page_index < column_metadata.pages.size(); ++page_index) {
            const auto& page = column_metadata.pages[page_index];
            page_layout::PageLayout layout;
            FullZipPageParams params;
            std::string why;
            if (!page_layout::decode_page_layout(page.encoding, layout, why) ||
                !full_zip_page_params(layout, page.length, params, why)) {
                error = "column '" + on_disk_field.name + "' page " + std::to_string(page_index) + ": " + why;
                return false;
            }
            if (page_index == 0U) {
                variable = params.length_bytes != 0U;
                if (variable) {
                    out.kind = ColumnValues::Kind::VariableWidth;
                    out.variable.large = lance_logical_type_has_large_offsets(on_disk_field.logical_type);
                    append_list_offset(out.variable.offsets, 0, out.variable.large);
                } else {
                    out.kind = ColumnValues::Kind::FixedWidth;
                    const auto schema_bytes = lance_logical_type_value_bytes(on_disk_field.logical_type);
                    if (schema_bytes != params.value_bytes - params.item_validity_bytes) {
                        error = "column '" + on_disk_field.name + "': FullZip values are " +
                                std::to_string(params.value_bytes) + " bytes but the type is " +
                                std::to_string(schema_bytes);
                        return false;
                    }
                }
            } else if (variable != (params.length_bytes != 0U)) {
                error = "column '" + on_disk_field.name + "' mixes fixed- and variable-width FullZip pages";
                return false;
            }
            if (page.buffer_offsets.empty() || page.buffer_sizes.empty()) {
                error = "column '" + on_disk_field.name + "': FullZip page has no data buffer";
                return false;
            }
            if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[0], page.buffer_sizes[0], data, error)) {
                return false;
            }
            if (page.length > std::numeric_limits<std::uint32_t>::max()) {
                error = "FullZip page row count exceeds 2^32";
                return false;
            }
            const auto rows = static_cast<std::size_t>(page.length);
            levels.assign(rows, 0U);
            const std::uint32_t def_mask = params.bits_def == 0U ? 0U : ((1U << params.bits_def) - 1U);
            bool page_has_nulls = false;
            std::size_t at = 0;
            if (!variable) {
                std::uint64_t page_bytes = 0;
                if (!checked_mul(static_cast<std::uint64_t>(rows),
                                 static_cast<std::uint64_t>(params.value_bytes + params.control_bytes), page_bytes) ||
                    page_bytes != data.size()) {
                    error = "column '" + on_disk_field.name + "': FullZip page is " + std::to_string(data.size()) +
                            " bytes, expected " + std::to_string(page_bytes);
                    return false;
                }
                reserve_more(out.fixed, rows * (params.value_bytes - params.item_validity_bytes));
                if (params.items != 0U) {
                    out.items_per_row = params.items;
                }
            }
            for (std::size_t r = 0; r < rows; ++r) {
                std::uint32_t def = 0;
                if (params.control_bytes != 0U) {
                    if (data.size() - at < params.control_bytes) {
                        error = "column '" + on_disk_field.name + "': FullZip page truncated in a control word";
                        return false;
                    }
                    std::uint32_t word = 0;
                    std::memcpy(&word, data.data() + at, params.control_bytes);  // little-endian
                    at += params.control_bytes;
                    def = word & def_mask;
                    levels[r] = static_cast<std::uint16_t>(def);
                    page_has_nulls = page_has_nulls || def != 0U;
                }
                if (!variable) {
                    const auto first_item = (rows_so_far + r) * params.items;
                    if (params.item_validity_bytes != 0U) {
                        append_item_validity(out, data.data() + at, 0U, params.items, first_item);
                    } else if (params.items != 0U) {
                        append_item_validity(out, nullptr, 0U, params.items, first_item);
                    }
                    at += params.item_validity_bytes;
                    const auto item_bytes = params.value_bytes - params.item_validity_bytes;
                    out.fixed.insert(out.fixed.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                                     data.begin() + static_cast<std::ptrdiff_t>(at + item_bytes));
                    at += item_bytes;
                    continue;
                }
                if (def == 0U) {
                    if (data.size() - at < params.length_bytes) {
                        error = "column '" + on_disk_field.name + "': FullZip page truncated in a length";
                        return false;
                    }
                    std::uint64_t length = 0;
                    std::memcpy(&length, data.data() + at, params.length_bytes);
                    at += params.length_bytes;
                    if (length > data.size() - at) {
                        error = "column '" + on_disk_field.name + "': FullZip value runs past the page";
                        return false;
                    }
                    if (params.fsst) {
                        if (!fsst::decompress_value(*params.fsst, data.data() + at, static_cast<std::size_t>(length),
                                                    out.variable.data, error)) {
                            return false;
                        }
                    } else {
                        out.variable.data.insert(out.variable.data.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                                                 data.begin() + static_cast<std::ptrdiff_t>(at + length));
                    }
                    at += static_cast<std::size_t>(length);
                    if (out.variable.data.size() > default_read_limits().max_uncompressed_bytes ||
                        (!out.variable.large &&
                         out.variable.data.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))) {
                        error = "column '" + on_disk_field.name + "' exceeds the decoded-size limit";
                        return false;
                    }
                }
                append_list_offset(out.variable.offsets, static_cast<std::int64_t>(out.variable.data.size()),
                                   out.variable.large);
            }
            if (at != data.size()) {
                error = "column '" + on_disk_field.name + "': FullZip page has " + std::to_string(data.size() - at) +
                        " bytes left over after its " + std::to_string(rows) + " rows";
                return false;
            }
            // The validity bitmap stays empty until the first null, then covers every row so far.
            if (page_has_nulls && out.validity.empty() && rows_so_far != 0U) {
                std::vector<std::uint16_t> earlier(static_cast<std::size_t>(rows_so_far), 0U);
                append_levels_to_validity(earlier.data(), static_cast<std::uint32_t>(rows_so_far), 0U, out.validity,
                                          out.null_count);
            }
            if (page_has_nulls || !out.validity.empty()) {
                append_levels_to_validity(levels.data(), static_cast<std::uint32_t>(rows), rows_so_far, out.validity,
                                          out.null_count);
            }
            rows_so_far += page.length;
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
                    chunk.items != 0U ? chunk.items
                    : encoding_plan.repdef != nullptr
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
                if (chunk.items != 0U) {
                    chunk_values = chunk.items;
                } else if (nullable) {
                    chunk_values = chunk.repdef_values;
                } else {
                    if (chunk.values.size() < offset_width) {
                        error = "variable-width chunk is too short to hold an offset table";
                        return false;
                    }
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
    std::uint64_t page_rows_before = 0;
    for (const auto& page : column_metadata.pages) {
        if (!read_page_buffers(data_file_path, page, false, control, payload, error)) {
            return false;
        }
        if (!split_miniblock_payload(payload, encoding_plan.chunk_shape, chunks, error) ||
            !apply_item_view(encoding_plan, chunks, error)) {
            return false;
        }
        // How many values a chunk holds depends on how it is encoded, and the header only states it
        // when the chunk carries definition levels:
        //   * with definition levels, the header's count is authoritative;
        //   * bit-packed chunks are one FastLanes block each (1024 values), the last one short;
        //   * flat chunks are sized by bytes -- the writer fills them to a byte budget, not to a
        //     value count, so a flat int64 chunk holds whatever fits.
        std::uint64_t remaining = page.length;
        for (auto& chunk : chunks) {
            if (encoding_plan.fsl_item_validity) {
                // [element validity bits][values]: move the values into place and keep the bits.
                if (chunk.extra_buffers.size() != 1U) {
                    error = "fixed_size_list chunk with element validity does not carry two buffers";
                    return false;
                }
                std::swap(chunk.values, chunk.extra_buffers[0]);
            }
            std::uint32_t chunk_values = 0;
            if (chunk.items != 0U) {
                chunk_values = static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk.items, UINT32_MAX));
            } else if (nullable) {
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
            if (encoding_plan.fsl_items != 0U) {
                out.items_per_row = encoding_plan.fsl_items;
                const auto first_item = (page_rows_before + page.length - remaining) * encoding_plan.fsl_items;
                const auto items = static_cast<std::uint64_t>(chunk_values) * encoding_plan.fsl_items;
                if (encoding_plan.fsl_item_validity) {
                    const auto& bits = chunk.extra_buffers[0];
                    if (bits.size() != static_cast<std::size_t>((items + 7U) / 8U)) {
                        error = "fixed_size_list element validity is " + std::to_string(bits.size()) +
                                " bytes for " + std::to_string(items) + " elements";
                        return false;
                    }
                    append_item_validity(out, bits.data(), 0U, items, first_item);
                } else {
                    append_item_validity(out, nullptr, 0U, items, first_item);
                }
            }
            remaining -= chunk_values;
        }
        if (remaining != 0U) {
            error = "miniblock page chunks cover " + std::to_string(page.length - remaining) +
                    " of " + std::to_string(page.length) + " rows";
            return false;
        }
        page_rows_before += page.length;
    }
    if (nullable && validity_rows != declared_rows) {
        error = "definition levels cover " + std::to_string(validity_rows) + " rows but the column has " +
                std::to_string(declared_rows);
        return false;
    }
    return true;
}


/// Value counts per chunk, from a MiniBlock page's metadata buffer: one word per chunk (u32 when
/// `has_large_chunk`, else u16), `log_num_values` in its low 4 bits. Every chunk but the last holds
/// `2^log` values; the last holds what is left of `num_items`.
bool chunk_items_from_metadata(const std::vector<std::uint8_t>& control, bool large_words, std::uint64_t num_items,
                               std::vector<std::uint64_t>& out, std::string& error) {
    out.clear();
    const std::size_t word = large_words ? 4U : 2U;
    if (control.empty() || control.size() % word != 0U) {
        error = "list page metadata is " + std::to_string(control.size()) + " bytes, not whole words";
        return false;
    }
    const std::size_t chunks = control.size() / word;
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i + 1U < chunks; ++i) {
        const auto log = control[i * word] & 0x0FU;
        if (log == 0U) {
            error = "list page chunk " + std::to_string(i) + " is not the last but declares no values";
            return false;
        }
        const std::uint64_t n = std::uint64_t{1} << log;
        if (n > num_items - sum) {
            error = "list page chunks declare more values than the page's " + std::to_string(num_items);
            return false;
        }
        sum += n;
        out.push_back(n);
    }
    if (num_items - sum == 0U && num_items != 0U) {
        error = "list page's last chunk has no values";
        return false;
    }
    out.push_back(num_items - sum);
    return true;
}

/// Append `count` bits of `src` (empty = all valid) to `dst`, which already holds `at` bits.
void append_validity_bits(std::vector<std::uint8_t>& dst, std::uint64_t& dst_nulls, std::uint64_t at,
                          const std::vector<std::uint8_t>& src, std::uint64_t src_nulls, std::uint64_t count) {
    if (src.empty() && dst.empty()) {
        return;  // still all valid
    }
    if (dst.empty()) {
        dst.assign(static_cast<std::size_t>((at + 7U) / 8U), 0U);
        for (std::uint64_t i = 0; i < at; ++i) {
            dst[static_cast<std::size_t>(i >> 3U)] |= static_cast<std::uint8_t>(1U << (i & 7U));
        }
    }
    dst.resize(static_cast<std::size_t>((at + count + 7U) / 8U), 0U);
    for (std::uint64_t i = 0; i < count; ++i) {
        const bool valid = src.empty() || ((src[static_cast<std::size_t>(i >> 3U)] >> (i & 7U)) & 1U) != 0U;
        if (valid) {
            const auto d = at + i;
            dst[static_cast<std::size_t>(d >> 3U)] |= static_cast<std::uint8_t>(1U << (d & 7U));
        }
    }
    dst_nulls += src_nulls;
}

/// Append one page's decoded leaf values to the column's. Validity is not touched: for a list it
/// comes from the unravelled levels.
bool append_leaf_values(ColumnValues& dst, ColumnValues& src, std::string& error) {
    if (dst.kind == ColumnValues::Kind::FixedWidth) {
        if (src.kind != ColumnValues::Kind::FixedWidth) {
            error = "list pages decoded to different value kinds";
            return false;
        }
        dst.fixed.insert(dst.fixed.end(), src.fixed.begin(), src.fixed.end());
        return true;
    }
    if (src.kind != ColumnValues::Kind::VariableWidth || src.variable.large != dst.variable.large) {
        error = "list pages decoded to different value kinds";
        return false;
    }
    if (src.variable.offsets.empty()) {
        return true;  // a page with no items
    }
    const auto width = static_cast<std::size_t>(src.variable.large ? 8U : 4U);
    const auto base = read_list_offset(dst.variable.offsets, dst.variable.offsets.size() / width - 1U, dst.variable.large);
    const auto first = read_list_offset(src.variable.offsets, 0, src.variable.large);
    for (std::size_t i = 1; i < src.variable.offsets.size() / width; ++i) {
        append_list_offset(dst.variable.offsets, base + read_list_offset(src.variable.offsets, i, src.variable.large) - first,
                           dst.variable.large);
    }
    dst.variable.data.insert(dst.variable.data.end(), src.variable.data.begin(), src.variable.data.end());
    return true;
}

/// Decode `count` levels stored as raw u16 (no compression declared) or through `encoding`.
bool decode_level_buffer(const std::vector<std::uint8_t>& bytes, const page_layout::Compressive* encoding,
                         std::uint64_t count, std::vector<std::uint16_t>& out, std::string& error) {
    if (count > std::numeric_limits<std::uint32_t>::max() || count > bytes.size() * 8U * 4096U + 65536U) {
        error = "a page declares an implausible " + std::to_string(count) + " levels";
        return false;
    }
    if (encoding == nullptr && count == 0U) {
        // Raw u16 levels: pylance leaves the count out (proto3's 0) and the buffer size says it.
        count = bytes.size() / 2U;
    }
    out.resize(static_cast<std::size_t>(count));
    if (encoding == nullptr) {
        if (bytes.size() != count * 2U) {
            error = "raw levels are " + std::to_string(bytes.size()) + " bytes for " + std::to_string(count);
            return false;
        }
        if (!bytes.empty()) {  // an empty vector's data() may be null, and memcpy from null is UB
            std::memcpy(out.data(), bytes.data(), bytes.size());
        }
        return true;
    }
    return decode_levels(bytes, *encoding, static_cast<std::uint32_t>(count), out.data(), error);
}

/// A list column: one leaf's values under one or more list layers (docs/NESTED_COLUMNS.md).
///
/// Page by page, two steps. First the page's repetition and definition levels are unravelled into
/// per-layer offsets and validity (src/repdef.cpp). Then its values are read through the ordinary
/// decoders as a flat page of ITEMS -- the same page, declaring its item count instead of its row
/// count, with each chunk's value count taken from the page's metadata words -- so every value
/// encoding (bit-packing, dictionaries, FSST, RLE) works inside a list without a second copy. A
/// constant page (all lists empty or null, or one repeated item) carries its levels as buffers.
bool decode_list_column(const std::filesystem::path& data_file_path, const pb::Field& on_disk_field,
                        const pb::ColumnMetadata& column_metadata, ColumnValues& out, std::string& error) {
    const auto& logical_type = on_disk_field.logical_type;
    const bool variable = lance_field_is_variable_width(logical_type);
    if (variable) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = lance_logical_type_has_large_offsets(logical_type);
        append_list_offset(out.variable.offsets, 0, out.variable.large);
    } else {
        out.kind = ColumnValues::Kind::FixedWidth;
    }
    const std::size_t value_bytes = variable ? 0U : lance_logical_type_value_bytes(logical_type);
    if (!variable && value_bytes == 0U) {
        error = "column '" + on_disk_field.name + "': list items of type " + logical_type + " are not read yet";
        return false;
    }

    std::vector<ColumnValues::NestedLayer> layers;  // outermost first
    std::vector<std::uint8_t> item_validity;
    std::uint64_t item_nulls = 0;
    std::uint64_t items_total = 0;
    std::vector<bool> shape;  // per layer, innermost first: is it a list? Must agree across pages.

    std::vector<std::uint8_t> control;
    std::vector<std::uint8_t> payload;
    std::vector<MiniBlockChunkView> chunks;
    std::vector<std::uint16_t> rep;
    std::vector<std::uint16_t> def;
    for (std::size_t page_index = 0; page_index < column_metadata.pages.size(); ++page_index) {
        const auto& page = column_metadata.pages[page_index];
        const auto where = "column '" + on_disk_field.name + "' page " + std::to_string(page_index) + ": ";
        page_layout::PageLayout layout;
        std::string why;
        if (!page_layout::decode_page_layout(page.encoding, layout, why)) {
            error = where + why;
            return false;
        }
        rep.clear();
        def.clear();
        bool has_def = false;
        std::uint64_t num_items = repdef::kInferItems;
        const std::vector<std::uint8_t>* layer_kinds = nullptr;
        std::vector<std::uint64_t> items;

        if (layout.kind == page_layout::LayoutKind::kMiniBlock && layout.mini_block.has_repetition &&
            layout.mini_block.rep_compression != nullptr) {
            const auto& mb = layout.mini_block;
            MiniBlockChunkShape chunk_shape;
            chunk_shape.has_repetition = true;
            chunk_shape.has_definition = mb.repdef_compression != nullptr;
            chunk_shape.large_buffer_sizes = mb.has_large_chunk;
            chunk_shape.num_buffers = mb.num_buffers != 0U ? mb.num_buffers : 2U;
            if (!read_page_buffers(data_file_path, page, false, control, payload, error) ||
                !split_miniblock_payload(payload, chunk_shape, chunks, error) ||
                !chunk_items_from_metadata(control, mb.has_large_chunk, mb.num_items, items, error)) {
                error = where + error;
                return false;
            }
            if (items.size() != chunks.size()) {
                error = where + std::to_string(chunks.size()) + " chunks but " + std::to_string(items.size()) +
                        " metadata words";
                return false;
            }
            for (const auto& chunk : chunks) {
                const auto n = chunk.num_levels;
                if (n == 0U) {
                    continue;
                }
                if (chunk.rep.empty() || (chunk_shape.has_definition && chunk.repdef.empty())) {
                    error = where + "a chunk declares levels but carries no level buffer";
                    return false;
                }
                rep.resize(rep.size() + n);
                if (!decode_levels(chunk.rep, *mb.rep_compression, n, rep.data() + rep.size() - n, error)) {
                    error = where + "repetition levels: " + error;
                    return false;
                }
                if (chunk_shape.has_definition) {
                    def.resize(def.size() + n);
                    if (!decode_levels(chunk.repdef, *mb.repdef_compression, n, def.data() + def.size() - n, error)) {
                        error = where + "definition levels: " + error;
                        return false;
                    }
                }
            }
            has_def = chunk_shape.has_definition;
            num_items = mb.num_items;
            layer_kinds = &mb.layers;
        } else if (layout.kind == page_layout::LayoutKind::kConstant) {
            // Buffers: [rep, def] after the value when the value is not inline; no value at all when
            // every item is null or there are none.
            const auto& c = layout.constant;
            const std::size_t buffers = page.buffer_offsets.size();
            if (page.buffer_sizes.size() != buffers || (buffers != 2U && buffers != 3U) ||
                (c.inline_value.has_value() && buffers != 2U)) {
                error = where + "a constant list page has " + std::to_string(buffers) + " buffers";
                return false;
            }
            const std::size_t rep_at = buffers == 3U ? 1U : 0U;
            if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[rep_at], page.buffer_sizes[rep_at],
                                            control, error) ||
                !read_lance_data_file_bytes(data_file_path, page.buffer_offsets[rep_at + 1U],
                                            page.buffer_sizes[rep_at + 1U], payload, error) ||
                !decode_level_buffer(control, c.rep_compression.get(), c.num_rep_values, rep, error) ||
                !decode_level_buffer(payload, c.def_compression.get(), c.num_def_values, def, error)) {
                error = where + error;
                return false;
            }
            has_def = true;
            layer_kinds = &c.layers;
        } else {
            error = where + "a list column page must be MiniBlock or Constant with repetition levels, not " +
                    page_layout::describe(layout);
            return false;
        }

        std::vector<repdef::UnraveledLayer> unraveled;
        if (!repdef::unravel(rep, true, def, has_def, *layer_kinds, num_items, unraveled, error)) {
            error = where + error;
            return false;
        }
        if (unraveled.back().length != page.length) {
            error = where + "levels describe " + std::to_string(unraveled.back().length) + " rows, the page " +
                    std::to_string(page.length);
            return false;
        }
        const auto page_items = unraveled[0].length;
        std::vector<bool> page_shape;
        for (const auto& layer : unraveled) {
            page_shape.push_back(repdef::is_list_layer(layer.kind));
        }
        if (page_index == 0U) {
            shape = page_shape;
            for (std::size_t k = 1; k < shape.size(); ++k) {
                if (!shape[k]) {
                    const bool above_every_list =
                        std::find(shape.begin() + static_cast<std::ptrdiff_t>(k), shape.end(), true) == shape.end();
                    error = where + (above_every_list ? "a list inside a struct is not read yet"
                                                      : "a struct inside a list is not read yet");
                    return false;
                }
            }
            layers.resize(shape.size() - 1U);
            for (auto& layer : layers) {
                layer.offsets.push_back(0);
            }
        } else if (page_shape != shape) {
            error = where + "its list layers differ from the column's first page";
            return false;
        }

        // The page's items.
        ColumnValues page_values;
        if (layout.kind == page_layout::LayoutKind::kMiniBlock) {
            pb::ColumnMetadata one_page;
            one_page.pages.push_back(page);
            one_page.pages.back().length = page_items;
            auto view = std::make_shared<ItemView>();
            view->page_chunk_items.push_back(std::move(items));
            if (!decode_column_impl(data_file_path, on_disk_field, one_page, page_values, error, view)) {
                return false;
            }
        } else if (page_items != 0U) {
            // One value repeated for every item -- or, with no value stored, every item null.
            const auto& c = layout.constant;
            std::vector<std::uint8_t> value;
            if (page.buffer_offsets.size() == 3U) {
                if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[0], page.buffer_sizes[0], control,
                                                error)) {
                    return false;
                }
                if (variable) {
                    if (!decode_scalar_variable_value(control, value, error)) {
                        error = where + error;
                        return false;
                    }
                } else {
                    value = control;
                }
            } else if (c.inline_value.has_value()) {
                value = *c.inline_value;
            } else if (unraveled[0].null_count != page_items) {
                error = where + "a constant list page stores no value but has valid items";
                return false;
            } else if (!variable) {
                value.assign(value_bytes, 0U);
            }
            if (!variable && value.size() != value_bytes) {
                error = where + "constant item is " + std::to_string(value.size()) + " bytes, the type " +
                        std::to_string(value_bytes);
                return false;
            }
            std::uint64_t total = 0;
            if (!checked_mul(page_items, static_cast<std::uint64_t>(std::max<std::size_t>(value.size(), 1U)), total) ||
                total > default_read_limits().max_uncompressed_bytes) {
                error = where + "constant list page expands past the decoded-size limit";
                return false;
            }
            page_values.kind = out.kind;
            page_values.variable.large = out.variable.large;
            if (variable) {
                append_list_offset(page_values.variable.offsets, 0, out.variable.large);
            }
            for (std::uint64_t i = 0; i < page_items; ++i) {
                if (variable) {
                    page_values.variable.data.insert(page_values.variable.data.end(), value.begin(), value.end());
                    append_list_offset(page_values.variable.offsets,
                                       static_cast<std::int64_t>(page_values.variable.data.size()), out.variable.large);
                } else {
                    page_values.fixed.insert(page_values.fixed.end(), value.begin(), value.end());
                }
            }
        } else {
            page_values.kind = out.kind;
            page_values.variable.large = out.variable.large;
        }
        if (!append_leaf_values(out, page_values, error)) {
            error = where + error;
            return false;
        }

        // The page's layers: layer k (innermost first, k >= 1) lands in layers[size - k].
        append_validity_bits(item_validity, item_nulls, items_total, unraveled[0].validity, unraveled[0].null_count,
                             page_items);
        items_total += page_items;
        for (std::size_t k = 1; k < unraveled.size(); ++k) {
            auto& dst = layers[layers.size() - k];
            const auto& src = unraveled[k];
            const auto base = dst.offsets.back();
            for (std::size_t i = 1; i < src.offsets.size(); ++i) {
                dst.offsets.push_back(base + src.offsets[i]);
            }
            append_validity_bits(dst.validity, dst.null_count, dst.length, src.validity, src.null_count, src.length);
            dst.length += src.length;
        }
    }
    if (column_metadata.pages.empty()) {
        error = "column '" + on_disk_field.name + "' has no pages";
        return false;
    }
    if (!variable && out.fixed.size() != items_total * value_bytes) {
        error = "column '" + on_disk_field.name + "' decoded " + std::to_string(out.fixed.size()) +
                " bytes for " + std::to_string(items_total) + " items";
        return false;
    }
    out.validity = std::move(item_validity);
    out.null_count = item_nulls;
    out.layers = std::move(layers);
    return true;
}

/// Does this column hold list items? Read off the first page's descriptor: repetition levels.
bool column_has_repetition(const pb::ColumnMetadata& column_metadata) {
    if (column_metadata.pages.empty() || column_metadata.pages.front().encoding.empty()) {
        return false;
    }
    page_layout::PageLayout layout;
    std::string ignored;
    if (!page_layout::decode_page_layout(column_metadata.pages.front().encoding, layout, ignored)) {
        return false;  // the ordinary path refuses it by name
    }
    if (layout.kind == page_layout::LayoutKind::kConstant) {
        return std::any_of(layout.constant.layers.begin(), layout.constant.layers.end(),
                           [](std::uint8_t kind) { return repdef::is_list_layer(kind); });
    }
    return (layout.kind == page_layout::LayoutKind::kMiniBlock && layout.mini_block.has_repetition) ||
           (layout.kind == page_layout::LayoutKind::kFullZip && layout.full_zip.bits_rep != 0U);
}

}  // namespace

bool decode_lance_physical_column(const std::filesystem::path& data_file_path, const pb::Field& on_disk_field,
                                  const pb::ColumnMetadata& column_metadata, ColumnValues& out, std::string& error) {
    if (column_has_repetition(column_metadata)) {
        error.clear();
        out = ColumnValues{};
        return decode_list_column(data_file_path, on_disk_field, column_metadata, out, error);
    }
    return decode_column_impl(data_file_path, on_disk_field, column_metadata, out, error, nullptr);
}

}  // namespace nano_lance
