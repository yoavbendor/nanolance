// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Lance's BYTE_STREAM_SPLIT for float and double columns, through nanom's kernels (the planes are
// width streams of n bytes, one per byte position of the value).

#include "encoding/values/byte_stream_split.hpp"

#include <nanom/columnar.hpp>
#include <nanom/columnar_encode.hpp>

#include <cstddef>
#include <span>

namespace nano_lance::byte_stream_split {

bool encode(std::span<const std::byte> values, std::size_t width, std::span<std::byte> out) {
    if (width == 0U || values.size() % width != 0U) {
        return false;
    }
    return ::nanom::columnar::byte_stream_split_encode(values, width, values.size() / width, out);
}

bool decode(std::span<const std::byte> planes, std::size_t width, std::span<std::byte> out) {
    if (width == 0U || planes.size() % width != 0U) {
        return false;
    }
    return ::nanom::columnar::byte_stream_split(planes, width, planes.size() / width, out);
}

}  // namespace nano_lance::byte_stream_split
