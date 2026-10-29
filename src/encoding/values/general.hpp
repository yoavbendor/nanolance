// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// Lance's general (block) compression of one buffer, both directions. A zstd buffer is [u64 LE uncompressed
/// size][zstd frame] (a bare frame from older Lance is also accepted when decoding); an lz4 buffer is
/// [u32 LE uncompressed size][LZ4 block] (nanolance/lz4_block.hpp encodes and decodes it). Scheme "none"
/// or empty is stored as is, and an empty buffer stays empty. The decoders bound their output by what the
/// compressed bytes can produce, and the decoded-size limit.
namespace nano_lance::general {

/// `raw` framed as Lance's general-compression payload at zstd `level`: [u64 LE size][zstd frame], into `out`.
bool zstd_frame(const std::vector<std::uint8_t>& raw, int level, std::vector<std::uint8_t>& out, std::string& error);

/// `raw` framed as Lance's lz4 general-compression payload: [u32 LE size][LZ4 block], into `out`.
bool lz4_frame(const std::vector<std::uint8_t>& raw, std::vector<std::uint8_t>& out, std::string& error);

/// Inverse of zstd_frame: [u64 LE uncompressed size][zstd frame] -> raw bytes, into `out`. Refuses a declared
/// size that zstd cannot produce from the bytes given, or one the frame's own content size disagrees with.
bool zstd_unframe(const std::vector<std::uint8_t>& framed, std::vector<std::uint8_t>& out, std::string& error);

/// A buffer compressed by Lance's general buffer compressor (`Compression.scheme`: "zstd", "lz4" or
/// "none"/empty), decompressed into `out`. Bounded: never more than zstd/lz4 can produce from the
/// bytes given, nor the decoded-size limit.
bool decompress(const std::string& scheme, const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out,
                std::string& error);

}  // namespace nano_lance::general
