// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// FSST decompression, for reading string and binary columns written by the Rust `lance` crate.
//
// WHY THIS EXISTS. Stock Lance compresses variable-width columns with FSST (Fast Static Symbol
// Table, Boncz/Neumann/Leis 2020) and names it as `CompressiveEncoding` field 6, wrapping the real
// value encoding inside. Until this existed, every string column from pylance -- with or without
// nulls, compressed or not -- was refused by nanolance's reader with "unsupported encoding
// ... (CompressiveEncoding variant 6)", which made `utf8` the last common type a stock-Lance file
// could carry that nanolance could not read.
//
// COMPRESSION (roadmap F2) came later, for high-cardinality strings -- the one shape where
// pylance's files were still much smaller than nanolance's (1.75x on unique short strings). The
// symbol table is built the way Lance builds it (`build_symbol_table` in `rust/compression/fsst`,
// after the paper): six rounds over a ~16 KiB sample, each compressing the sample with the current
// table, counting how often every symbol and every adjacent pair occurs, and keeping the 255
// candidates with the highest gain (count x length, single bytes x8); the table from the round
// that compressed the sample best wins. Three deliberate differences, none visible to a decoder:
// the sample is drawn with a fixed seed, so the same input always writes the same file; a
// candidate is identified by its bytes alone, so the same string reached two ways is one candidate
// with the summed gain rather than two; and matching never reads past the end of a value, where
// Lance loads whole 8-byte words and relies on a sentinel.
//
// THE FORMAT is a byte-for-byte match of `rust/compression/fsst` in lancedb/lance, confirmed against
// a real pylance 12.0.0 file: a fixed 2312-byte symbol table of
// [u64 header][n_symbols * u64 symbol][n_symbols * u8 length], header =
// magic("FSST" in its top 32 bits) | encoder_switch << 24 | suffix_lim << 16 | terminator << 8 |
// n_symbols. Compressed bytes are a stream of codes: 255 escapes the next byte as a literal,
// anything else indexes the symbol table and emits that symbol's 1..8 bytes.
//
// `encoder_switch = 0` means the encoder declined to compress (Lance skips FSST below 32 KiB of
// input) and the "compressed" bytes are the originals verbatim. That is the common case for small
// files, so it is not an edge case -- it is most of them.
//
// ENDIANNESS. The header and symbols are serialized with Rust's `to_ne_bytes`, i.e. NATIVE endian,
// so a file written on a big-endian machine would not be portable to a little-endian one in the
// first place. This reader assumes little-endian, matching every other integer it reads off disk.
//
// TRUST. The symbol table, the codes and the offsets all come from an untrusted file. Every declared
// length is validated to be 1..=8 and every code to be below `symbol_count` before it is used, and
// an escape at the end of a value is rejected rather than reading past it. Unlike the Rust decoder,
// which leaves undeclared codes mapped to a sentinel that expands to nothing, this one refuses them:
// a valid encoder never emits one, and silently decoding to a shorter string is exactly the class of
// bug this reader exists to catch.

#pragma once

#include <array>
#include <cstring>

// The codec itself is nanom's (nanom/fsst.hpp, nanom/fsst_encode.hpp, namespace nanom::codec::fsst):
// the table format, the decoder and the encoder described above. This header keeps nanolance's
// calling convention -- byte vectors and an error string -- over it.
#include <nanom/fsst.hpp>
#include <nanom/fsst_encode.hpp>

#include "nanolance/fsst_kernel.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nano_lance::fsst {

using ::nanom::codec::fsst::kEscape;
using ::nanom::codec::fsst::kMaxSymbolLength;
using ::nanom::codec::fsst::kSymbolTableBytes;

using SymbolTable = ::nanom::codec::fsst::symbol_table;
/// A trained encoder: nanom's table plus, when the build and the table allow it, the AVX-512 kernel's copy of the
/// same table (built by train(), shared by copies). `batch` is null otherwise, and the batch compressors use
/// the portable path.
struct Encoder : ::nanom::codec::fsst::encoder {
    std::shared_ptr<const BatchTable> batch;
};

/// Parse the `Fsst.symbol_table` bytes from the page descriptor. Returns false with `error` set if
/// the buffer is not exactly `kSymbolTableBytes` long, carries the wrong magic, or declares a symbol
/// length outside 1..=8; `out` is then left untouched.
bool parse_symbol_table(const std::vector<std::uint8_t>& bytes, SymbolTable& out, std::string& error);

/// Append the decompression of `[data, data + size)` -- one value -- to `out`. `table.passthrough`
/// copies the bytes through unchanged. Returns false with `error` set on an unknown code or an
/// escape with no payload byte left in the value.
bool decompress_value(const SymbolTable& table, const std::uint8_t* data, std::size_t size,
                      std::vector<std::uint8_t>& out, std::string& error);

/// `decompress_value` into raw memory, for decoding a whole chunk into one scratch buffer: checks
/// every code as decompress_value does and advances `dst` past the decoded bytes. The caller
/// guarantees `size * kMaxSymbolLength + kMaxSymbolLength` writable bytes at `dst` -- the worst
/// case, since symbols are copied as whole 8-byte words.
bool decode_checked(const SymbolTable& table, const std::uint8_t* data, std::size_t size, std::uint8_t*& dst,
                    std::string& error);

/// Train a table on `values` (a sample of them, if they are large). Returns false when no symbol is
/// worth having -- empty input, say -- and the caller should store the values plain.
bool train(const std::vector<std::pair<const std::uint8_t*, std::size_t>>& values, Encoder& out);

/// Builds the kernel's copy of `encoder`'s table (train() does this; tests call it on tables they build by hand).
/// Without the kernel in this build (see kernel_built) it attaches nothing.
void attach_batch_table(Encoder& encoder);

/// Whether this build has the AVX-512 kernel (GCC/Clang on x86-64, not WebAssembly), whatever the CPU.
bool kernel_built();

/// Compresses `count` values with the AVX-512 kernel when this build has it, this CPU runs it and the encoder has a
/// kernel table; otherwise with compress_many_portable. The output is the same either way: each value's bytes are
/// what compress_into writes for it.
void compress_many(const Encoder& encoder, Item* items, std::size_t count);

/// The kernel alone: false (nothing written) when it is unavailable here.
bool compress_many_avx512(const Encoder& encoder, Item* items, std::size_t count);

/// The portable compressors (compress_into_padded4 where the items allow it, else compress_into_padded or compress_into).
void compress_many_portable(const Encoder& encoder, Item* items, std::size_t count);

/// Append the compression of one value to `out`: at each position the longest symbol that matches,
/// or an escape and the literal byte.
void compress_value(const Encoder& encoder, const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);

/// `compress_value` into raw memory: writes at most 2 * size bytes at `dst` (every byte escaped)
/// and returns how many it wrote.
inline std::size_t compress_into(const Encoder& encoder, const std::uint8_t* data, std::size_t size,
                                 std::uint8_t* dst) {
    return ::nanom::codec::fsst::compress_unchecked(
        encoder, std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size),
        reinterpret_cast<std::byte*>(dst));
}

namespace detail {

/// One value's greedy parse in progress (see compress_into_padded).
struct PaddedLane {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos;
    std::uint8_t* out;
};

/// Advance one position of a padded parse: the longest long symbol that matches, else the two-byte or
/// one-byte entry, else an escape. Branch-free on the match: whether a long symbol matches is close to a
/// coin flip on text, so the choice is a select, not a mispredicted branch. Every position writes its code and
/// literal byte (the literal is kept only for an escape), so the bytes are the same as a branching loop's.
inline void padded_step(const Encoder& encoder, PaddedLane& lane) {
    namespace f = ::nanom::codec::fsst;
    // The low `8 * n` bits of a word, by symbol length n (0 is never a match: an empty slot).
    static constexpr auto kLowBytes = [] {
        std::array<std::uint64_t, 9> masks{};
        for (std::size_t n = 1; n <= 8; ++n) {
            masks[n] = n == 8 ? ~std::uint64_t{0} : (std::uint64_t{1} << (8 * n)) - 1U;
        }
        return masks;
    }();
    std::uint64_t word = 0;
    std::memcpy(&word, lane.data + lane.pos, 8);
    const std::size_t remaining = lane.size - lane.pos;
    const auto& s = encoder.long_codes[f::enc_detail::hash3(word)];
    const bool long_hit = std::size_t{s.length} - 1U < 8U && s.length <= remaining &&
                          ((word ^ s.value) & kLowBytes[s.length]) == 0;
    std::uint16_t entry = 0;
    if (remaining >= 2) {
        entry = encoder.short_or_byte[word & 0xFFFF];
    } else {
        const auto one = encoder.byte_codes[word & 0xFF];
        entry = static_cast<std::uint16_t>((1U << 8) | (one != Encoder::kNone ? one : f::kEscape));
    }
    const std::uint8_t code = long_hit ? s.code : static_cast<std::uint8_t>(entry & 0xFF);
    const std::size_t step = long_hit ? s.length : static_cast<std::size_t>(entry >> 8);
    lane.out[0] = code;
    lane.out[1] = static_cast<std::uint8_t>(word & 0xFF);
    lane.out += code == f::kEscape ? 2 : 1;
    lane.pos += step;
}

}  // namespace detail

/// `compress_into` for a value with at least 7 readable bytes after it (any bytes): the 8-byte loads run
/// to the value's end instead of finishing the last bytes from a zero-padded copy, which for short values
/// (ids, codes) is most of them. The same output: a long symbol (3+ bytes) matches only within the value,
/// the two-byte lookup is used only when two bytes remain, and the three-byte hash reads only value bytes
/// whenever a long symbol could fit.
inline std::size_t compress_into_padded(const Encoder& encoder, const std::uint8_t* data, std::size_t size,
                                        std::uint8_t* dst) {
    if (encoder.short_or_byte.empty()) {  // an encoder built without train(): the general path
        return compress_into(encoder, data, size, dst);
    }
    detail::PaddedLane lane{data, size, 0, dst};
    while (lane.pos < size) {
        detail::padded_step(encoder, lane);
    }
    return static_cast<std::size_t>(lane.out - dst);
}

/// `compress_into_padded` for four values at once, each with the padding `compress_into_padded` needs. Their
/// parses are independent, so their dependency chains (load, table lookup, next position) overlap: text
/// compression is latency-bound, one value at a time leaves the core waiting. `written[i]` gets the bytes
/// written at `dst[i]`, the same as four calls to compress_into_padded.
inline void compress_into_padded4(const Encoder& encoder, const std::uint8_t* const data[4],
                                  const std::size_t size[4], std::uint8_t* const dst[4], std::size_t written[4]) {
    if (encoder.short_or_byte.empty()) {
        for (std::size_t i = 0; i < 4; ++i) {
            written[i] = compress_into(encoder, data[i], size[i], dst[i]);
        }
        return;
    }
    detail::PaddedLane lanes[4];
    for (std::size_t i = 0; i < 4; ++i) {
        lanes[i] = detail::PaddedLane{data[i], size[i], 0, dst[i]};
    }
    for (bool active = true; active;) {
        active = false;
        for (auto& lane : lanes) {
            if (lane.pos < lane.size) {
                detail::padded_step(encoder, lane);
                active = true;
            }
        }
    }
    for (std::size_t i = 0; i < 4; ++i) {
        written[i] = static_cast<std::size_t>(lanes[i].out - dst[i]);
    }
}

/// The `kSymbolTableBytes` serialization `parse_symbol_table` (and Lance) reads, encoder switch on.
std::vector<std::uint8_t> serialize(const Encoder& encoder);

}  // namespace nano_lance::fsst
