// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// LZ4 *block* decompression, for reading buffers the Rust `lance` crate compressed with
// `General{ scheme = LZ4 }`.
//
// WHY THIS EXISTS. Lance picks LZ4 over zstd for some buffers -- most visibly a low-cardinality
// string column's dictionary block, which is what a categorical column looks like on disk. nanolance
// read that block raw and failed on its header, so "a `utf8` column with few distinct values" was a
// stock-Lance shape it could not read.
//
// WHY NOT LINK liblz4. Decompression is the entire format below: a token byte, a run of literals,
// and a back-reference. There is no entropy coder, no dictionary negotiation and no framing (the
// LZ4 *frame* format, with its magic number and checksums, is a different thing and is NOT what
// Lance writes). Vendoring a whole library to read ~70 lines of format would cost more than it saved
// -- nanolance already carries zstd only because zstd genuinely needs it.
//
// THE WRAPPER Lance puts around it is `[u32 LE uncompressed size][LZ4 block]`; the size comes from
// the `lz4` crate's `prepend_size` mode, and this reader uses it as the exact output length rather
// than as a hint.
//
// TRUST. Both the declared size and the block are untrusted. The declared size is checked against the
// read-limit budget before anything is allocated, every literal run is checked against the input, and
// every back-reference is checked against how much output exists so far -- so a hostile block can
// make this return false, never read or write out of bounds. A block that decodes to fewer or more
// bytes than it declared is refused rather than accepted short.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nano_lance::lz4_block {

/// Decompress `[u32 LE uncompressed size][LZ4 block]` into `out`, which is REPLACED (not appended
/// to). Returns false with `error` set on a malformed or over-budget block.
bool decompress_sized(const std::vector<std::uint8_t>& sized, std::vector<std::uint8_t>& out,
                      std::string& error);

/// Decompress a bare LZ4 block of exactly `uncompressed_size` bytes. Exposed separately so the fuzz
/// harness can drive the block parser without the size prefix deciding everything.
bool decompress_block(const std::uint8_t* data, std::size_t size, std::size_t uncompressed_size,
                      std::vector<std::uint8_t>& out, std::string& error);

}  // namespace nano_lance::lz4_block
