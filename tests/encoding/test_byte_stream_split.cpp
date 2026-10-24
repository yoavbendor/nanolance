// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// encoding/values/byte_stream_split: encode then decode for f32 and f64 at 0, 1, 7, 1024 and 1025 values,
// with NaNs, infinities, negative zero and denormals bit for bit; the plane layout (byte k of value i at
// k * n + i); and the refusals (width 0, a size that is not a whole number of values, a short output).

#include "encoding/values/byte_stream_split.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

using Bytes = std::vector<std::byte>;

/// `n` little-endian values of `width` bytes. The first ones are the edge values of the format: quiet and
/// signalling-style NaN payloads, both infinities, negative zero, the smallest and largest denormals, the
/// largest finite value.
Bytes values(std::size_t n, std::size_t width, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    Bytes out(n * width);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t bits = rng();
        if (width == 4U) {
            const std::uint32_t edges[] = {0x7FC01234U, 0x7F800000U, 0xFF800000U, 0x80000000U,
                                           0x00000001U, 0x007FFFFFU, 0x7F7FFFFFU, 0x7FA00001U};
            bits = i < 8U ? edges[i] : bits & 0xFFFFFFFFU;
        } else {
            const std::uint64_t edges[] = {0x7FF8000000001234ULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
                                           0x8000000000000000ULL, 0x0000000000000001ULL, 0x000FFFFFFFFFFFFFULL,
                                           0x7FEFFFFFFFFFFFFFULL, 0x7FF4000000000001ULL};
            bits = i < 8U ? edges[i] : bits;
        }
        std::memcpy(out.data() + i * width, &bits, width);
    }
    return out;
}

bool same_bytes(const Bytes& a, const Bytes& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

void test_round_trip() {
    for (const std::size_t width : {4U, 8U}) {
        for (const std::size_t n : {0U, 1U, 7U, 1024U, 1025U}) {
            const auto in = values(n, width, n * 97U + width);
            const auto what = std::to_string(width * 8U) + "-bit, " + std::to_string(n) + " values";
            Bytes planes(in.size());
            require(nano_lance::byte_stream_split::encode(in, width, planes), what + ": encode");
            Bytes back(in.size());
            require(nano_lance::byte_stream_split::decode(planes, width, back), what + ": decode");
            require(same_bytes(back, in), what + ": round trip, bit for bit");
        }
    }
}

void test_plane_layout() {
    // Byte k of value i goes to k * n + i, so each plane is one byte position of every value.
    for (const std::size_t width : {4U, 8U}) {
        const std::size_t n = 5U;
        const auto in = values(n, width, 3U);
        Bytes planes(in.size());
        require(nano_lance::byte_stream_split::encode(in, width, planes), "layout encode");
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t k = 0; k < width; ++k) {
                require(planes[k * n + i] == in[i * width + k],
                        std::to_string(width * 8U) + "-bit layout: byte " + std::to_string(k) + " of value " +
                            std::to_string(i) + " is at " + std::to_string(k * n + i));
            }
        }
    }
    // A known small case, spelled out: two f32 values 0x04030201 and 0x08070605 (little-endian bytes).
    const Bytes in = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
    Bytes planes(8U);
    require(nano_lance::byte_stream_split::encode(in, 4U, planes), "known case encode");
    const Bytes expected = {std::byte{0x01}, std::byte{0x05}, std::byte{0x02}, std::byte{0x06},
                            std::byte{0x03}, std::byte{0x07}, std::byte{0x04}, std::byte{0x08}};
    require(same_bytes(planes, expected), "known case: planes are 01 05 | 02 06 | 03 07 | 04 08");
}

void test_refusals() {
    const Bytes in(12U, std::byte{0x2AU});
    Bytes out(16U);
    require(!nano_lance::byte_stream_split::encode(in, 0U, out), "width 0 is refused by encode");
    require(!nano_lance::byte_stream_split::decode(in, 0U, out), "width 0 is refused by decode");
    require(!nano_lance::byte_stream_split::encode(Bytes(9U), 4U, out), "9 bytes at width 4 is refused by encode");
    require(!nano_lance::byte_stream_split::decode(Bytes(10U), 8U, out), "10 bytes at width 8 is refused by decode");
    Bytes short_out(8U);
    require(!nano_lance::byte_stream_split::encode(in, 4U, short_out), "a short output is refused by encode");
    require(!nano_lance::byte_stream_split::decode(in, 4U, short_out), "a short output is refused by decode");
    require(nano_lance::byte_stream_split::encode(Bytes{}, 4U, out), "an empty input encodes to nothing");
}

}  // namespace

int main() {
    test_round_trip();
    test_plane_layout();
    test_refusals();
    std::cout << "encoding/values/byte_stream_split: all checks passed\n";
    return 0;
}
