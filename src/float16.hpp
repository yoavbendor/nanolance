// IEEE binary16 bits of a float (src/float16.hpp): shared by CAST and the vector index builds.
#pragma once

#include <cstdint>
#include <cstring>

namespace nano_lance {

/// The IEEE binary16 bits of `f`, rounded to nearest, ties to even (subnormals and overflow included).
/// nanoarrow's ArrowFloatToHalfFloat truncates and flushes subnormals, which numpy does not.
inline std::uint16_t float16_bits(float f) {
    std::uint32_t x = 0;
    std::memcpy(&x, &f, sizeof x);
    const std::uint32_t sign = (x >> 16) & 0x8000U;
    const std::uint32_t mag = x & 0x7fffffffU;
    if (mag >= 0x7f800000U) {  // infinity, or NaN with its top mantissa bit kept
        return static_cast<std::uint16_t>(sign | 0x7c00U | (mag > 0x7f800000U ? 0x200U : 0U));
    }
    if (mag >= 0x477ff000U) {  // from 65520 up, rounds to infinity
        return static_cast<std::uint16_t>(sign | 0x7c00U);
    }
    if (mag <= 0x33000000U) {  // up to half of the smallest subnormal rounds to zero
        return static_cast<std::uint16_t>(sign);
    }
    if (mag < 0x38800000U) {  // subnormal in binary16: units of 2^-24
        const std::int32_t exponent = static_cast<std::int32_t>(mag >> 23) - 127;  // -25 .. -15 here
        const std::uint32_t significand = (mag & 0x7fffffU) | 0x800000U;
        const auto shift = static_cast<std::uint32_t>(-exponent - 1);  // 2^-24 units: 14 .. 24 bits
        std::uint32_t q = significand >> shift;
        const std::uint32_t rem = significand & ((std::uint32_t{1} << shift) - 1);
        const std::uint32_t half = std::uint32_t{1} << (shift - 1);
        if (rem > half || (rem == half && (q & 1U) != 0)) {
            ++q;  // a carry to 0x400 is the smallest normal, as the encoding has it
        }
        return static_cast<std::uint16_t>(sign | q);
    }
    const std::uint32_t exponent = (mag >> 23) - 127 + 15;
    std::uint32_t q = (exponent << 10) | ((mag & 0x7fffffU) >> 13);
    const std::uint32_t rem = mag & 0x1fffU;
    if (rem > 0x1000U || (rem == 0x1000U && (q & 1U) != 0)) {
        ++q;  // a carry can reach infinity, as it should
    }
    return static_cast<std::uint16_t>(sign | q);
}

}  // namespace nano_lance
