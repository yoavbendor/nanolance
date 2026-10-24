// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// float -> IEEE binary16 (base/float16.hpp), as numpy rounds it: every finite half reads back as itself,
// a value halfway between two halves rounds to the even one (subnormals included), values from 65520 up
// overflow to infinity, half the smallest subnormal and less rounds to zero, and NaN stays a NaN.

#include "base/float16.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

/// The value of half bits `h` (an independent decoder; finite values only).
double half_value(std::uint16_t h) {
    const double sign = (h & 0x8000U) != 0 ? -1.0 : 1.0;
    const int exponent = (h >> 10) & 0x1F;
    const int mantissa = h & 0x3FF;
    if (exponent == 0) {
        return sign * std::ldexp(mantissa, -24);
    }
    return sign * std::ldexp(1024 + mantissa, exponent - 25);
}

}  // namespace

int main() {
    using nano_lance::float16_bits;

    // Known values.
    require(float16_bits(1.0F) == 0x3C00U, "1.0");
    require(float16_bits(-2.0F) == 0xC000U, "-2.0");
    require(float16_bits(0.0F) == 0x0000U, "+0");
    require(float16_bits(-0.0F) == 0x8000U, "-0 keeps its sign");
    require(float16_bits(0.1F) == 0x2E66U, "0.1");
    require(float16_bits(65504.0F) == 0x7BFFU, "the largest half");
    require(float16_bits(65519.0F) == 0x7BFFU, "just under the overflow threshold");
    require(float16_bits(65520.0F) == 0x7C00U, "65520 rounds to infinity");
    require(float16_bits(1e30F) == 0x7C00U, "a large float overflows to infinity");
    require(float16_bits(-1e30F) == 0xFC00U, "and a large negative one to -infinity");
    require(float16_bits(std::numeric_limits<float>::infinity()) == 0x7C00U, "+inf");
    const auto nan = float16_bits(std::numeric_limits<float>::quiet_NaN());
    require((nan & 0x7C00U) == 0x7C00U && (nan & 0x3FFU) != 0U, "NaN stays NaN");
    require(float16_bits(std::ldexp(1.0F, -14)) == 0x0400U, "the smallest normal");
    require(float16_bits(std::ldexp(1.0F, -24)) == 0x0001U, "the smallest subnormal");
    require(float16_bits(std::ldexp(1.0F, -25)) == 0x0000U, "half the smallest subnormal ties to even (zero)");
    require(float16_bits(std::ldexp(1.0F, -26)) == 0x0000U, "a quarter of it rounds to zero");
    require(float16_bits(std::ldexp(3.0F, -26)) == 0x0001U, "three quarters of it rounds up");
    require(float16_bits(std::ldexp(3.0F, -25)) == 0x0002U, "1.5 subnormal units ties to even (2)");
    require(float16_bits(1.0F + std::ldexp(1.0F, -11)) == 0x3C00U, "1 + half an ulp ties to even (down)");
    require(float16_bits(1.0F + std::ldexp(3.0F, -11)) == 0x3C02U, "1 + 1.5 ulp ties to even (up)");
    require(float16_bits(std::ldexp(1023.5F, -24)) == 0x0400U,
            "a subnormal rounding up carries into the smallest normal");

    // Every finite half: its value maps back to it, and the midpoint to the next half rounds to the even
    // one of the two.
    for (std::uint32_t h = 0; h < 0x10000U; ++h) {
        if ((h & 0x7C00U) == 0x7C00U) {
            continue;  // inf / NaN
        }
        const auto bits = static_cast<std::uint16_t>(h);
        const double v = half_value(bits);
        require(float16_bits(static_cast<float>(v)) == bits, "half " + std::to_string(h) + " reads back as itself");
        if ((h & 0x7FFFU) < 0x7BFFU) {
            const auto next = static_cast<std::uint16_t>(h + 1U);
            const double mid = (v + half_value(next)) / 2.0;
            const auto even = (h & 1U) == 0U ? bits : next;
            require(float16_bits(static_cast<float>(mid)) == even,
                    "the midpoint after half " + std::to_string(h) + " rounds to even");
        }
    }

    std::cout << "float16: all checks passed\n";
    return 0;
}
