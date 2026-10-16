// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/bloom_filter.hpp"

#include <algorithm>
#include <cmath>

namespace nano_lance::bloom_filter {
namespace {

constexpr std::uint64_t kP1 = 0x9E3779B185EBCA87ULL;
constexpr std::uint64_t kP2 = 0xC2B2AE3D27D4EB4FULL;
constexpr std::uint64_t kP3 = 0x165667B19E3779F9ULL;
constexpr std::uint64_t kP4 = 0x85EBCA77C2B2AE63ULL;
constexpr std::uint64_t kP5 = 0x27D4EB2F165667C5ULL;

// sbbf.rs's SALT: one odd multiplier per word of a block.
constexpr std::uint32_t kSalt[8] = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
                                    0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

constexpr std::uint32_t kBlockBytes = 32;  // 8 words of 4 bytes: one block
constexpr std::uint8_t kLog2MinBytes = 5;  // 32 B
constexpr std::uint8_t kLog2MaxBytes = 27;  // 128 MiB

std::uint64_t rotl(std::uint64_t x, unsigned r) { return (x << r) | (x >> (64U - r)); }

std::uint64_t read_le64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (unsigned k = 0; k < 8; ++k) {
        v |= static_cast<std::uint64_t>(p[k]) << (8U * k);
    }
    return v;
}

std::uint32_t read_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::uint64_t xx_round(std::uint64_t acc, std::uint64_t input) {
    acc += input * kP2;
    acc = rotl(acc, 31);
    return acc * kP1;
}

std::uint64_t xx_merge(std::uint64_t acc, std::uint64_t val) {
    acc ^= xx_round(0, val);
    return acc * kP1 + kP4;
}

/// lgamma's and exp's double arithmetic, as sbbf.rs's false_positive_probability.
double false_positive_probability(std::uint64_t ndv, unsigned log2_bytes) {
    constexpr double kWordBits = 32.0;
    constexpr double kBucketWords = 8.0;
    constexpr int kIters = 750;
    const double bytes = std::ldexp(1.0, static_cast<int>(log2_bytes));
    const double items = static_cast<double>(ndv);
    if (items == 0.0) {
        return 0.0;
    }
    // A dense filter's false positives are certain; this also short-cuts the sum.
    if (items / (bytes * 8.0) > 2.0) {
        return 1.0;
    }
    double result = 0.0;
    // lam is B/c: B the bits in a bucket, c the bits per distinct value.
    const double lam = kBucketWords * kWordBits / ((bytes * 8.0) / items);
    const double loglam = std::log(lam);
    // Summed from the smallest terms (the highest i) up, as Lance does.
    for (int i = kIters - 1; i >= 0; --i) {
        const double logp = static_cast<double>(i) * loglam - lam - std::lgamma(static_cast<double>(i + 1));
        const double logfinner = kBucketWords * std::log(1.0 - std::pow(1.0 - 1.0 / kWordBits, i));
        result += std::exp(logp + logfinner);
    }
    return std::min(result, 1.0);
}

}  // namespace

std::uint64_t xxh64(const std::uint8_t* data, std::size_t n) {
    constexpr std::uint64_t seed = 0;
    std::size_t i = 0;
    std::uint64_t h = 0;
    if (n >= 32) {
        std::uint64_t v1 = seed + kP1 + kP2;
        std::uint64_t v2 = seed + kP2;
        std::uint64_t v3 = seed;
        std::uint64_t v4 = seed - kP1;
        while (i + 32 <= n) {
            v1 = xx_round(v1, read_le64(data + i));
            v2 = xx_round(v2, read_le64(data + i + 8));
            v3 = xx_round(v3, read_le64(data + i + 16));
            v4 = xx_round(v4, read_le64(data + i + 24));
            i += 32;
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xx_merge(h, v1);
        h = xx_merge(h, v2);
        h = xx_merge(h, v3);
        h = xx_merge(h, v4);
    } else {
        h = seed + kP5;
    }
    h += static_cast<std::uint64_t>(n);
    while (i + 8 <= n) {
        h ^= xx_round(0, read_le64(data + i));
        h = rotl(h, 27) * kP1 + kP4;
        i += 8;
    }
    if (i + 4 <= n) {
        h ^= static_cast<std::uint64_t>(read_le32(data + i)) * kP1;
        h = rotl(h, 23) * kP2 + kP3;
        i += 4;
    }
    while (i < n) {
        h ^= static_cast<std::uint64_t>(data[i]) * kP5;
        h = rotl(h, 11) * kP1;
        ++i;
    }
    h ^= h >> 33U;
    h *= kP2;
    h ^= h >> 29U;
    h *= kP3;
    h ^= h >> 32U;
    return h;
}

std::uint8_t log2_bytes_for(std::uint64_t ndv, double fpp) {
    // The smallest size whose rate is at most fpp (sbbf.rs's binary search over 0..64).
    unsigned low = 0;
    unsigned high = 64;
    while (high > low + 1) {
        const unsigned mid = (high + low) / 2;
        if (false_positive_probability(ndv, mid) <= fpp) {
            high = mid;
        } else {
            low = mid;
        }
    }
    return static_cast<std::uint8_t>(std::clamp<unsigned>(high, kLog2MinBytes, kLog2MaxBytes));
}

std::vector<std::uint8_t> empty_filter(std::size_t num_bytes) { return std::vector<std::uint8_t>(num_bytes, 0); }

void insert(std::uint8_t* filter, std::size_t num_bytes, std::uint64_t hash) {
    const std::uint64_t blocks = num_bytes / kBlockBytes;
    // The block: the hash's high half scaled to the block count.
    const std::uint64_t block = ((hash >> 32U) * blocks) >> 32U;
    std::uint8_t* words = filter + block * kBlockBytes;
    const auto low = static_cast<std::uint32_t>(hash);
    for (unsigned i = 0; i < 8; ++i) {
        const std::uint32_t bit = (low * kSalt[i]) >> 27U;
        std::uint8_t* word = words + 4U * i;
        const std::uint32_t current = read_le32(word);
        const std::uint32_t updated = current | (1U << bit);
        for (unsigned k = 0; k < 4; ++k) {
            word[k] = static_cast<std::uint8_t>(updated >> (8U * k));
        }
    }
}

bool may_contain(const std::uint8_t* filter, std::size_t num_bytes, std::uint64_t hash) {
    const std::uint64_t blocks = num_bytes / kBlockBytes;
    const std::uint64_t block = ((hash >> 32U) * blocks) >> 32U;
    const std::uint8_t* words = filter + block * kBlockBytes;
    const auto low = static_cast<std::uint32_t>(hash);
    for (unsigned i = 0; i < 8; ++i) {
        const std::uint32_t bit = (low * kSalt[i]) >> 27U;
        if (((read_le32(words + 4U * i) >> bit) & 1U) == 0U) {
            return false;
        }
    }
    return true;
}

}  // namespace nano_lance::bloom_filter
