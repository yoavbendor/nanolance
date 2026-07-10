// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Faithful C++ port of the FastLanes 1024-element bit-packing kernel vendored by Lance
// (lance-bitpacking = spiraldb/fastlanes). Packs exactly 1024 values of an unsigned word type T
// into `width` bits each, using the transposed FL_ORDER layout, so the output is byte-identical to
// what Lance's InlineBitpacking emits and can be read back by stock Lance.
//
// Layout: output length is 1024*width/(8*sizeof(T)) words of type T. index(row,lane) selects the
// input element for a given (row, lane) within the 1024-element block; LANES = 1024/(8*sizeof(T)).
//
// Reference: rust/compression/bitpacking/src/lib.rs (pack!/unpack! macros, BitPacking trait). That
// Rust source generates a *separate, fully-unrolled function per bit width* (pack_8_1..pack_8_8,
// pack_64_1..pack_64_64, ...) via macro expansion, so `width` is a compile-time literal baked into
// each generated function body -- every shift amount and word-boundary-crossing branch is constant-
// folded, not computed at runtime. The kernels below do the C++ equivalent: pack_1024_w<T,Width>/
// unpack_1024_w<T,Width> take Width as a template (compile-time) constant, and the public
// pack_1024/unpack_1024 (runtime `width` argument, matching the existing call sites) dispatch to the
// right instantiation via a compile-time-built function-pointer table (one entry per width), so the
// jump itself is O(1) rather than a linear chain of comparisons.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace nano_lance::fastlanes {

inline constexpr int kFlOrder[8] = {0, 4, 2, 6, 1, 5, 3, 7};

inline constexpr std::size_t fl_index(std::size_t row, std::size_t lane) {
    const std::size_t o = row / 8U;
    const std::size_t s = row % 8U;
    return (static_cast<std::size_t>(kFlOrder[o]) * 16U) + (s * 128U) + lane;
}

template <class T>
inline constexpr T fl_mask(unsigned width) {
    constexpr unsigned kBits = sizeof(T) * 8U;
    if (width == 0U) {
        return T(0);
    }
    if (width >= kBits) {
        return static_cast<T>(~T(0));
    }
    return static_cast<T>((T(1) << width) - T(1));
}

// Row-outer, lane-inner loop order (same rationale in both kernels below): fl_index(row,lane) is
// contiguous in `lane` for a fixed row, and so is `in[kLanes*word + lane]`, so every inner loop walks
// a contiguous run the compiler can auto-vectorize, unlike a lane-outer order where each lane's row
// loop carries a serial accumulator dependency that blocks vectorization. Width being a template
// constant here (not a runtime argument) additionally lets the compiler constant-fold every
// shift/word-boundary computation per row, matching what the Rust macro-generated functions get from
// literal bit widths. Bit-level arithmetic verified byte-identical to a runtime-width, lane-outer
// reference implementation across every (T, Width) combination via a standalone comparison harness.

template <class T, unsigned Width>
inline void pack_1024_w(const T* in, T* out) {
    constexpr unsigned kBits = sizeof(T) * 8U;
    constexpr std::size_t kLanes = 1024U / kBits;
    constexpr T mask = fl_mask<T>(Width);
    T tmp[128];  // kLanes <= 1024/8 = 128 (the T=uint8_t case)
    for (unsigned row = 0; row < kBits; ++row) {
        const T* in_row = in + fl_index(row, 0);
        const unsigned shift = (row * Width) % kBits;
        const unsigned curr_word = (row * Width) / kBits;
        const unsigned next_word = ((row + 1U) * Width) / kBits;
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
            const unsigned remaining = ((row + 1U) * Width) % kBits;
            const unsigned rshift = Width - remaining;
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                tmp[lane] = static_cast<T>(static_cast<T>(in_row[lane] & mask) >> rshift);
            }
        }
    }
}

template <class T, unsigned Width>
inline void unpack_1024_w(const T* in, T* out) {
    constexpr unsigned kBits = sizeof(T) * 8U;
    constexpr std::size_t kLanes = 1024U / kBits;
    T src[128];
    std::memcpy(src, in, kLanes * sizeof(T));
    for (unsigned row = 0; row < kBits; ++row) {
        const unsigned shift = (row * Width) % kBits;
        const unsigned curr_word = (row * Width) / kBits;
        const unsigned next_word = ((row + 1U) * Width) / kBits;
        T* out_row = out + fl_index(row, 0);
        if (next_word > curr_word) {
            const unsigned remaining = ((row + 1U) * Width) % kBits;
            const unsigned current_bits = Width - remaining;
            const T low_mask = fl_mask<T>(current_bits);
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                out_row[lane] = static_cast<T>((src[lane] >> shift) & low_mask);
            }
            if (next_word < Width) {
                const T* next_in = in + kLanes * next_word;
                const T high_mask = fl_mask<T>(remaining);
                for (std::size_t lane = 0; lane < kLanes; ++lane) {
                    src[lane] = next_in[lane];
                    out_row[lane] =
                        static_cast<T>(out_row[lane] | static_cast<T>((src[lane] & high_mask) << current_bits));
                }
            }
        } else {
            const T mask = fl_mask<T>(Width);
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                out_row[lane] = static_cast<T>((src[lane] >> shift) & mask);
            }
        }
    }
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
            std::memcpy(out + kLanes * row, in + fl_index(row, 0), kLanes * sizeof(T));
        }
        return;
    }
    using Fn = void (*)(const T*, T*);
    static constexpr auto table = []<unsigned... Ws>(std::integer_sequence<unsigned, Ws...>) {
        return std::array<Fn, sizeof...(Ws)>{&pack_1024_w<T, Ws + 1U>...};
    }(std::make_integer_sequence<unsigned, kBits - 1U>{});
    table[width - 1U](in, out);
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
            std::memcpy(out + fl_index(row, 0), in + kLanes * row, kLanes * sizeof(T));
        }
        return;
    }
    using Fn = void (*)(const T*, T*);
    static constexpr auto table = []<unsigned... Ws>(std::integer_sequence<unsigned, Ws...>) {
        return std::array<Fn, sizeof...(Ws)>{&unpack_1024_w<T, Ws + 1U>...};
    }(std::make_integer_sequence<unsigned, kBits - 1U>{});
    table[width - 1U](in, out);
}

/// Number of packed T-words produced by pack_1024 at this width.
template <class T>
inline constexpr std::size_t packed_words_1024(unsigned width) {
    constexpr unsigned kBits = sizeof(T) * 8U;
    return (1024U * width) / kBits;
}

}  // namespace nano_lance::fastlanes
