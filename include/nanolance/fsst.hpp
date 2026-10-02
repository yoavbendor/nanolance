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
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nano_lance::fsst {

/// Exact serialized size of a Lance FSST symbol table. Not a maximum: the buffer is fixed-size and
/// zero-padded past `symbol_count`, and a table of any other length is malformed.
inline constexpr std::size_t kSymbolTableBytes = 8U + 256U * 8U + 256U;

/// Longest symbol FSST can define, and therefore the most bytes one input code can expand to.
inline constexpr std::size_t kMaxSymbolLength = 8U;

/// The code that escapes the following byte as a literal.
inline constexpr std::uint8_t kEscape = 255U;

struct SymbolTable {
    /// `encoder_switch` was 0: the values are stored verbatim and must be copied, not decoded.
    bool passthrough = true;
    std::uint32_t symbol_count = 0;
    /// Symbol bytes in order. Only the first `lengths[i]` bytes of entry `i` are meaningful.
    std::array<std::array<std::uint8_t, kMaxSymbolLength>, 256> symbols{};
    /// Length of each symbol, validated to be 1..=8 for every declared symbol.
    std::array<std::uint8_t, 256> lengths{};
};

/// Parse the `Fsst.symbol_table` bytes from the page descriptor. Returns false with `error` set if
/// the buffer is not exactly `kSymbolTableBytes` long, carries the wrong magic, or declares a symbol
/// length outside 1..=8.
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

/// A trained FSST table, ready to compress with. Symbol `i` is code `i`; code 255 is the escape.
struct Encoder {
    struct Slot {
        std::uint64_t value = 0;  // the symbol's bytes, little-endian, zero above `length`
        std::uint8_t length = 0;  // 0 = empty slot
        std::uint8_t code = 0;
    };
    static constexpr std::uint16_t kNone = 0xFFFFU;

    std::uint32_t symbol_count = 0;
    std::array<std::uint64_t, 255> symbols{};
    std::array<std::uint8_t, 255> lengths{};
    /// Lookups: one-byte symbols by byte, two-byte ones by their (little-endian) u16, longer ones
    /// in a 1024-slot table hashed on their first three bytes (one symbol per slot).
    std::array<std::uint16_t, 256> byte_codes{};
    std::vector<std::uint16_t> short_codes;
    std::array<Slot, 1024> long_codes{};
    /// For compression, by the next two bytes: (length << 8) | code of the best symbol of at most two
    /// bytes -- the two-byte one, else the one-byte one, else the escape (length 1). Built by train().
    std::vector<std::uint16_t> short_or_byte;
};

/// Train a table on `values` (a sample of them, if they are large). Returns false when no symbol is
/// worth having -- empty input, say -- and the caller should store the values plain.
bool train(const std::vector<std::pair<const std::uint8_t*, std::size_t>>& values, Encoder& out);

/// Append the compression of one value to `out`: at each position the longest symbol that matches,
/// or an escape and the literal byte.
void compress_value(const Encoder& encoder, const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);

/// `compress_value` into raw memory: writes at most 2 * size bytes at `dst` (every byte escaped)
/// and returns how many it wrote.
std::size_t compress_into(const Encoder& encoder, const std::uint8_t* data, std::size_t size, std::uint8_t* dst);

/// The `kSymbolTableBytes` serialization `parse_symbol_table` (and Lance) reads, encoder switch on.
std::vector<std::uint8_t> serialize(const Encoder& encoder);

}  // namespace nano_lance::fsst
