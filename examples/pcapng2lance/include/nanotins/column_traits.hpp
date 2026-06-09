#pragma once

// The single type -> column bridge. Maps a field type F (scalar, be<>/le<>, byte array) to its SoA
// element type, a backend-neutral `arrow_kind` tag, and a host-order extractor. Kept free of
// nanoarrow so the reflection core stays device-usable; the host glue maps `arrow_kind` -> nanoarrow.

#include "nanotins/bits.hpp"
#include "nanotins/endian.hpp"
#include "nanotins/portability.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace nanotins {

enum class arrow_kind {
    i8, u8, i16, u16, i32, u32, i64, u64, f32, f64, boolean, string, large_binary, fixed_binary
};

template <class U>
constexpr arrow_kind kind_for_scalar() {
    if constexpr (std::is_same_v<U, bool>) {
        return arrow_kind::boolean;
    } else if constexpr (std::is_floating_point_v<U>) {
        return sizeof(U) == 4 ? arrow_kind::f32 : arrow_kind::f64;
    } else if constexpr (std::is_signed_v<U>) {
        return sizeof(U) == 1   ? arrow_kind::i8
               : sizeof(U) == 2 ? arrow_kind::i16
               : sizeof(U) == 4 ? arrow_kind::i32
                                : arrow_kind::i64;
    } else {
        return sizeof(U) == 1   ? arrow_kind::u8
               : sizeof(U) == 2 ? arrow_kind::u16
               : sizeof(U) == 4 ? arrow_kind::u32
                                : arrow_kind::u64;
    }
}

// Primary: undefined on purpose so an unsupported field type is a clear compile error.
template <class F>
struct column_traits;

// Host-order scalar (integral / floating point).
template <class U>
    requires(std::is_arithmetic_v<U>)
struct column_traits<U> {
    using elem = U;
    static constexpr arrow_kind kind = kind_for_scalar<U>();
    static constexpr int fixed_width = 0;
    static constexpr bool variable = false;
    NANOTINS_HD static elem get(U v) { return v; }
};

// Wire big-endian / little-endian scalar: swap happens in host().
template <class U>
struct column_traits<be<U>> {
    using elem = U;
    static constexpr arrow_kind kind = kind_for_scalar<U>();
    static constexpr int fixed_width = 0;
    static constexpr bool variable = false;
    NANOTINS_HD static elem get(const be<U>& f) { return f.host(); }
};
template <class U>
struct column_traits<le<U>> {
    using elem = U;
    static constexpr arrow_kind kind = kind_for_scalar<U>();
    static constexpr int fixed_width = 0;
    static constexpr bool variable = false;
    NANOTINS_HD static elem get(const le<U>& f) { return f.host(); }
};

// Fixed-size byte array (MAC / IPv4 / IPv6): stored as fixed-size-binary, never swapped.
template <std::size_t N>
struct column_traits<std::array<std::uint8_t, N>> {
    using elem = std::array<std::uint8_t, N>;
    static constexpr arrow_kind kind = arrow_kind::fixed_binary;
    static constexpr int fixed_width = static_cast<int>(N);
    static constexpr bool variable = false;
    NANOTINS_HD static elem get(const elem& v) { return v; }
};

// is_bits<F> detection for the columns_of flattening.
template <class F>
struct is_bits : std::false_type {};
template <class Word, class... Fields>
struct is_bits<bits<Word, Fields...>> : std::true_type {};
template <class F>
inline constexpr bool is_bits_v = is_bits<F>::value;

}  // namespace nanotins
