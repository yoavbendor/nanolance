// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Compact, self-contained MD5 (RFC 1321) — so the blob-fetch benchmark tool needs no extra link deps and
// its digests match `md5sum` / Python `hashlib.md5` / pylance-side hashing byte-for-byte. Public-domain
// style reference implementation; not for security use, only for cross-implementation payload comparison.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace md5util {

namespace detail {
inline std::uint32_t rotl(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
}  // namespace detail

// Hash `len` bytes at `data`, return 32-char lowercase hex string.
inline std::string md5_hex(const std::uint8_t* data, std::size_t len) {
    static const std::uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    std::uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    const std::uint64_t bitlen = static_cast<std::uint64_t>(len) * 8U;

    // Process in 64-byte chunks, synthesizing the padding tail without copying the whole message.
    const std::size_t padded = ((len + 8) / 64 + 1) * 64;
    auto byte_at = [&](std::size_t i) -> std::uint8_t {
        if (i < len) {
            return data[i];
        }
        if (i == len) {
            return 0x80;
        }
        if (i >= padded - 8) {
            return static_cast<std::uint8_t>((bitlen >> (8 * (i - (padded - 8)))) & 0xFF);
        }
        return 0x00;
    };

    for (std::size_t off = 0; off < padded; off += 64) {
        std::uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            M[i] = static_cast<std::uint32_t>(byte_at(off + i * 4 + 0)) |
                   (static_cast<std::uint32_t>(byte_at(off + i * 4 + 1)) << 8) |
                   (static_cast<std::uint32_t>(byte_at(off + i * 4 + 2)) << 16) |
                   (static_cast<std::uint32_t>(byte_at(off + i * 4 + 3)) << 24);
        }
        std::uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t F;
            int g;
            if (i < 16) {
                F = (B & C) | (~B & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | (~D & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                F = C ^ (B | ~D);
                g = (7 * i) % 16;
            }
            F = F + A + K[i] + M[g];
            A = D;
            D = C;
            C = B;
            B = B + detail::rotl(F, S[i]);
        }
        a0 += A;
        b0 += B;
        c0 += C;
        d0 += D;
    }

    const std::uint32_t words[4] = {a0, b0, c0, d0};
    static const char* hex = "0123456789abcdef";
    std::string out(32, '0');
    int p = 0;
    for (int w = 0; w < 4; ++w) {
        for (int b = 0; b < 4; ++b) {  // little-endian byte order
            const std::uint8_t byte = static_cast<std::uint8_t>((words[w] >> (8 * b)) & 0xFF);
            out[p++] = hex[byte >> 4];
            out[p++] = hex[byte & 0xF];
        }
    }
    return out;
}

}  // namespace md5util
