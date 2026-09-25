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
// DECOMPRESSION ONLY. nanolance's writer emits uncompressed `Variable` pages, which stock Lance
// reads; there is nothing to gain from teaching it to *produce* FSST, and a compressor is a much
// larger and riskier piece of code than a decompressor (the symbol table construction is the whole
// algorithm; decoding is a table lookup).
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

}  // namespace nano_lance::fsst
