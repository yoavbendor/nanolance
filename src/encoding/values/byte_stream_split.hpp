// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <span>

/// Lance's byte-stream split of fixed-width values (floats and doubles, 4 and 8 bytes: the widths Lance
/// writes it for), both directions. n values of `width` bytes become `width` planes of n bytes each: byte k
/// of value i goes to position k * n + i, so each plane holds one byte position of every value. The planes
/// compress far better under zstd than the interleaved values do; the general compression of the planes is
/// values/general's job, not this module's.
namespace nano_lance::byte_stream_split {

/// The `values` (a whole number of `width`-byte values) as planes into `out` (at least as large as `values`,
/// every byte it covers overwritten). False when `width` is 0, `values` is not a whole number of values, or
/// `out` is too small.
bool encode(std::span<const std::byte> values, std::size_t width, std::span<std::byte> out);

/// The inverse of encode: `planes` (a whole number of `width`-byte values' worth of bytes) back to interleaved
/// values in `out`. False when `width` is 0, `planes` is not a whole number of values, or `out` is too small.
bool decode(std::span<const std::byte> planes, std::size_t width, std::span<std::byte> out);

}  // namespace nano_lance::byte_stream_split
