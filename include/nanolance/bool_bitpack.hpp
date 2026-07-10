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

// Pack `count` byte-per-value bools into ceil(count/8) LSB-first bytes, into `out`. Each output byte
// is composed in a register and written exactly once, so no pre-zeroing of `out` is needed -- a caller
// reusing `out` across chunks pays no per-chunk zero-fill (unlike the previous set-bits-via-|= shape,
// which required a zeroed buffer).
inline void pack_lsb_first(const std::uint8_t* values, std::size_t count, std::vector<std::uint8_t>& out) {
    const std::size_t out_bytes = (count + 7U) / 8U;
    out.resize(out_bytes);
    const std::size_t full_bytes = count / 8U;
    for (std::size_t j = 0; j < full_bytes; ++j) {
        const std::uint8_t* v = values + j * 8U;
        std::uint8_t b = 0;
        for (unsigned k = 0; k < 8U; ++k) {
            b = static_cast<std::uint8_t>(b | static_cast<std::uint8_t>((v[k] != 0U ? 1U : 0U) << k));
        }
        out[j] = b;
    }
    if (full_bytes < out_bytes) {
        std::uint8_t b = 0;
        for (std::size_t i = full_bytes * 8U; i < count; ++i) {
            if (values[i] != 0U) {
                b = static_cast<std::uint8_t>(b | static_cast<std::uint8_t>(1U << (i % 8U)));
            }
        }
        out[full_bytes] = b;
    }
}

// Convenience overload returning a fresh buffer (one-shot callers / tests).
inline std::vector<std::uint8_t> pack_lsb_first(const std::uint8_t* values, std::size_t count) {
    std::vector<std::uint8_t> out;
    pack_lsb_first(values, count, out);
    return out;
}

inline void unpack_lsb_first(const std::uint8_t* packed, std::size_t count, std::uint8_t* out) {
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<std::uint8_t>((packed[i / 8U] >> (i % 8U)) & 1U);
    }
}

}  // namespace nano_lance::boolpack
