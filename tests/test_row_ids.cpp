// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Row id sequences (rowids.proto): every shape round-trips, and the encoder picks the segment kind
// Lance picks. The oracle against pylance's files is bindings/python/tests/test_stable_row_ids.py.

#include "nanolance/row_ids.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

/// Which field (1 range, 2 holes, 3 bitmap, 4 sorted, 5 array) the first segment of an encoded
/// sequence is: the message is `0a <len> <key> ...`, the segment's oneof key right after its length.
int first_segment_kind(const std::vector<std::uint8_t>& bytes) {
    std::size_t at = 1;
    while (bytes[at] & 0x80U) {
        ++at;
    }
    ++at;  // the segment message's length (a varint)
    return bytes[at] >> 3U;
}

void roundtrip(const std::vector<std::uint64_t>& ids, int expected_kind, const std::string& name) {
    const auto seq = nano_lance::RowIdSequence::from_values(ids);
    const auto bytes = seq.encode();
    check(first_segment_kind(bytes) == expected_kind, name + ": segment kind " + std::to_string(first_segment_kind(bytes)));
    nano_lance::RowIdSequence back;
    std::string error;
    check(nano_lance::RowIdSequence::decode(bytes.data(), bytes.size(), back, error), name + ": decode " + error);
    check(back.to_vector() == ids, name + ": values");
}

}  // namespace

int main() {
    roundtrip({}, 1, "empty");
    roundtrip({10, 11, 12, 13}, 1, "range");
    {  // a few holes in a long run: RangeWithHoles
        std::vector<std::uint64_t> v;
        for (std::uint64_t i = 100; i < 200; ++i) {
            if (i != 120 && i != 150) {
                v.push_back(i);
            }
        }
        roundtrip(v, 2, "holes");
    }
    {  // every other id: a bitmap is the smallest
        std::vector<std::uint64_t> v;
        for (std::uint64_t i = 0; i < 400; i += 2) {
            v.push_back(i);
        }
        roundtrip(v, 3, "bitmap");
    }
    {  // sparse sorted ids: a sorted array
        std::vector<std::uint64_t> v;
        for (std::uint64_t i = 0; i < 50; ++i) {
            v.push_back(i * 100000);
        }
        roundtrip(v, 4, "sorted array");
    }
    roundtrip({5, 3, 9, 1}, 5, "unsorted array");
    roundtrip({1ULL << 40, (1ULL << 40) + 7, (1ULL << 41)}, 4, "64-bit values");

    // A RangeWithBitmap: bit i (least significant first) says start + i is present.
    {
        // 0a 09: segments[0], 9 bytes: 1a 07 (RangeWithBitmap, 7 bytes): 08 02 (start 2) 10 0a (end 10)
        // 1a 01 2d (bitmap 0b00101101): slots 0, 2, 3, 5 -> ids 2, 4, 5, 7
        const std::vector<std::uint8_t> bytes = {0x0a, 0x09, 0x1a, 0x07, 0x08, 0x02, 0x10, 0x0a, 0x1a, 0x01, 0x2d};
        nano_lance::RowIdSequence s;
        std::string error;
        check(nano_lance::RowIdSequence::decode(bytes.data(), bytes.size(), s, error), "bitmap decode " + error);
        check(s.to_vector() == std::vector<std::uint64_t>({2, 4, 5, 7}), "bitmap values");
    }

    // Random sequences round-trip.
    std::mt19937_64 rng(7);
    for (int round = 0; round < 300; ++round) {
        std::vector<std::uint64_t> v;
        const auto n = rng() % 200;
        std::uint64_t at = rng() % 1000;
        const bool sorted = rng() % 3 != 0;
        const auto gap = 1 + rng() % (rng() % 2 ? 3 : 100000);
        for (std::uint64_t i = 0; i < n; ++i) {
            at += 1 + rng() % gap;
            v.push_back(at);
        }
        if (!sorted) {
            std::shuffle(v.begin(), v.end(), rng);
        }
        const auto seq = nano_lance::RowIdSequence::from_values(v);
        const auto bytes = seq.encode();
        nano_lance::RowIdSequence back;
        std::string error;
        check(nano_lance::RowIdSequence::decode(bytes.data(), bytes.size(), back, error), "random decode " + error);
        check(back.to_vector() == v, "random values " + std::to_string(round));
    }

    // Malformed input is refused, not misread.
    {
        const std::vector<std::uint8_t> truncated = {0x0a, 0x20, 0x0a};
        nano_lance::RowIdSequence s;
        std::string error;
        check(!nano_lance::RowIdSequence::decode(truncated.data(), truncated.size(), s, error), "truncated refused");
        const std::vector<std::uint8_t> backwards = {0x0a, 0x06, 0x0a, 0x04, 0x08, 0x09, 0x10, 0x03};
        check(!nano_lance::RowIdSequence::decode(backwards.data(), backwards.size(), s, error), "end before start refused");
    }

    // Version sequences.
    {
        auto u = nano_lance::RowVersionSequence::uniform(5, 3);
        auto bytes = u.encode();
        nano_lance::RowVersionSequence back;
        std::string error;
        check(nano_lance::RowVersionSequence::decode(bytes.data(), bytes.size(), back, error), "version decode " + error);
        check(back.to_vector() == std::vector<std::uint64_t>(5, 3), "uniform versions");
        const auto mixed = nano_lance::RowVersionSequence::from_values({1, 1, 2, 2, 2, 7});
        bytes = mixed.encode();
        check(nano_lance::RowVersionSequence::decode(bytes.data(), bytes.size(), back, error), "mixed decode");
        check(back.to_vector() == std::vector<std::uint64_t>({1, 1, 2, 2, 2, 7}), "mixed versions");
    }

    if (failures == 0) {
        std::puts("row ids: ok");
    }
    return failures == 0 ? 0 : 1;
}
