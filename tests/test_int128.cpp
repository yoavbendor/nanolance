// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The portable 128-bit class (src/int128.hpp) against the compiler's __int128 on edge and random values.
// Known-value checks run everywhere; the comparison runs only where __int128 exists.

#include "int128.hpp"

#include <cstdint>
#include <cstdio>
#include <random>

namespace {

using W = nano_lance::i128::Wide128<true>;
using WU = nano_lance::i128::Wide128<false>;

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

W shl(std::uint64_t v, int n) { return W(v) << n; }

void known_values() {
    const W max = (W(1) << 127) - W(1);
    const W min = W(1) << 127;
    check(max + W(1) == min && (max + W(1)).negative(), "max + 1 wraps to min");
    check(max / W(10) * W(10) + max % W(10) == max, "max div/mod identity");
    check(W(-7) / W(2) == W(-3) && W(-7) % W(2) == W(-1), "-7 / 2 truncates toward zero");
    check(W(7) / W(-2) == W(-3) && W(7) % W(-2) == W(1), "7 / -2 remainder takes the dividend sign");
    check(W(-1) >> 1 == W(-1), "arithmetic shift keeps the sign");
    check((min >> 127) == W(-1), "min >> 127 is -1");
    check(W(-1) == W(-1) && W(-1) < W(0) && WU(W(-1)) > WU(1), "signed and unsigned order");
    check(shl(1, 100) / shl(1, 50) == shl(1, 50), "2^100 / 2^50");
    check(W(10) * W(10) * W(10) == W(1000), "small multiply");

    W r;
    check(nano_lance::i128::add_overflow(max, W(1), r) && r == min, "add overflow flag at max");
    check(!nano_lance::i128::add_overflow(max, W(-1), r), "max - 1 is not an overflow");
    check(nano_lance::i128::sub_overflow(min, W(1), r) && r == max, "sub overflow flag at min");
    check(nano_lance::i128::mul_overflow(min, W(-1), r) && r == min, "min * -1 overflows");
    check(!nano_lance::i128::mul_overflow(min, W(1), r) && r == min, "min * 1 does not");
    check(!nano_lance::i128::mul_overflow(W(-1) << 63, W(1) << 64, r) && r == min,
          "-2^63 * 2^64 = -2^127 fits");
    check(nano_lance::i128::mul_overflow(W(1) << 63, W(1) << 64, r), "2^63 * 2^64 overflows");
    check(nano_lance::i128::mul_overflow(W(1) << 64, W(1) << 64, r), "2^64 * 2^64 overflows");
    check(!nano_lance::i128::mul_overflow(W(1) << 63, W(1) << 63, r) && r == (W(1) << 126),
          "2^63 * 2^63 = 2^126 fits");
    check(WU(0) - WU(1) == WU(~std::uint64_t{0}, ~std::uint64_t{0}), "unsigned 0 - 1 wraps to all ones");
}

#if defined(__SIZEOF_INT128__)
using N = __int128;
using NU = unsigned __int128;

N to_n(W w) { return static_cast<N>((static_cast<NU>(w.hi) << 64) | w.lo); }
W from_n(N v) {
    const NU u = static_cast<NU>(v);
    return W(static_cast<std::uint64_t>(u), static_cast<std::uint64_t>(u >> 64));
}
N to_nu(WU w) { return static_cast<N>((static_cast<NU>(w.hi) << 64) | w.lo); }
WU from_nu(NU v) { return WU(static_cast<std::uint64_t>(v), static_cast<std::uint64_t>(v >> 64)); }

// A random value whose magnitude has a random bit length, so small, large and edge-adjacent values all occur.
N random_n(std::mt19937_64& rng) {
    const int bits = static_cast<int>(rng() % 129);
    NU v = (static_cast<NU>(rng()) << 64) | rng();
    if (bits < 128) {
        v &= (NU{1} << bits) - 1;
    }
    if (rng() % 2 == 0) {
        v = static_cast<NU>(0) - v;
    }
    return static_cast<N>(v);
}

void random_compare(std::uint64_t seed, int rounds) {
    std::mt19937_64 rng(seed);
    const N kMin = static_cast<N>(NU{1} << 127);
    const N kMax = static_cast<N>((NU{1} << 127) - 1);
    const N edges[] = {0, 1, -1, 2, -2, 10, -10, N(INT64_MAX), N(INT64_MIN), N(UINT64_MAX),
                       N(INT64_MAX) + 1, kMax, -kMax, static_cast<N>(NU{1} << 127)};
    for (int round = 0; round < rounds; ++round) {
        const N a = round < 14 * 14 ? edges[round / 14] : random_n(rng);
        const N b = round < 14 * 14 ? edges[round % 14] : random_n(rng);
        const W wa = from_n(a);
        const W wb = from_n(b);
        check(to_n(wa) == a, "round trip");
        check((wa < wb) == (a < b) && (wa == wb) == (a == b) && (wa >= wb) == (a >= b), "signed comparison");
        N r = 0;
        W wr;
        check(nano_lance::i128::add_overflow(wa, wb, wr) == __builtin_add_overflow(a, b, &r) && to_n(wr) == r,
              "add");
        check(nano_lance::i128::sub_overflow(wa, wb, wr) == __builtin_sub_overflow(a, b, &r) && to_n(wr) == r,
              "sub");
        check(nano_lance::i128::mul_overflow(wa, wb, wr) == __builtin_mul_overflow(a, b, &r) && to_n(wr) == r,
              "mul overflow flag and value");
        check(to_n(wa * wb) == static_cast<N>(static_cast<NU>(a) * static_cast<NU>(b)), "wrapping mul");
        if (b != 0 && !(a == kMin && b == -1)) {  // the native quotient overflows there
            check(to_n(wa / wb) == a / b, "signed div");
            check(to_n(wa % wb) == a % b, "signed mod");
        }
        const int s = static_cast<int>(rng() % 130);
        check(to_n(wa >> s) == (s >= 128 ? (a < 0 ? N(-1) : N(0)) : a >> s), "signed shift right");
        const N expect_shl = s >= 128 ? N(0) : static_cast<N>(static_cast<NU>(a) << s);
        check(to_n(wa << s) == expect_shl, "shift left");
        const NU ua = static_cast<NU>(a);
        const NU ub = static_cast<NU>(b);
        const WU wua = from_nu(ua);
        const WU wub = from_nu(ub);
        check(to_nu(wua + wub) == static_cast<N>(ua + ub), "unsigned add");
        check(to_nu(wua * wub) == static_cast<N>(ua * ub), "unsigned mul");
        if (ub != 0) {
            check(static_cast<NU>(to_nu(wua / wub)) == ua / ub, "unsigned div");
            check(static_cast<NU>(to_nu(wua % wub)) == ua % ub, "unsigned mod");
        }
        check((wua < wub) == (ua < ub), "unsigned compare");
    }
}
#endif

}  // namespace

int main() {
    known_values();
#if defined(__SIZEOF_INT128__)
    random_compare(0x5eed, 20000);
#endif
    if (failures == 0) {
        std::printf("int128: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "int128: %d failure(s)\n", failures);
    return 1;
}
