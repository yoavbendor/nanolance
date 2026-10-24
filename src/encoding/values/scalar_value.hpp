// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// The inline value of a constant page: a Lance scalar value buffer holding a length-1 array, both
/// directions. The layout is [u32 LE num_buffers][u32 LE buffer_len ...][buffers]. A utf8 or binary value is
/// two buffers: offsets [0, len] (two u32, or two u64 when `large`) and the data, which is the value. A
/// fixed-width value too wide to inline in the descriptor is one buffer, the value's bytes; the decoder reads
/// that spelling, the encoder writes only the two-buffer one. Follows lance-arrow's scalar value encoding.
namespace nano_lance::scalar_value {

/// The value's scalar buffer: the two-buffer utf8/binary layout, offsets as u64 when `large`.
std::vector<std::uint8_t> encode_variable(const std::vector<std::uint8_t>& value, bool large);

/// The value (the data buffer, or the single buffer) of a scalar value buffer, into `out`. Every length in the
/// buffer is untrusted: the header, each buffer's length and the data range are checked against the bytes given.
[[nodiscard]] bool decode_variable(const std::vector<std::uint8_t>& buffer, std::vector<std::uint8_t>& out,
                                   std::string& error);

}  // namespace nano_lance::scalar_value
