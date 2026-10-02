// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// LSB-first bit packing for bool columns, matching stock Lance's on-disk bool representation
// (Flat{bits_per_value: 1}, verified empirically against real Lance 8.0.0: bit 0 of byte 0 is row 0).
// nanolance's internal ColumnValues::fixed representation for bool stays one byte per value
// (0x00/0x01); this header only transcodes at the on-disk write/read boundary.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nano_lance::boolpack {

namespace detail {

/// Eight byte-per-value bools (each 0 or 1) as one LSB-first byte: the multiply moves byte i's low bit
/// to bit 56 + i with no carries between the partial products, so one multiply and a shift replace
/// eight shifts and ors.
inline std::uint8_t pack8(const std::uint8_t* v) {
    std::uint64_t x = 0;
    std::memcpy(&x, v, 8U);
    return static_cast<std::uint8_t>(((x & 0x0101010101010101ULL) * 0x0102040810204080ULL) >> 56U);
}

/// byte -> the eight 0/1 bytes it holds, LSB first, as one little-endian u64.
struct UnpackTable {
    std::uint64_t entry[256];
    constexpr UnpackTable() : entry{} {
        for (unsigned b = 0; b < 256U; ++b) {
            std::uint64_t e = 0;
            for (unsigned k = 0; k < 8U; ++k) {
                e |= static_cast<std::uint64_t>((b >> k) & 1U) << (8U * k);
            }
            entry[b] = e;
        }
    }
};
inline constexpr UnpackTable kUnpack{};

}  // namespace detail

// Pack `count` byte-per-value bools (0 or 1, as nanolance holds them) into ceil(count/8) LSB-first
// bytes, into `out`. Each output byte is written exactly once, so `out` needs no pre-zeroing.
inline void pack_lsb_first(const std::uint8_t* values, std::size_t count, std::uint8_t* out) {
    const std::size_t full_bytes = count / 8U;
    for (std::size_t j = 0; j < full_bytes; ++j) {
        out[j] = detail::pack8(values + j * 8U);
    }
    if (full_bytes * 8U < count) {
        std::uint8_t b = 0;
        for (std::size_t i = full_bytes * 8U; i < count; ++i) {
            b = static_cast<std::uint8_t>(b | ((values[i] & 1U) << (i % 8U)));
        }
        out[full_bytes] = b;
    }
}

inline void pack_lsb_first(const std::uint8_t* values, std::size_t count, std::vector<std::uint8_t>& out) {
    out.resize((count + 7U) / 8U);
    pack_lsb_first(values, count, out.data());
}

// Convenience overload returning a fresh buffer (one-shot callers / tests).
inline std::vector<std::uint8_t> pack_lsb_first(const std::uint8_t* values, std::size_t count) {
    std::vector<std::uint8_t> out;
    pack_lsb_first(values, count, out);
    return out;
}

// Expand bits `[bit_offset, bit_offset + count)` of LSB-first `packed` to one 0/1 byte each at `out`.
// Byte-aligned runs go eight values per table lookup.
inline void unpack_lsb_first(const std::uint8_t* packed, std::size_t bit_offset, std::size_t count,
                             std::uint8_t* out) {
    std::size_t i = 0;
    for (; i < count && ((bit_offset + i) & 7U) != 0U; ++i) {
        const auto bit = bit_offset + i;
        out[i] = static_cast<std::uint8_t>((packed[bit >> 3U] >> (bit & 7U)) & 1U);
    }
    const std::uint8_t* src = packed + ((bit_offset + i) >> 3U);
    for (; i + 8U <= count; i += 8U) {
        std::memcpy(out + i, &detail::kUnpack.entry[*src++], 8U);
    }
    for (; i < count; ++i) {
        const auto bit = bit_offset + i;
        out[i] = static_cast<std::uint8_t>((packed[bit >> 3U] >> (bit & 7U)) & 1U);
    }
}

inline void unpack_lsb_first(const std::uint8_t* packed, std::size_t count, std::uint8_t* out) {
    unpack_lsb_first(packed, 0U, count, out);
}

}  // namespace nano_lance::boolpack
