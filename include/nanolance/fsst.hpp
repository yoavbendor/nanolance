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

// The codec itself is nanom's (nanom/fsst.hpp, nanom/fsst_encode.hpp, namespace nanom::codec::fsst):
// the table format, the decoder and the encoder described above. This header keeps nanolance's
// calling convention -- byte vectors and an error string -- over it.
#include <nanom/fsst.hpp>
#include <nanom/fsst_encode.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nano_lance::fsst {

using ::nanom::codec::fsst::kEscape;
using ::nanom::codec::fsst::kMaxSymbolLength;
using ::nanom::codec::fsst::kSymbolTableBytes;

using SymbolTable = ::nanom::codec::fsst::symbol_table;
using Encoder = ::nanom::codec::fsst::encoder;

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

/// The `kSymbolTableBytes` serialization `parse_symbol_table` (and Lance) reads, encoder switch on.
std::vector<std::uint8_t> serialize(const Encoder& encoder);

}  // namespace nano_lance::fsst
