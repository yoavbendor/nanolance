// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// 128-bit integers for the decimal paths. MSVC has no __int128, so the type is either the compiler's
// (GCC/Clang, when __SIZEOF_INT128__ is set) or a two-word class with the same operators and the same
// semantics: two's complement wrapping, truncating division (the remainder takes the dividend's sign),
// arithmetic right shift for the signed type. -DNANOLANCE_PORTABLE_INT128 forces the class everywhere
// (used to test it on Linux). Wide128 is always compiled so tests can check it against __int128.

#pragma once

#include <cstdint>
#include <type_traits>

#if defined(__SIZEOF_INT128__) && !defined(NANOLANCE_PORTABLE_INT128)
#define NANOLANCE_NATIVE_INT128 1
#else
#define NANOLANCE_NATIVE_INT128 0
#endif

namespace nano_lance {
namespace i128 {

template <bool Signed>
struct Wide128 {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    constexpr Wide128() = default;
    constexpr Wide128(std::uint64_t low, std::uint64_t high) : lo(low), hi(high) {}
    template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    constexpr Wide128(T v)  // NOLINT: implicit, like the built-in integer types
        : lo(static_cast<std::uint64_t>(v)),
          hi(std::is_signed_v<T> && static_cast<std::int64_t>(v) < 0 ? ~std::uint64_t{0} : 0) {}

    explicit constexpr operator std::uint64_t() const { return lo; }
    explicit constexpr operator Wide128<!Signed>() const { return Wide128<!Signed>(lo, hi); }

    constexpr bool negative() const { return Signed && (hi >> 63) != 0; }
    constexpr bool bit(int i) const { return i >= 64 ? ((hi >> (i - 64)) & 1U) != 0 : ((lo >> i) & 1U) != 0; }

    friend constexpr bool operator==(Wide128 a, Wide128 b) { return a.lo == b.lo && a.hi == b.hi; }
    friend constexpr bool operator!=(Wide128 a, Wide128 b) { return !(a == b); }
    friend constexpr bool operator<(Wide128 a, Wide128 b) {
        if (a.hi != b.hi) {
            return Signed ? static_cast<std::int64_t>(a.hi) < static_cast<std::int64_t>(b.hi) : a.hi < b.hi;
        }
        return a.lo < b.lo;
    }
    friend constexpr bool operator>(Wide128 a, Wide128 b) { return b < a; }
    friend constexpr bool operator<=(Wide128 a, Wide128 b) { return !(b < a); }
    friend constexpr bool operator>=(Wide128 a, Wide128 b) { return !(a < b); }

    friend constexpr Wide128 operator+(Wide128 a, Wide128 b) {
        const std::uint64_t lo_sum = a.lo + b.lo;
        return Wide128(lo_sum, a.hi + b.hi + (lo_sum < a.lo ? 1U : 0U));
    }
    friend constexpr Wide128 operator-(Wide128 a, Wide128 b) {
        return Wide128(a.lo - b.lo, a.hi - b.hi - (a.lo < b.lo ? 1U : 0U));
    }
    friend constexpr Wide128 operator-(Wide128 a) { return Wide128() - a; }
    friend constexpr Wide128 operator*(Wide128 a, Wide128 b) {  // low 128 bits of the product
        const auto p = mul64(a.lo, b.lo);
        return Wide128(p.lo, p.hi + a.lo * b.hi + a.hi * b.lo);
    }
    friend constexpr Wide128 operator/(Wide128 a, Wide128 b) {
        Wide128 q;
        Wide128 r;
        udivmod(magnitude(a), magnitude(b), q, r);
        return (a.negative() != b.negative()) ? -q : q;
    }
    friend constexpr Wide128 operator%(Wide128 a, Wide128 b) {
        Wide128 q;
        Wide128 r;
        udivmod(magnitude(a), magnitude(b), q, r);
        return a.negative() ? -r : r;
    }
    friend constexpr Wide128 operator&(Wide128 a, Wide128 b) { return Wide128(a.lo & b.lo, a.hi & b.hi); }
    friend constexpr Wide128 operator|(Wide128 a, Wide128 b) { return Wide128(a.lo | b.lo, a.hi | b.hi); }
    friend constexpr Wide128 operator^(Wide128 a, Wide128 b) { return Wide128(a.lo ^ b.lo, a.hi ^ b.hi); }
    friend constexpr Wide128 operator<<(Wide128 a, int n) {
        if (n == 0) {
            return a;
        }
        if (n >= 128) {
            return Wide128();
        }
        if (n >= 64) {
            return Wide128(0, a.lo << (n - 64));
        }
        return Wide128(a.lo << n, (a.hi << n) | (a.lo >> (64 - n)));
    }
    friend constexpr Wide128 operator>>(Wide128 a, int n) {
        if (n == 0) {
            return a;
        }
        const std::uint64_t fill = (Signed && a.negative()) ? ~std::uint64_t{0} : 0;
        if (n >= 128) {
            return Wide128(fill, fill);
        }
        if (n >= 64) {
            return Wide128(shr64(a.hi, n - 64, fill), fill);
        }
        return Wide128((a.lo >> n) | (a.hi << (64 - n)), shr64(a.hi, n, fill));
    }

    constexpr Wide128& operator+=(Wide128 b) { return *this = *this + b; }
    constexpr Wide128& operator-=(Wide128 b) { return *this = *this - b; }
    constexpr Wide128& operator*=(Wide128 b) { return *this = *this * b; }
    constexpr Wide128& operator/=(Wide128 b) { return *this = *this / b; }
    constexpr Wide128& operator%=(Wide128 b) { return *this = *this % b; }
    constexpr Wide128& operator&=(Wide128 b) { return *this = *this & b; }
    constexpr Wide128& operator|=(Wide128 b) { return *this = *this | b; }
    constexpr Wide128& operator^=(Wide128 b) { return *this = *this ^ b; }
    constexpr Wide128& operator<<=(int n) { return *this = *this << n; }
    constexpr Wide128& operator>>=(int n) { return *this = *this >> n; }

    // |v| as the unsigned value with the same bits (so the minimum of the signed type maps to 2^127).
    static constexpr Wide128 magnitude(Wide128 v) { return v.negative() ? Wide128() - v : v; }

    // 64 x 64 -> 128 bits, by 32-bit limbs.
    struct Product {
        std::uint64_t lo;
        std::uint64_t hi;
    };
    static constexpr Product mul64(std::uint64_t a, std::uint64_t b) {
        const std::uint64_t mask = 0xFFFFFFFFU;
        const std::uint64_t a0 = a & mask;
        const std::uint64_t a1 = a >> 32;
        const std::uint64_t b0 = b & mask;
        const std::uint64_t b1 = b >> 32;
        const std::uint64_t p00 = a0 * b0;
        const std::uint64_t p01 = a0 * b1;
        const std::uint64_t p10 = a1 * b0;
        const std::uint64_t p11 = a1 * b1;
        const std::uint64_t mid = (p00 >> 32) + (p01 & mask) + (p10 & mask);
        return Product{(mid << 32) | (p00 & mask), p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32)};
    }
    static constexpr std::uint64_t shr64(std::uint64_t x, int s, std::uint64_t fill) {
        return s == 0 ? x : ((x >> s) | (fill << (64 - s)));
    }
    // Unsigned restoring division on the bit patterns; d != 0.
    static constexpr void udivmod(Wide128 n, Wide128 d, Wide128& q, Wide128& r) {
        q = Wide128();
        r = Wide128();
        for (int i = 127; i >= 0; --i) {
            const bool top = (r.hi >> 63) != 0;
            r = r << 1;
            r.lo |= n.bit(i) ? 1U : 0U;
            const bool ge = top || !(r.hi < d.hi || (r.hi == d.hi && r.lo < d.lo));
            if (ge) {
                r = r - d;
                q = q | (Wide128(1, 0) << i);
            }
        }
    }
};

// Overflow-checked operations on Wide128<true>: the result is the wrapped value; true means overflow.
inline bool add_overflow(Wide128<true> a, Wide128<true> b, Wide128<true>& r) {
    r = a + b;
    return a.negative() == b.negative() && r.negative() != a.negative();
}
inline bool sub_overflow(Wide128<true> a, Wide128<true> b, Wide128<true>& r) {
    r = a - b;
    return a.negative() != b.negative() && r.negative() != a.negative();
}
inline bool mul_overflow(Wide128<true> a, Wide128<true> b, Wide128<true>& r) {
    // Exact unsigned product of the magnitudes, from 64-bit words: a = a1*2^64 + a0, b = b1*2^64 + b0.
    const Wide128<false> ma = static_cast<Wide128<false>>(Wide128<true>::magnitude(a));
    const Wide128<false> mb = static_cast<Wide128<false>>(Wide128<true>::magnitude(b));
    const auto p00 = Wide128<true>::mul64(ma.lo, mb.lo);
    const auto c1 = Wide128<true>::mul64(ma.hi, mb.lo);  // a1 * b0, weight 2^64
    const auto c2 = Wide128<true>::mul64(ma.lo, mb.hi);  // a0 * b1, weight 2^64
    const std::uint64_t mid = c1.lo + c2.lo;             // weight 2^64
    const std::uint64_t top = p00.hi + mid;  // the high word of the exact product, when it fits
    const bool of = (ma.hi != 0 && mb.hi != 0) || c1.hi != 0 || c2.hi != 0 || mid < c1.lo || top < mid;
    r = a * b;  // wrapped; the exact value when there is no overflow
    if (of) {
        return true;
    }
    const Wide128<false> p(p00.lo, top);  // the exact magnitude product, which fits
    const bool neg = a.negative() != b.negative();
    const bool fits = (p.hi >> 63) == 0 || (neg && p.hi == (std::uint64_t{1} << 63) && p.lo == 0);
    return !fits;
}

}  // namespace i128

#if NANOLANCE_NATIVE_INT128
using I128 = __int128;
using U128 = unsigned __int128;

inline bool i128_add_overflow(I128 a, I128 b, I128& r) { return __builtin_add_overflow(a, b, &r); }
inline bool i128_sub_overflow(I128 a, I128 b, I128& r) { return __builtin_sub_overflow(a, b, &r); }
inline bool i128_mul_overflow(I128 a, I128 b, I128& r) { return __builtin_mul_overflow(a, b, &r); }
#else
using I128 = i128::Wide128<true>;
using U128 = i128::Wide128<false>;

inline bool i128_add_overflow(I128 a, I128 b, I128& r) { return i128::add_overflow(a, b, r); }
inline bool i128_sub_overflow(I128 a, I128 b, I128& r) { return i128::sub_overflow(a, b, r); }
inline bool i128_mul_overflow(I128 a, I128 b, I128& r) { return i128::mul_overflow(a, b, r); }
#endif

}  // namespace nano_lance
