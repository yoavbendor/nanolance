// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// encoding/values/rle: runs encoded then decoded back for 1, 2, 4, 8 and 16-byte values: one run, many runs,
// a run of exactly 255, 256 and 1000 (split into 255s), alternating runs and no runs; the exact run-length
// bytes of the split; the index form (u32 little-endian); the decoder's refusals (width 0, a values buffer
// that is not a whole number of values, run values and lengths that disagree); and the run-length definition
// levels, hand-built from the layout the header describes (8 and 16-bit values, 8, 16 and 32-bit lengths), with
// their refusals.

#include "encoding/values/rle.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;
using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;  // (value id, run length)

/// The `width`-byte value with id `id`: its bytes are distinct for distinct ids, and no byte is 0 for
/// width >= 2, so a missed or misplaced byte shows.
Bytes value_of(std::size_t width, std::uint64_t id) {
    Bytes out(width);
    for (std::size_t k = 0; k < width; ++k) {
        out[k] = static_cast<std::uint8_t>(((id + 1U) * 131U + k * 17U) & 0xFFU);
    }
    if (width >= 2U) {
        out[0] = static_cast<std::uint8_t>(id & 0xFFU);
        out[1] = static_cast<std::uint8_t>((id >> 8U) | 0x01U);
    }
    return out;
}

/// The rows the runs stand for, spelled out.
Bytes expanded(std::size_t width, const Runs& runs) {
    Bytes out;
    for (const auto& [id, length] : runs) {
        const auto v = value_of(width, id);
        for (std::uint64_t i = 0; i < length; ++i) {
            out.insert(out.end(), v.begin(), v.end());
        }
    }
    return out;
}

/// Encodes `runs` of `width`-byte values, decodes the buffers and checks the rows come back; returns the
/// encoded run lengths for the caller's exact checks.
Bytes round_trip(const std::string& what, std::size_t width, const Runs& runs) {
    Bytes values;
    Bytes lengths;
    for (const auto& [id, length] : runs) {
        const auto v = value_of(width, id);
        nano_lance::rle::append_run(v, length, values, lengths);
    }
    Bytes out;
    std::string error;
    require(nano_lance::rle::decode(values, lengths, width, out, error), what + ": decode failed: " + error);
    require(out == expanded(width, runs), what + ": round trip gives back the rows");
    return lengths;
}

void test_round_trip_widths() {
    for (const std::size_t width : {1U, 2U, 4U, 8U, 16U}) {
        const std::string w = std::to_string(width) + "-byte";
        round_trip(w + " one run", width, {{7U, 1U}});
        round_trip(w + " one run of 3000", width, {{7U, 3000U}});
        round_trip(w + " many runs", width, {{1U, 5U}, {2U, 1U}, {3U, 40U}, {4U, 1U}, {5U, 999U}});
        round_trip(w + " no runs", width, {});
        round_trip(w + " alternating runs", width, [] {
            Runs r;
            for (std::uint64_t i = 0; i < 400U; ++i) {
                r.emplace_back(i % 2U, 1U + (i % 3U));
            }
            return r;
        }());
    }
}

void test_split_runs() {
    // A run longer than 255 is split into runs of at most 255, each carrying the same value.
    const auto v = value_of(4U, 9U);
    {
        Bytes values;
        Bytes lengths;
        nano_lance::rle::append_run(v, 255U, values, lengths);
        require(lengths == Bytes{255U}, "a run of exactly 255 is one length of 255");
        require(values.size() == 4U, "a run of exactly 255 has one run value");
    }
    {
        Bytes values;
        Bytes lengths;
        nano_lance::rle::append_run(v, 256U, values, lengths);
        require(lengths == (Bytes{255U, 1U}), "a run of 256 splits into 255 and 1");
        const Bytes first(values.begin(), values.begin() + 4);
        const Bytes second(values.begin() + 4, values.end());
        require(values.size() == 8U && first == v && second == v, "a run of 256 repeats its value in both runs");
    }
    {
        Bytes values;
        Bytes lengths;
        nano_lance::rle::append_run(v, 1000U, values, lengths);
        require(lengths == (Bytes{255U, 255U, 255U, 235U}), "a run of 1000 splits into 255, 255, 255 and 235");
    }
    round_trip("a run of 255", 4U, {{9U, 255U}});
    round_trip("a run of 256", 4U, {{9U, 256U}});
    round_trip("a run of 1000", 4U, {{9U, 1000U}});
    round_trip("a run of 1000 next to a short one", 8U, {{1U, 1000U}, {2U, 2U}});
}

void test_many_random_runs() {
    std::mt19937_64 rng(0x5EEDU);
    Runs runs;
    for (std::size_t i = 0; i < 10000U; ++i) {
        runs.emplace_back(rng() % 97U, 1U + rng() % 300U);
    }
    round_trip("10000 random runs of 1 to 300 rows", 2U, runs);
}

void test_index_runs() {
    // Dictionary-RLE indices: each run value is the index as 4 little-endian bytes.
    Bytes values;
    Bytes lengths;
    nano_lance::rle::append_index_run(0x01020304U, 2U, values, lengths);
    nano_lance::rle::append_index_run(0xFFFFFFFFU, 300U, values, lengths);
    const Bytes expected_values = {0x04, 0x03, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    require(values == expected_values, "index runs are little-endian u32 values");
    require(lengths == (Bytes{2U, 255U, 45U}), "index run of 300 splits into 255 and 45");
    Bytes out;
    std::string error;
    require(nano_lance::rle::decode(values, lengths, 4U, out, error), "index runs decode as width 4: " + error);
    require(out.size() == 4U * 302U, "index runs expand to 302 indices");
}

void test_no_runs_and_append() {
    Bytes out = {0xAB, 0xCD};
    std::string error;
    require(nano_lance::rle::decode(Bytes{}, Bytes{}, 4U, out, error), "an empty buffer decodes");
    require(out == (Bytes{0xAB, 0xCD}), "decoding nothing leaves the output as it was");
    require(nano_lance::rle::decode(Bytes{1U}, Bytes{3U}, 1U, out, error), "one run appends: " + error);
    require(out == (Bytes{0xAB, 0xCD, 1U, 1U, 1U}), "a run appends after what the output held");
}

void test_refusals() {
    std::string error;
    Bytes out;
    require(!nano_lance::rle::decode(Bytes(4U, 1U), Bytes{1U}, 0U, out, error), "width 0 is refused");
    require(error == "rle chunk buffer sizes invalid", "width 0 message: " + error);
    require(!nano_lance::rle::decode(Bytes(5U, 1U), Bytes{1U}, 4U, out, error), "a values size of 5 at width 4 is refused");
    require(error == "rle chunk buffer sizes invalid", "a partial value message: " + error);
    require(!nano_lance::rle::decode(Bytes(8U, 1U), Bytes{1U, 2U, 3U}, 4U, out, error),
            "three lengths for two values are refused");
    require(error == "rle run count mismatch between values and lengths", "count mismatch message: " + error);
    require(!nano_lance::rle::decode(Bytes(4U, 1U), Bytes{}, 4U, out, error), "a missing length is refused");
    require(error == "rle run count mismatch between values and lengths", "missing length message: " + error);
}

// Run-length definition levels: [u64 LE values size][run values][run lengths].
Bytes levels_block(const Bytes& run_values, const Bytes& run_lengths) {
    Bytes block(8U, 0U);
    const auto size = static_cast<std::uint64_t>(run_values.size());
    for (std::size_t i = 0; i < 8U; ++i) {
        block[i] = static_cast<std::uint8_t>((size >> (8U * i)) & 0xFFU);
    }
    block.insert(block.end(), run_values.begin(), run_values.end());
    block.insert(block.end(), run_lengths.begin(), run_lengths.end());
    return block;
}

void test_levels() {
    std::uint16_t levels[1100];
    std::string error;

    // 8-bit values, 8-bit lengths: 0 0 0 1 (x255) 0 (x44) -> 3 + 255 + 44 = 302 levels.
    {
        const auto block = levels_block({0U, 1U, 0U}, {3U, 255U, 44U});
        require(nano_lance::rle::decode_levels(block, 8U, 8U, 302U, levels, error), "8/8-bit levels: " + error);
        for (std::size_t i = 0; i < 302U; ++i) {
            const std::uint16_t want = (i < 3U || (i >= 258U)) ? 0U : 1U;
            require(levels[i] == want, "8/8-bit levels: level " + std::to_string(i));
        }
    }
    // 16-bit values, 16-bit lengths: 2 (x1000), 0 (x24) -> 1024 levels.
    {
        const Bytes values = {2U, 0U, 0U, 0U};
        const Bytes lengths = {0xE8U, 0x03U, 0x18U, 0x00U};  // 1000, 24
        require(nano_lance::rle::decode_levels(levels_block(values, lengths), 16U, 16U, 1024U, levels, error),
                "16/16-bit levels: " + error);
        require(levels[0] == 2U && levels[999] == 2U && levels[1000] == 0U && levels[1023] == 0U,
                "16/16-bit levels: runs in place");
    }
    // 8-bit values, 32-bit lengths: 1 (x70000) -> 70000 levels, into a buffer of its own.
    {
        std::vector<std::uint16_t> big(70000U);
        const Bytes lengths = {0x70U, 0x11U, 0x01U, 0x00U};  // 70000
        require(nano_lance::rle::decode_levels(levels_block({1U}, lengths), 8U, 32U, 70000U, big.data(), error),
                "8/32-bit levels: " + error);
        require(big.front() == 1U && big.back() == 1U, "8/32-bit levels: all 70000 set");
    }
    // No runs cover zero levels.
    require(nano_lance::rle::decode_levels(levels_block({}, {}), 8U, 8U, 0U, levels, error),
            "an empty block covers zero levels");
}

void test_level_refusals() {
    std::uint16_t levels[64];
    std::string error;
    require(!nano_lance::rle::decode_levels(levels_block({0U}, {1U}), 4U, 8U, 1U, levels, error),
            "4-bit values are refused");
    require(error == "run-length-encoded definition levels declare unsupported widths (4-bit values, 8-bit run lengths)",
            "unsupported widths message: " + error);
    require(!nano_lance::rle::decode_levels(levels_block({0U}, {1U}), 8U, 64U, 1U, levels, error),
            "64-bit lengths are refused");
    require(!nano_lance::rle::decode_levels(Bytes(7U, 0U), 8U, 8U, 1U, levels, error), "a 7-byte block is refused");
    require(error == "run-length definition-level buffer is too short for its header", "short header message: " + error);
    Bytes past = levels_block({0U, 1U}, {1U, 1U});
    past[0] = 100U;  // the header claims 100 bytes of values
    require(!nano_lance::rle::decode_levels(past, 8U, 8U, 2U, levels, error), "a values size past the block is refused");
    require(error == "run-length definition-level values buffer runs past the end of the block",
            "values past the end message: " + error);
    require(!nano_lance::rle::decode_levels(levels_block({0U, 1U, 2U}, {1U, 1U, 1U}), 16U, 8U, 3U, levels, error),
            "an odd number of bytes for 16-bit values is refused");
    require(error == "run-length definition-level values buffer is not a whole number of values",
            "partial value message: " + error);
    require(!nano_lance::rle::decode_levels(levels_block({0U, 1U}, {1U}), 8U, 8U, 2U, levels, error),
            "a length missing for the second run is refused");
    require(error == "run-length definition-level block has 2 run values but 1 bytes of run lengths",
            "length count message: " + error);
    require(!nano_lance::rle::decode_levels(levels_block({0U, 1U}, {4U, 4U}), 8U, 8U, 5U, levels, error),
            "runs covering more than the chunk are refused");
    require(error == "run-length definition levels cover more than the chunk's 5 values",
            "too many levels message: " + error);
    require(!nano_lance::rle::decode_levels(levels_block({0U, 1U}, {2U, 2U}), 8U, 8U, 9U, levels, error),
            "runs covering fewer than the chunk are refused");
    require(error == "run-length definition levels cover 4 of the chunk's 9 values",
            "too few levels message: " + error);
}

// The encoded bytes of the rows [start, start + rows), counted from the run list by the rule chunk_rows
// documents, independently of chunk_rows: 8-byte header, one (value, length) pair per run piece of at most
// 255 rows, padded to 8.
std::size_t window_bytes_by_hand(const std::vector<std::uint64_t>& lengths, std::size_t value_bytes,
                                 std::size_t start, std::size_t rows) {
    std::size_t pieces = 0;
    std::size_t pos = 0;
    for (const auto length : lengths) {
        const std::size_t a = std::max(pos, start);
        const std::size_t b = std::min<std::size_t>(pos + length, start + rows);
        if (a < b) {
            pieces += (b - a + 254U) / 255U;
        }
        pos += length;
    }
    const std::size_t raw = 8U + pieces * (value_bytes + 1U);
    return (raw + 7U) / 8U * 8U;
}

void test_chunk_rows() {
    using nano_lance::rle::chunk_rows;
    require(chunk_rows({}, 8).empty(), "no runs, no chunks");

    // The shapes pylance 12 writes for a 50,000-row int64 column (measured, W1q): runs of 2 take 1,024 rows
    // per chunk, runs of 50 take 4,096, and the last chunk takes what is left.
    const std::vector<std::uint64_t> two(25000, 2);
    auto rows = chunk_rows(two, 8);
    require(rows.size() == 49U && rows[0] == 1024U && rows[47] == 1024U && rows[48] == 848U,
            "runs of 2 chunk as pylance's 1,024 rows, last 848");
    const std::vector<std::uint64_t> fifty(1000, 50);
    rows = chunk_rows(fifty, 8);
    require(rows.size() == 13U && rows[0] == 4096U && rows[11] == 4096U && rows[12] == 848U,
            "runs of 50 chunk as pylance's 4,096 rows, last 848");
    // The 8192-byte limit at the edge: int64 runs of 4 take 2,048 rows (4,096 rows would encode to 9,224
    // bytes), runs of 5 take 4,096 (7,392 bytes). int32 runs of 3 take 4,096 (6,840 bytes), runs of 2 take 2,048.
    rows = chunk_rows(std::vector<std::uint64_t>(12500, 4), 8);
    require(rows[0] == 2048U, "int64 runs of 4 take 2,048 rows per chunk");
    rows = chunk_rows(std::vector<std::uint64_t>(10000, 5), 8);
    require(rows[0] == 4096U && rows[11] == 4096U, "int64 runs of 5 take 4,096 rows per chunk");
    rows = chunk_rows(std::vector<std::uint64_t>(16667, 3), 4);
    require(rows[0] == 4096U, "int32 runs of 3 take 4,096 rows per chunk");
    rows = chunk_rows(std::vector<std::uint64_t>(25000, 2), 4);
    require(rows[0] == 2048U, "int32 runs of 2 take 2,048 rows per chunk");
    // A run of 1,000 rows is split into 255s in its chunk: one chunk of 1,000 rows, exact count.
    rows = chunk_rows(std::vector<std::uint64_t>{1000U}, 1);
    require(rows.size() == 1U && rows[0] == 1000U, "one long run is one chunk of its rows");

    // Random run lengths and widths: every non-last chunk is a power of two up to 4096 that fits 8192 bytes,
    // and doubling it (capped at what is left) would not fit or would pass 4096. The rows add up.
    std::mt19937 gen(20260501U);
    std::uniform_int_distribution<std::uint64_t> len_dist(1U, 8U);
    for (const std::size_t vb : {std::size_t{1}, std::size_t{4}, std::size_t{8}, std::size_t{16}}) {
        std::vector<std::uint64_t> lengths;
        std::uint64_t total = 0;
        while (total < 60000U) {
            const auto l = len_dist(gen);
            lengths.push_back(l);
            total += l;
        }
        // A few long runs too, so pieces of 255 are exercised.
        lengths.push_back(700U);
        lengths.push_back(255U);
        total += 955U;
        const auto chunks = chunk_rows(lengths, vb);
        std::size_t start = 0;
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const std::size_t p = chunks[i];
            sum += p;
            const bool last = i + 1U == chunks.size();
            require(window_bytes_by_hand(lengths, vb, start, p) <= 8192U, "a chunk fits 8192 bytes");
            if (!last) {
                require(p <= 4096U && (p & (p - 1U)) == 0U, "a non-last chunk is a power of two up to 4096");
                const std::size_t remaining = static_cast<std::size_t>(total) - start;
                const std::size_t doubled = std::min<std::size_t>(2U * p, remaining);
                require(p == 4096U || window_bytes_by_hand(lengths, vb, start, doubled) > 8192U,
                        "a chunk is the largest power of two that fits");
            }
            start += p;
        }
        require(sum == total && start == total, "the chunks add up to the rows");
    }
}

}  // namespace

int main() {
    test_chunk_rows();
    test_round_trip_widths();
    test_split_runs();
    test_many_random_runs();
    test_index_runs();
    test_no_runs_and_append();
    test_refusals();
    test_levels();
    test_level_refusals();
    std::cout << "encoding/values/rle: all checks passed\n";
    return 0;
}
