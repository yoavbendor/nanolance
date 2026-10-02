// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// 32-bit Roaring bitmaps in their portable serialization -- the format Lance stores an index's
/// fragment coverage in (IndexMetadata.fragment_bitmap) -- read and written as sorted id lists.
/// https://github.com/RoaringBitmap/RoaringFormatSpec
namespace nano_lance::roaring {

/// The set's members, ascending. Array, bitmap and run containers, with or without the run cookie.
bool decode(const std::uint8_t* data, std::size_t size, std::vector<std::uint32_t>& out, std::string& error);

/// `ids` (ascending, distinct) as array and bitmap containers: what any reader of the format reads.
std::vector<std::uint8_t> encode(const std::vector<std::uint32_t>& ids);

}  // namespace nano_lance::roaring
