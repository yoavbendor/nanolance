// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Faithful C++ port of the FastLanes 1024-element bit-packing kernel vendored by Lance
// (lance-bitpacking = spiraldb/fastlanes). Packs exactly 1024 values of an unsigned word type T
// into `width` bits each, using the transposed FL_ORDER layout, so the output is byte-identical to
// what Lance's InlineBitpacking emits and can be read back by stock Lance.
//
// Layout: output length is 1024*width/(8*sizeof(T)) words of type T. index(row,lane) selects the
// input element for a given (row, lane) within the 1024-element block; LANES = 1024/(8*sizeof(T)).
//
// Reference: rust/compression/bitpacking/src/lib.rs (pack!/unpack! macros, BitPacking trait).

#include <cstddef>
#include <cstdint>

namespace nano_lance::fastlanes {

inline constexpr int kFlOrder[8] = {0, 4, 2, 6, 1, 5, 3, 7};

inline constexpr std::size_t fl_index(std::size_t row, std::size_t lane) {
    const std::size_t o = row / 8U;
    const std::size_t s = row % 8U;
    return (static_cast<std::size_t>(kFlOrder[o]) * 16U) + (s * 128U) + lane;
}

template <class T>
inline T fl_mask(unsigned width) {
    constexpr unsigned kBits = sizeof(T) * 8U;
    if (width == 0U) {
        return T(0);
    }
    if (width >= kBits) {
        return static_cast<T>(~T(0));
    }
    return static_cast<T>((T(1) << width) - T(1));
}

/// Pack `in[0..1024]` into `out` at `width` bits per value (width in 0..=8*sizeof(T)).
/// `out` must hold 1024*width/(8*sizeof(T)) words of type T.
template <class T>
inline void pack_1024(unsigned width, const T* in, T* out) {
    constexpr unsigned kBits = sizeof(T) * 8U;
    constexpr std::size_t kLanes = 1024U / kBits;
    if (width == 0U) {
        return;
    }
    if (width == kBits) {
        for (unsigned row = 0; row < kBits; ++row) {
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                out[kLanes * row + lane] = in[fl_index(row, lane)];
            }
        }
        return;
    }
    const T mask = fl_mask<T>(width);
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
        T tmp = 0;
        for (unsigned row = 0; row < kBits; ++row) {
            const T src = static_cast<T>(in[fl_index(row, lane)] & mask);
            if (row == 0U) {
                tmp = src;
            } else {
                tmp = static_cast<T>(tmp | static_cast<T>(src << ((row * width) % kBits)));
            }
            const unsigned curr_word = (row * width) / kBits;
            const unsigned next_word = ((row + 1U) * width) / kBits;
            if (next_word > curr_word) {
                out[kLanes * curr_word + lane] = tmp;
                const unsigned remaining = ((row + 1U) * width) % kBits;
                tmp = static_cast<T>(src >> (width - remaining));
            }
        }
    }
}

/// Unpack `in` (1024*width/(8*sizeof(T)) words) into `out[0..1024]` at `width` bits per value.
template <class T>
inline void unpack_1024(unsigned width, const T* in, T* out) {
    constexpr unsigned kBits = sizeof(T) * 8U;
    constexpr std::size_t kLanes = 1024U / kBits;
    if (width == 0U) {
        for (std::size_t i = 0; i < 1024U; ++i) {
            out[i] = 0;
        }
        return;
    }
    if (width == kBits) {
        for (unsigned row = 0; row < kBits; ++row) {
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                out[fl_index(row, lane)] = in[kLanes * row + lane];
            }
        }
        return;
    }
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
        T src = in[lane];
        for (unsigned row = 0; row < kBits; ++row) {
            const unsigned curr_word = (row * width) / kBits;
            const unsigned next_word = ((row + 1U) * width) / kBits;
            const unsigned shift = (row * width) % kBits;
            T tmp;
            if (next_word > curr_word) {
                const unsigned remaining = ((row + 1U) * width) % kBits;
                const unsigned current_bits = width - remaining;
                tmp = static_cast<T>((src >> shift) & fl_mask<T>(current_bits));
                if (next_word < width) {
                    src = in[kLanes * next_word + lane];
                    tmp = static_cast<T>(tmp | static_cast<T>((src & fl_mask<T>(remaining)) << current_bits));
                }
            } else {
                tmp = static_cast<T>((src >> shift) & fl_mask<T>(width));
            }
            out[fl_index(row, lane)] = tmp;
        }
    }
}

/// Number of packed T-words produced by pack_1024 at this width.
template <class T>
inline constexpr std::size_t packed_words_1024(unsigned width) {
    constexpr unsigned kBits = sizeof(T) * 8U;
    return (1024U * width) / kBits;
}

}  // namespace nano_lance::fastlanes
