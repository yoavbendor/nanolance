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
#include <cstring>

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
///
/// Row-outer, lane-inner loop order -- same rationale as unpack_1024 below: fl_index(row,lane) is
/// contiguous in `lane` for a fixed row, so every inner loop here walks a contiguous run the compiler
/// can auto-vectorize, instead of the lane-outer order's serial per-lane accumulator chain. pack has it
/// even easier than unpack: every input value is already available (no reload-on-boundary needed), so
/// the accumulator is just a small per-lane array carried across rows. Verified byte-identical to the
/// lane-outer version across every (T, width) combination, and round-tripped through unpack_1024.
template <class T>
inline void pack_1024(unsigned width, const T* in, T* out) {
    constexpr unsigned kBits = sizeof(T) * 8U;
    constexpr std::size_t kLanes = 1024U / kBits;
    if (width == 0U) {
        return;
    }
    if (width == kBits) {
        for (unsigned row = 0; row < kBits; ++row) {
            std::memcpy(out + kLanes * row, in + fl_index(row, 0), kLanes * sizeof(T));
        }
        return;
    }
    const T mask = fl_mask<T>(width);
    T tmp[128];  // kLanes <= 1024/8 = 128 (the T=uint8_t case)
    for (unsigned row = 0; row < kBits; ++row) {
        const T* in_row = in + fl_index(row, 0);
        const unsigned shift = (row * width) % kBits;
        const unsigned curr_word = (row * width) / kBits;
        const unsigned next_word = ((row + 1U) * width) / kBits;
        if (row == 0U) {
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                tmp[lane] = static_cast<T>(in_row[lane] & mask);
            }
        } else {
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                tmp[lane] = static_cast<T>(tmp[lane] | static_cast<T>(static_cast<T>(in_row[lane] & mask) << shift));
            }
        }
        if (next_word > curr_word) {
            T* out_word = out + kLanes * curr_word;
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                out_word[lane] = tmp[lane];
            }
            const unsigned remaining = ((row + 1U) * width) % kBits;
            const unsigned rshift = width - remaining;
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                tmp[lane] = static_cast<T>(static_cast<T>(in_row[lane] & mask) >> rshift);
            }
        }
    }
}

/// Unpack `in` (1024*width/(8*sizeof(T)) words) into `out[0..1024]` at `width` bits per value.
///
/// Row-outer, lane-inner loop order: fl_index(row,lane) == C(row) + lane is contiguous in `lane` for a
/// fixed row, and so is `in[kLanes*word + lane]`, so every inner loop below walks a contiguous run --
/// unlike a lane-outer order, where each lane's row loop carries a serial `src` dependency chain that
/// blocks vectorization. This shape lets the compiler auto-vectorize the inner loops without any
/// intrinsics (measured 3-7x faster than the lane-outer order at plain -O2, up to ~20x with
/// -march=native/AVX2 available); the bit-level arithmetic is unchanged and verified byte-identical to
/// the lane-outer version across every (T, width) combination.
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
            std::memcpy(out + fl_index(row, 0), in + kLanes * row, kLanes * sizeof(T));
        }
        return;
    }
    // kLanes <= 1024/8 = 128 (the T=uint8_t case); fixed-size so the compiler can keep it in registers
    // instead of spilling to a heap allocation for what's always a small, compile-time-bounded array.
    T src[128];
    std::memcpy(src, in, kLanes * sizeof(T));
    for (unsigned row = 0; row < kBits; ++row) {
        const unsigned curr_word = (row * width) / kBits;
        const unsigned next_word = ((row + 1U) * width) / kBits;
        const unsigned shift = (row * width) % kBits;
        T* out_row = out + fl_index(row, 0);
        if (next_word > curr_word) {
            const unsigned remaining = ((row + 1U) * width) % kBits;
            const unsigned current_bits = width - remaining;
            const T low_mask = fl_mask<T>(current_bits);
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                out_row[lane] = static_cast<T>((src[lane] >> shift) & low_mask);
            }
            if (next_word < width) {
                const T* next_in = in + kLanes * next_word;
                const T high_mask = fl_mask<T>(remaining);
                for (std::size_t lane = 0; lane < kLanes; ++lane) {
                    src[lane] = next_in[lane];
                    out_row[lane] =
                        static_cast<T>(out_row[lane] | static_cast<T>((src[lane] & high_mask) << current_bits));
                }
            }
        } else {
            const T mask = fl_mask<T>(width);
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                out_row[lane] = static_cast<T>((src[lane] >> shift) & mask);
            }
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
