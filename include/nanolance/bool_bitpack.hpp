// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// LSB-first bit packing for bool columns, matching stock Lance's on-disk bool representation
// (Flat{bits_per_value: 1}, verified empirically against real Lance 8.0.0: bit 0 of byte 0 is row 0).
// nanolance's internal ColumnValues::fixed representation for bool stays one byte per value
// (0x00/0x01); this header only transcodes at the on-disk write/read boundary.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nano_lance::boolpack {

inline std::vector<std::uint8_t> pack_lsb_first(const std::uint8_t* values, std::size_t count) {
    std::vector<std::uint8_t> out((count + 7U) / 8U, 0U);
    for (std::size_t i = 0; i < count; ++i) {
        if (values[i] != 0U) {
            out[i / 8U] |= static_cast<std::uint8_t>(1U << (i % 8U));
        }
    }
    return out;
}

inline void unpack_lsb_first(const std::uint8_t* packed, std::size_t count, std::uint8_t* out) {
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<std::uint8_t>((packed[i / 8U] >> (i % 8U)) & 1U);
    }
}

}  // namespace nano_lance::boolpack
