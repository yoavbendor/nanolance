// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The split-block bloom filter behind the BLOOMFILTER scalar index: XXH64 of a value's bytes (against
// its published vectors), the size chosen for a set of values, every inserted value found, the
// false-positive rate on absent values under the configured probability (with slack), and an empty
// filter that holds nothing. Lance reads these filters, so a bit that moves would make an index lie.

#include "nanolance/bloom_filter.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace bf = nano_lance::bloom_filter;

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::uint64_t hash_of_u64(std::uint64_t v) {
    std::uint8_t bytes[8];
    for (unsigned k = 0; k < 8; ++k) {
        bytes[k] = static_cast<std::uint8_t>(v >> (8U * k));
    }
    return bf::xxh64(bytes, sizeof bytes);
}

std::uint64_t hash_of_text(const std::string& s) {
    return bf::xxh64(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

}  // namespace

int main() {
    // XXH64 against its published vectors (seed 0): the empty input and "abc".
    require(bf::xxh64(nullptr, 0) == 0xEF46DB3751D8E999ULL, "xxh64 of the empty input");
    require(hash_of_text("abc") == 0x44BC2CF5AD770999ULL, "xxh64 of \"abc\"");
    // Inputs of 32 bytes and over take the four-lane path: equal bytes hash equally, and a changed length
    // or a changed byte hashes differently.
    const std::string long_text(100, 'x');
    require(hash_of_text(long_text) == hash_of_text(std::string(100, 'x')), "xxh64 is a function of the bytes");
    require(hash_of_text(long_text) != hash_of_text(std::string(99, 'x')), "xxh64 sees the length");
    require(hash_of_text(long_text) != hash_of_text(std::string(100, 'y')), "xxh64 sees the content");

    // The size for a set of values: at least 32 bytes, growing with the number of values and with a lower
    // probability, and clamped at both ends.
    require(bf::log2_bytes_for(0, 0.01) == 5, "no values: the minimum size of 32 bytes");
    require(bf::log2_bytes_for(1, 0.01) == 5, "one value fits the minimum size");
    const auto small = bf::log2_bytes_for(1000, 0.01);
    const auto large = bf::log2_bytes_for(100000, 0.01);
    require(small >= 5 && small < large, "more values take a larger filter");
    require(bf::log2_bytes_for(10000, 0.001) > bf::log2_bytes_for(10000, 0.1),
            "a lower probability takes a larger filter");
    require(bf::log2_bytes_for(1000000000000ULL, 0.000000001) == 27, "clamped to 128 MiB");

    // An empty filter holds nothing.
    {
        const auto empty = bf::empty_filter(32);
        require(empty.size() == 32, "empty_filter has the size asked for");
        for (std::uint64_t v = 0; v < 500; ++v) {
            require(!bf::may_contain(empty.data(), empty.size(), hash_of_u64(v)), "an empty filter holds nothing");
        }
    }

    // The filter of 10000 values, sized for them at 1%: every inserted value is found, and the absent ones
    // are found at a rate under the probability.
    const std::uint64_t ndv = 10000;
    const double fpp = 0.01;
    const std::size_t num_bytes = std::size_t{1} << bf::log2_bytes_for(ndv, fpp);
    auto filter = bf::empty_filter(num_bytes);
    for (std::uint64_t v = 0; v < ndv; ++v) {
        bf::insert(filter.data(), filter.size(), hash_of_u64(v * 7919U));
    }
    for (std::uint64_t v = 0; v < ndv; ++v) {
        require(bf::may_contain(filter.data(), filter.size(), hash_of_u64(v * 7919U)),
                "an inserted value is found: " + std::to_string(v));
    }
    {
        // Absent values are keys never inserted (ndv and up); the rate is taken with slack: 1.5 times the
        // configured probability, for the sampling and the block layout.
        std::uint64_t false_positives = 0;
        const std::uint64_t probes = 200000;
        for (std::uint64_t i = 0; i < probes; ++i) {
            const std::uint64_t absent = (ndv + i) * 7919U;
            if (bf::may_contain(filter.data(), filter.size(), hash_of_u64(absent))) {
                ++false_positives;
            }
        }
        const double rate = static_cast<double>(false_positives) / static_cast<double>(probes);
        require(rate < 1.5 * fpp, "the false-positive rate " + std::to_string(rate) + " is under 1.5 x " +
                                      std::to_string(fpp));
    }

    // Inserting a value twice is the same as once.
    {
        auto once = bf::empty_filter(64);
        auto twice = bf::empty_filter(64);
        bf::insert(once.data(), once.size(), hash_of_text("row"));
        bf::insert(twice.data(), twice.size(), hash_of_text("row"));
        bf::insert(twice.data(), twice.size(), hash_of_text("row"));
        require(once == twice, "inserting a value twice changes nothing");
        require(bf::may_contain(once.data(), once.size(), hash_of_text("row")), "a single value is found");
    }

    std::cout << "bloom_filter: ok (xxh64 vectors, sizes, 10000 values found, false-positive rate)\n";
    return 0;
}
