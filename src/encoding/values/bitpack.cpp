// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Lance's bitpacking (lance-encoding encodings/physical/bitpacking.rs): FastLanes blocks of 1024 values,
// each inline-bitpacked block led by its bit width, or out of line with the width in the descriptor.

#include "encoding/values/bitpack.hpp"

#include <nanom/columnar_encode.hpp>
#include <nanom/fastlanes.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace nano_lance::bitpack {
namespace {

// Build one bitpacked chunk buffer: [bit_width as one width_bytes word][FastLanes packed 1024 values],
// into `out`. `count` (<=1024) values are read from `src`; the rest of the 1024-block is zero-padded.
// Every byte of `out` is unconditionally overwritten, so callers reusing `out` across chunks pay no
// per-chunk allocation or zero-fill. Staging into `in` is one bulk memcpy (src is contiguous typed
// data), zeroing only the tail of the final partial chunk instead of the whole 1024-block every call.
template <class T>
void encode_block_typed(const std::uint8_t* src, std::size_t count, std::vector<std::uint8_t>& out) {
    T in[1024];
    if (count != 0U) {  // src may be null for an empty block, and memcpy's pointers must not be
        std::memcpy(in, src, count * sizeof(T));
    }
    if (count < 1024U) {
        std::memset(in + count, 0, (1024U - count) * sizeof(T));
    }
    unsigned width = bit_width(src, count, sizeof(T));
    // thread_local + resize (not a fresh zero-filled vector per chunk): bounded by <=1024 words, fully
    // overwritten by pack_1024 below -- same reuse precedent as unpack_bitpacked_page on the read side.
    thread_local std::vector<T> packed;
    packed.resize(::nanom::columnar::fastlanes::packed_words_1024<T>(width));
    ::nanom::columnar::fastlanes::pack_1024_unchecked<T>(width, in, packed.data());
    out.resize(sizeof(T) * (1U + packed.size()));
    const T width_word = static_cast<T>(width);
    std::memcpy(out.data(), &width_word, sizeof(T));
    if (!packed.empty()) {
        std::memcpy(out.data() + sizeof(T), packed.data(), packed.size() * sizeof(T));
    }
}

// Decode one bitpacked page chunk ([bit_width word][FastLanes packed 1024]) into `num_values`
// little-endian fixed-width values appended to out_fixed.
template <class T>
bool decode_block_typed(const std::vector<std::uint8_t>& chunk, std::uint64_t num_values,
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
    const auto packed_words = ::nanom::columnar::fastlanes::packed_words_1024<T>(width);
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
    ::nanom::columnar::fastlanes::unpack_1024_unchecked<T>(width, packed.data(), values);
    const auto bytes = static_cast<std::size_t>(num_values) * sizeof(T);
    const auto* p = reinterpret_cast<const std::uint8_t*>(values);
    out_fixed.insert(out_fixed.end(), p, p + bytes);
    return true;
}

/// Unpack a buffer that is a SERIES of inline-bitpacked FastLanes blocks into `count` values.
///
/// `decode_block` above decodes exactly one block, because that is what a miniblock chunk
/// carries -- the chunk splitter has already cut the buffer up. A dictionary buffer has no such
/// splitter: it is one buffer holding however many blocks its entries need, each with its own bit
/// width word, so the blocks have to be walked. Widths differ per block, so the block size cannot be
/// computed up front; the position advances as each header is read.
template <class T>
[[nodiscard]] bool decode_series_typed(const std::vector<std::uint8_t>& buffer, std::size_t count,
                                                  std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    // `count` is the descriptor's; the buffer is what exists. Every block costs at least its width
    // word and yields at most 1024 values, so reserve no more than the buffer could produce -- a
    // reserve of the declared count alone asked for 4 GiB from a small page (fuzz_column_decode).
    const std::size_t producible = (buffer.size() / sizeof(T)) * 1024U;
    out.reserve(std::min(count, producible) * sizeof(T));
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
        const auto packed_words = ::nanom::columnar::fastlanes::packed_words_1024<T>(width);
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
        ::nanom::columnar::fastlanes::unpack_1024_unchecked<T>(width, packed.data(), values);
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
[[nodiscard]] bool decode_out_of_line_typed(const std::vector<std::uint8_t>& buffer, std::size_t count,
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
    const auto packed_words = ::nanom::columnar::fastlanes::packed_words_1024<T>(width);
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
        ::nanom::columnar::fastlanes::unpack_1024_unchecked<T>(width, packed.data(), block);
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
            ::nanom::columnar::fastlanes::unpack_1024_unchecked<T>(width, packed.data(), block);
            std::memcpy(dest + whole_blocks * 1024U, block, tail * sizeof(T));
        }
    }
    return true;
}

}  // namespace

// Bit width (0..bits) needed to represent the largest of `count` little-endian values of `width_bytes`.
namespace {

/// The OR of `count` values of type T: one typed loop the compiler vectorizes (a memcpy of a runtime width per
/// value did not, and this runs over every bitpacked column, once to decide and once per block to write).
template <typename T>
std::uint64_t or_all(const std::uint8_t* src, std::size_t count) {
    T acc = 0;
    for (std::size_t i = 0; i < count; ++i) {
        T v;
        std::memcpy(&v, src + i * sizeof(T), sizeof(T));
        acc |= v;
    }
    return acc;
}

}  // namespace

unsigned bit_width(const std::uint8_t* src, std::size_t count, std::size_t width_bytes) {
    std::uint64_t max_value = 0;
    switch (width_bytes) {
        case 1U:
            max_value = or_all<std::uint8_t>(src, count);
            break;
        case 2U:
            max_value = or_all<std::uint16_t>(src, count);
            break;
        case 4U:
            max_value = or_all<std::uint32_t>(src, count);
            break;
        case 8U:
            max_value = or_all<std::uint64_t>(src, count);
            break;
        default:
            for (std::size_t i = 0; i < count; ++i) {
                std::uint64_t v = 0;
                std::memcpy(&v, src + i * width_bytes, width_bytes);
                max_value |= v;
            }
            break;
    }
    unsigned bits = 0;
    while (max_value != 0U) {
        ++bits;
        max_value >>= 1U;
    }
    return bits;
}

void encode_block(const std::uint8_t* src, std::size_t count, std::size_t width_bytes,
                           std::vector<std::uint8_t>& out) {
    switch (width_bytes) {
        case 1U:
            return encode_block_typed<std::uint8_t>(src, count, out);
        case 2U:
            return encode_block_typed<std::uint16_t>(src, count, out);
        case 4U:
            return encode_block_typed<std::uint32_t>(src, count, out);
        default:
            return encode_block_typed<std::uint64_t>(src, count, out);
    }
}

bool decode_block(const std::vector<std::uint8_t>& chunk, std::uint64_t num_values,
                                    std::size_t bytes_per_value, std::vector<std::uint8_t>& out_fixed,
                                    std::string& error) {
    switch (bytes_per_value) {
        case 1U:
            return decode_block_typed<std::uint8_t>(chunk, num_values, out_fixed, error);
        case 2U:
            return decode_block_typed<std::uint16_t>(chunk, num_values, out_fixed, error);
        case 4U:
            return decode_block_typed<std::uint32_t>(chunk, num_values, out_fixed, error);
        default:
            return decode_block_typed<std::uint64_t>(chunk, num_values, out_fixed, error);
    }
}

[[nodiscard]] bool decode_out_of_line(const std::vector<std::uint8_t>& buffer,
                                                         std::size_t count, unsigned width,
                                                         std::size_t bytes_per_value, std::uint8_t* dest,
                                                         const char* what, std::string& error) {
    switch (bytes_per_value) {
        case 1U:
            return decode_out_of_line_typed<std::uint8_t>(buffer, count, width, dest, what, error);
        case 2U:
            return decode_out_of_line_typed<std::uint16_t>(buffer, count, width,
                                                               reinterpret_cast<std::uint16_t*>(dest), what, error);
        case 4U:
            return decode_out_of_line_typed<std::uint32_t>(buffer, count, width,
                                                               reinterpret_cast<std::uint32_t*>(dest), what, error);
        case 8U:
            return decode_out_of_line_typed<std::uint64_t>(buffer, count, width,
                                                               reinterpret_cast<std::uint64_t*>(dest), what, error);
        default:
            error = std::string(what) + " has " + std::to_string(bytes_per_value) +
                    "-byte values, which has no FastLanes kernel";
            return false;
    }
}

[[nodiscard]] bool decode_series(const std::vector<std::uint8_t>& buffer,
                                                           std::size_t count, std::size_t bytes_per_value,
                                                           std::vector<std::uint8_t>& out, std::string& error) {
    switch (bytes_per_value) {
        case 1U:
            return decode_series_typed<std::uint8_t>(buffer, count, out, error);
        case 2U:
            return decode_series_typed<std::uint16_t>(buffer, count, out, error);
        case 4U:
            return decode_series_typed<std::uint32_t>(buffer, count, out, error);
        case 8U:
            return decode_series_typed<std::uint64_t>(buffer, count, out, error);
        default:
            // FastLanes kernels exist for 8/16/32/64-bit lanes only. A 16-byte decimal is never
            // inline-bitpacked by Lance for exactly that reason; refuse by width rather than guess.
            error = "inline-bitpacked dictionary entries are " + std::to_string(bytes_per_value) +
                    " bytes, which has no FastLanes kernel";
            return false;
    }
}

// The inverse of decode_out_of_line_typed: whole 1,024-value blocks, each packed at `width` bits with
// no per-block header, then the final partial block packed too (zero-padded). The decoder's own
// rule for a tail whose packed size equals its raw size is ambiguous, so the caller must refuse
// that case (see fixed_dictionary_out_of_line_ambiguous in encoding_choice.cpp).
template <class T>
void encode_out_of_line_typed(const std::uint8_t* src, std::size_t count, unsigned width,
                              std::vector<std::uint8_t>& out) {
    const auto packed_words = ::nanom::columnar::fastlanes::packed_words_1024<T>(width);
    const std::size_t blocks = (count + 1023U) / 1024U;
    out.assign(blocks * packed_words * sizeof(T), 0);
    thread_local std::vector<T> packed;
    packed.resize(packed_words != 0U ? packed_words : 1U);
    T in[1024];
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t first = b * 1024U;
        const std::size_t n = std::min<std::size_t>(1024U, count - first);
        std::memset(in, 0, sizeof(in));
        std::memcpy(in, src + first * sizeof(T), n * sizeof(T));
        ::nanom::columnar::fastlanes::pack_1024_unchecked<T>(width, in, packed.data());
        if (packed_words != 0U) {
            std::memcpy(out.data() + b * packed_words * sizeof(T), packed.data(), packed_words * sizeof(T));
        }
    }
}

void encode_out_of_line(const std::uint8_t* src, std::size_t count, unsigned width, std::size_t width_bytes,
                        std::vector<std::uint8_t>& out) {
    switch (width_bytes) {
        case 1U:
            return encode_out_of_line_typed<std::uint8_t>(src, count, width, out);
        case 2U:
            return encode_out_of_line_typed<std::uint16_t>(src, count, width, out);
        case 4U:
            return encode_out_of_line_typed<std::uint32_t>(src, count, width, out);
        default:
            return encode_out_of_line_typed<std::uint64_t>(src, count, width, out);
    }
}

}  // namespace nano_lance::bitpack
