// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// encoding_choice: the commit-time heuristics that decide whether bitpacking, RLE, a constant, a
// dictionary or dictionary + RLE pays for a buffered column. Each decision is checked at and around its
// threshold (the 1024-value bitpack chunk width word, the n/2 run cutoff, the 32760-byte one-chunk run
// buffer, the 100-row and rows/2 dictionary limits, the 0.8 size ratio) and on degenerate input: no rows,
// one row, an all-null column's buffers, a size that is not a whole number of values.

#include "encoding/encoding_choice.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace ec = nano_lance::encoding_choice;
using nano_lance::ColumnValues;

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

template <class T>
ColumnValues fixed_column(const std::vector<T>& values) {
    ColumnValues cv;
    cv.fixed.resize(values.size() * sizeof(T));
    if (!values.empty()) {
        std::memcpy(cv.fixed.data(), values.data(), cv.fixed.size());
    }
    cv.rows = values.size();
    return cv;
}

ColumnValues variable_column(const std::vector<std::string>& rows, bool large = false) {
    ColumnValues cv;
    cv.kind = ColumnValues::Kind::VariableWidth;
    cv.variable.large = large;
    cv.rows = rows.size();
    const std::size_t ow = large ? 8U : 4U;
    std::int64_t end = 0;
    auto push = [&](std::int64_t off) {
        const std::size_t at = cv.variable.offsets.size();
        cv.variable.offsets.resize(at + ow);
        if (large) {
            std::memcpy(cv.variable.offsets.data() + at, &off, 8);
        } else {
            const auto o32 = static_cast<std::int32_t>(off);
            std::memcpy(cv.variable.offsets.data() + at, &o32, 4);
        }
    };
    push(0);
    for (const auto& r : rows) {
        cv.variable.data.insert(cv.variable.data.end(), r.begin(), r.end());
        end += static_cast<std::int64_t>(r.size());
        push(end);
    }
    return cv;
}

// `distinct` values cycled over `rows` rows, each `len` bytes.
std::vector<std::string> cycled(std::size_t rows, std::size_t distinct, std::size_t len) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < rows; ++i) {
        std::string v = std::to_string(i % distinct);
        v.resize(len, 'x');
        out.push_back(v);
    }
    return out;
}

void test_bitpack() {
    // 1024 u8 values: a chunk costs 1 + 1024*width/8 words; width 7 fits under 1024 bytes, width 8 does not.
    std::vector<std::uint8_t> v(1024, 0x7F);
    require(ec::bitpack_beats_flat(fixed_column(v), 1), "u8 width 7 beats flat");
    v[5] = 0xFF;
    require(!ec::bitpack_beats_flat(fixed_column(v), 1), "u8 width 8 does not beat flat");

    // u64: width 63 -> 1009 words (8072 B) < 8192; width 64 -> 1025 words > 8192.
    std::vector<std::uint64_t> w(1024, (1ULL << 63U) - 1U);
    require(ec::bitpack_beats_flat(fixed_column(w), 8), "u64 width 63 beats flat");
    w[0] = ~0ULL;
    require(!ec::bitpack_beats_flat(fixed_column(w), 8), "u64 width 64 does not beat flat");

    // Chunks are costed on their own maximum: one wide chunk among narrow ones only costs that chunk.
    std::vector<std::uint32_t> x(2048, 1U);
    require(ec::bitpack_beats_flat(fixed_column(x), 4), "narrow u32 columns beat flat");
    for (std::size_t i = 0; i < 1024; ++i) {
        x[i] = 0xFFFFFFFFU;
    }
    require(ec::bitpack_beats_flat(fixed_column(x), 4), "a full-width chunk beside a narrow one still wins");
    for (auto& e : x) {
        e = 0xFFFFFFFFU;
    }
    require(!ec::bitpack_beats_flat(fixed_column(x), 4), "all full-width chunks lose");

    // A partial last chunk is still costed with its width word.
    std::vector<std::uint16_t> p(10, 1U);
    require(!ec::bitpack_beats_flat(fixed_column(p), 2), "10 u16 values: 1 + 64 words, not smaller than 10");

    // Degenerate input is left as tagged.
    require(ec::bitpack_beats_flat(fixed_column(std::vector<std::uint32_t>{}), 4), "empty column: not our call");
    require(ec::bitpack_beats_flat(fixed_column(std::vector<std::uint8_t>(7, 0)), 4), "size not a multiple of bpv");
    require(ec::bitpack_beats_flat(fixed_column(std::vector<std::uint8_t>(8, 0)), 0), "bpv 0");
    require(!ec::bitpack_beats_flat(fixed_column(std::vector<std::uint64_t>{0}), 8),
            "one u64: width word + 0 words = 8 B, not < 8");

    // A borrowed buffer is read through fixed_data()/fixed_size().
    std::vector<std::uint8_t> borrowed(1024, 1);
    ColumnValues b;
    b.fixed_borrowed = borrowed.data();
    b.fixed_borrowed_size = borrowed.size();
    require(ec::bitpack_beats_flat(b, 1), "borrowed buffer is costed");
}

// `runs` runs of `len` values alternating 0/1.
// Full-width bytes (0x80 / 0xFF), so InlineBitpacking cannot beat the runs (Lance compares RLE with it).
std::vector<std::uint8_t> alternating_runs(std::size_t runs, std::size_t len) {
    std::vector<std::uint8_t> out;
    for (std::size_t r = 0; r < runs; ++r) {
        out.insert(out.end(), len, static_cast<std::uint8_t>(r % 2U == 0U ? 0x80U : 0xFFU));
    }
    return out;
}

void test_fixed_rle() {
    // The cutoff: rejected when split runs >= ceil(n/2).
    auto cv = fixed_column(std::vector<std::uint32_t>{7, 7, 7, 7});
    require(ec::fixed_column_rle_plan(cv, 4), "one run of 4 is RLE");
    require(cv.fixed_rle_plan.computed && cv.fixed_rle_plan.runs.size() == 1U, "plan has the one run");
    require(cv.fixed_rle_plan.runs[0].first == 0U && cv.fixed_rle_plan.runs[0].second == 4U, "run is (0, 4)");

    cv = fixed_column(std::vector<std::uint32_t>{1, 1, 2, 2});
    // Pylance's size model (W1q): a 4-byte value and its length cost 5 bytes a run, so 2 runs (10 bytes) beat
    // 4 rows of 4 bytes (16); 4 runs (20 bytes) do not.
    require(ec::fixed_column_rle_plan(cv, 4), "2 runs in 4 rows: 10 < 16 bytes is RLE");
    cv = fixed_column(std::vector<std::uint32_t>{1, 2, 3, 4});
    require(!ec::fixed_column_rle_plan(cv, 4), "4 runs in 4 rows: 20 >= 16 bytes rejects");
    require(!cv.fixed_rle_plan.computed && cv.fixed_rle_plan.runs.empty(), "rejected plan is empty");

    cv = fixed_column(std::vector<std::uint32_t>{1, 1, 1, 2, 2});
    require(ec::fixed_column_rle_plan(cv, 4), "2 runs in 5 rows: 2 < 3 accepts");
    require(cv.fixed_rle_plan.runs.size() == 2U && cv.fixed_rle_plan.runs[1].first == 3U &&
                cv.fixed_rle_plan.runs[1].second == 2U,
            "runs are (0,3) and (3,2)");

    // A stale plan is cleared even when the verdict is no.
    cv.fixed_rle_plan.computed = true;
    cv.fixed_rle_plan.runs.emplace_back(0, 1);
    cv.fixed = {1, 0, 0, 0, 2, 0, 0, 0};
    require(!ec::fixed_column_rle_plan(cv, 4) && !cv.fixed_rle_plan.computed && cv.fixed_rle_plan.runs.empty(),
            "stale plan cleared on a no");

    // Degenerate.
    cv = fixed_column(std::vector<std::uint32_t>{});
    require(!ec::fixed_column_rle_plan(cv, 4), "0 rows");
    cv = fixed_column(std::vector<std::uint32_t>{9});
    require(!ec::fixed_column_rle_plan(cv, 4), "1 row: 1 >= 1 rejects");
    cv = fixed_column(std::vector<std::uint8_t>(6, 0));
    require(!ec::fixed_column_rle_plan(cv, 0), "bpv 0");
    require(!ec::fixed_column_rle_plan(cv, 4), "size not a multiple of bpv");
    require(!ec::fixed_column_rle_plan(cv, 3), "bpv 3 is not a bitpackable width");
    // Lance's rule: RLE only when it also beats InlineBitpacking's estimate. All zeros pack to one word a block.
    cv = fixed_column(std::vector<std::uint8_t>(10, 0));
    require(!ec::fixed_column_rle_plan(cv, 1) && !ec::fixed_column_rle_plan(cv, 2),
            "all-zero buffer bitpacks smaller than its runs");
    // Two runs of full-width bytes: a 1,024-value block would be larger than the 10 raw bytes, so no bitpacking
    // estimate, and the runs (4 bytes at 1 byte, 6 at 2) beat raw.
    cv = fixed_column(std::vector<std::uint8_t>{255, 255, 255, 255, 255, 128, 128, 128, 128, 128});
    require(ec::fixed_column_rle_plan(cv, 1) && ec::fixed_column_rle_plan(cv, 2),
            "two runs of full-width values are RLE at 1 and 2 bytes");

    // A run over 255 counts as ceil(L/255) split runs but is recorded whole.
    cv = fixed_column(std::vector<std::uint64_t>(1000, 5));
    require(ec::fixed_column_rle_plan(cv, 8), "one long run");
    require(cv.fixed_rle_plan.runs.size() == 1U && cv.fixed_rle_plan.runs[0].second == 1000U, "run recorded unsplit");
    cv = fixed_column(alternating_runs(2, 256));
    require(ec::fixed_column_rle_plan(cv, 1), "2 runs of 256");

    // Runs no longer have to fit one chunk (W1q): the writer splits them. The size model still applies, so
    // 16365 u8 runs of 3 (2 bytes a run against 3 bytes a row) are RLE, and runs of 1 are not.
    cv = fixed_column(alternating_runs(16365, 3));
    require(ec::fixed_column_rle_plan(cv, 1), "16365 u8 runs of 3 are RLE across chunks");
    cv = fixed_column(alternating_runs(16365, 1));
    require(!ec::fixed_column_rle_plan(cv, 1), "u8 runs of 1 are not RLE");

    // A big scattered column fails (through the prefix sample); a big run column passes.
    std::vector<std::uint32_t> scattered(20000);
    for (std::size_t i = 0; i < scattered.size(); ++i) {
        scattered[i] = static_cast<std::uint32_t>(i);
    }
    cv = fixed_column(scattered);
    require(!ec::fixed_column_rle_plan(cv, 4), "scattered large column");
    cv = fixed_column(std::vector<std::uint32_t>(20000, 3));
    require(ec::fixed_column_rle_plan(cv, 4), "constant large column");
}

void test_constant() {
    std::vector<std::uint8_t> out{9, 9};
    auto cv = variable_column({"abc", "abc", "abc"});
    require(ec::variable_column_constant_value(cv, out), "all rows equal");
    require(std::string(out.begin(), out.end()) == "abc", "the constant is returned");

    cv = variable_column({"abc", "abc", "abd"});
    require(!ec::variable_column_constant_value(cv, out), "last row differs");
    cv = variable_column({"abc", "abcd"});
    require(!ec::variable_column_constant_value(cv, out), "a longer row differs");
    cv = variable_column({"", "", ""});
    require(ec::variable_column_constant_value(cv, out) && out.empty(), "all-empty rows are the constant empty value");
    cv = variable_column({"only"});
    require(ec::variable_column_constant_value(cv, out) && std::string(out.begin(), out.end()) == "only", "1 row");
    cv = variable_column({});
    require(!ec::variable_column_constant_value(cv, out), "0 rows");
    cv = variable_column({"zz", "zz"}, true);
    require(ec::variable_column_constant_value(cv, out), "64-bit offsets");

    // Offsets that run past the data buffer are refused rather than read.
    cv = variable_column({"abc", "abc"});
    cv.variable.data.resize(2);
    require(!ec::variable_column_constant_value(cv, out), "first row outside the data");
    // An all-null column buffers empty values.
    cv = variable_column({"", ""});
    cv.validity = {0x00};
    cv.null_count = 2;
    require(ec::variable_column_constant_value(cv, out), "all-null column buffers one empty value");
}

void test_dict_rle() {
    auto cv = variable_column({"a", "a", "a", "b", "b", "b"});
    require(ec::variable_column_dict_rle_beneficial(cv), "2 runs in 6 rows");
    require(cv.structural_dict_rle_plan.computed && cv.structural_dict_rle_plan.distinct.size() == 2U, "2 distinct");
    require(cv.structural_dict_rle_plan.runs.size() == 2U && cv.structural_dict_rle_plan.runs[0].second == 3U,
            "runs of 3");

    cv = variable_column({"a", "a", "b", "b", "a", "a"});
    require(!ec::variable_column_dict_rle_beneficial(cv), "3 runs in 6 rows: 6 >= 6 rejects");
    require(!cv.structural_dict_rle_plan.computed, "no plan on a no");

    cv = variable_column({"a", "a", "a", "b", "b", "b", "a", "a", "a"});
    require(ec::variable_column_dict_rle_beneficial(cv), "3 runs in 9 rows");
    const auto& runs = cv.structural_dict_rle_plan.runs;
    require(cv.structural_dict_rle_plan.distinct.size() == 2U && runs.size() == 3U && runs[0].first == 0U &&
                runs[1].first == 1U && runs[2].first == 0U,
            "a repeated value reuses its dictionary id");

    cv = variable_column({"a", "a", "a", "b"});
    require(!ec::variable_column_dict_rle_beneficial(cv), "2 runs in 4 rows");
    cv = variable_column({"a", "a"});
    require(!ec::variable_column_dict_rle_beneficial(cv), "1 run in 2 rows: 2 >= 2 rejects");
    cv = variable_column({"a", "a", "a"});
    require(ec::variable_column_dict_rle_beneficial(cv), "1 run in 3 rows");
    cv = variable_column({"a"});
    require(!ec::variable_column_dict_rle_beneficial(cv), "1 row");
    cv = variable_column({});
    require(!ec::variable_column_dict_rle_beneficial(cv), "0 rows");
    cv = variable_column({"", "", ""}, true);
    require(ec::variable_column_dict_rle_beneficial(cv), "empty values, 64-bit offsets");

    // A run over 255 is recorded whole, though it costs two split runs.
    cv = variable_column(std::vector<std::string>(300, "r"));
    require(ec::variable_column_dict_rle_beneficial(cv) && cv.structural_dict_rle_plan.runs[0].second == 300U,
            "long run");

    // One-chunk run buffer: split * 5 + 32 <= 32760, so 6545 runs pass and 6546 do not.
    auto many_runs = [](std::size_t runs) {
        std::vector<std::string> rows;
        for (std::size_t r = 0; r < runs; ++r) {
            rows.insert(rows.end(), 3, r % 2U == 0U ? "a" : "b");
        }
        return rows;
    };
    cv = variable_column(many_runs(6545));
    require(ec::variable_column_dict_rle_beneficial(cv), "6545 runs fit the chunk");
    cv = variable_column(many_runs(6546));
    require(!ec::variable_column_dict_rle_beneficial(cv), "6546 runs overflow the chunk");

    // Offsets past the data are refused.
    cv = variable_column({"aaa", "aaa", "aaa"});
    cv.variable.data.resize(4);
    require(!ec::variable_column_dict_rle_beneficial(cv), "offsets outside the data");
}

void test_dict() {
    // Under 100 rows is never a dictionary; 100 low-cardinality rows are.
    auto cv = variable_column(cycled(99, 2, 10));
    require(!ec::variable_column_dict_beneficial(cv), "99 rows");
    cv = variable_column(cycled(100, 2, 10));
    require(ec::variable_column_dict_beneficial(cv), "100 rows, 2 distinct");
    require(cv.structural_dict_plan.computed && cv.structural_dict_plan.distinct.size() == 2U &&
                cv.structural_dict_plan.indices.size() == 100U,
            "plan has 2 distinct values and 100 indices");
    require(cv.structural_dict_plan.indices[0] == 0U && cv.structural_dict_plan.indices[1] == 1U &&
                cv.structural_dict_plan.indices[2] == 0U,
            "indices follow first appearance");
    cv = variable_column(cycled(100, 2, 10), true);
    require(ec::variable_column_dict_beneficial(cv), "64-bit offsets");

    // Cardinality cutoff: distinct > rows/2 rejects (long values so the size ratio is not the reason).
    cv = variable_column(cycled(100, 50, 100));
    require(ec::variable_column_dict_beneficial(cv), "50 distinct in 100 rows");
    cv = variable_column(cycled(100, 51, 100));
    require(!ec::variable_column_dict_beneficial(cv), "51 distinct in 100 rows");
    require(!cv.structural_dict_plan.computed, "no plan on a no");

    // Size ratio: 1-byte values cost more as indices than they save.
    cv = variable_column(cycled(100, 2, 1));
    require(!ec::variable_column_dict_beneficial(cv), "1-byte values do not pay");

    // Degenerate.
    cv = variable_column({});
    require(!ec::variable_column_dict_beneficial(cv), "0 rows");
    cv = variable_column({"x"});
    require(!ec::variable_column_dict_beneficial(cv), "1 row");
    cv = variable_column(std::vector<std::string>(200, ""));
    require(!ec::variable_column_dict_beneficial(cv), "200 empty rows: no bytes to save");
    cv = variable_column(cycled(100, 2, 10));
    cv.variable.data.resize(5);
    require(!ec::variable_column_dict_beneficial(cv), "offsets outside the data");

    // Large columns go through the prefix sample: unique values fail, 3 distinct values pass.
    cv = variable_column(cycled(20000, 20000, 12));
    require(!ec::variable_column_dict_beneficial(cv), "20000 unique values");
    cv = variable_column(cycled(20000, 3, 12));
    require(ec::variable_column_dict_beneficial(cv), "20000 rows, 3 distinct");
    require(cv.structural_dict_plan.indices.size() == 20000U, "all rows indexed");
}

}  // namespace

/// `rows` 64-bit values from `distinct` 40-bit values, in a scrambled order (a shuffled column).
std::vector<std::uint64_t> shuffled_64(std::size_t rows, std::size_t distinct) {
    std::vector<std::uint64_t> out(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        const std::uint64_t k = (static_cast<std::uint64_t>(i) * 2654435761ULL) % distinct;
        out[i] = (k * 0x9E3779B97F4A7C15ULL) >> 24;  // 40-bit values
    }
    return out;
}

/// Lance's should_dictionary_encode and its dictionary-values choice (W1p), at the cases pylance 12 was measured on.
void test_fixed_dict() {
    using FixedDictValues = nano_lance::StructuralDictPlan::FixedDictValues;
    auto cv = fixed_column(shuffled_64(99, 2));
    require(!ec::fixed_column_dict_beneficial(cv), "99 rows");

    cv = fixed_column(shuffled_64(100, 2));
    require(ec::fixed_column_dict_beneficial(cv), "100 rows, 2 distinct");
    require(cv.structural_dict_plan.fixed && cv.structural_dict_plan.fixed_distinct.size() == 2U,
            "plan holds 2 values");
    require(cv.structural_dict_plan.fixed_values == FixedDictValues::kLz4,
            "100 rows of 2 values: a 1,024-value block is larger than the raw block, so LZ4");

    cv = fixed_column(shuffled_64(50000, 10));
    require(ec::fixed_column_dict_beneficial(cv) && cv.structural_dict_plan.fixed_distinct.size() == 10U,
            "50,000 rows, 10 distinct");
    require(cv.structural_dict_plan.fixed_values == FixedDictValues::kLz4, "10 values: LZ4");

    cv = fixed_column(shuffled_64(50000, 1000));
    require(ec::fixed_column_dict_beneficial(cv), "50,000 rows, 1,000 distinct");
    require(cv.structural_dict_plan.fixed_values == FixedDictValues::kInlineBitpacking,
            "1,000 x 40-bit: 5,120 bytes of blocks against 8,000 raw, so InlineBitpacking");

    cv = fixed_column(shuffled_64(50000, 2000));
    require(ec::fixed_column_dict_beneficial(cv), "50,000 rows, 2,000 distinct");
    require(cv.structural_dict_plan.fixed_values == FixedDictValues::kOutOfLineBitpacking &&
                cv.structural_dict_plan.fixed_width == 40U,
            "more than one block: OutOfLineBitpacking at the 40-bit width");

    // Lance's sample starts block b at b * rows / 32: with 5,000 distinct values of period 5,000 its blocks 16..31
    // repeat 0..15, the ratio halves and pylance 12 takes the dictionary (measured); 6,007 has no such repeat.
    cv = fixed_column(shuffled_64(50000, 5000));
    require(ec::fixed_column_dict_beneficial(cv) && cv.structural_dict_plan.fixed_distinct.size() == 5000U,
            "5,000 distinct of period 5,000: the sample repeats, a dictionary as pylance");
    cv = fixed_column(shuffled_64(50000, 6007));
    require(!ec::fixed_column_dict_beneficial(cv), "6,007 distinct: the sample projects past rows / 2");

    // 128-bit values (decimal128, fixed_size_binary(16)): the same decision at 16 bytes an entry, the dictionary
    // never bitpacked (LZ4 by default), 2 words an entry in the plan.
    {
        std::vector<std::uint64_t> words;
        for (std::size_t i = 0; i < 50000U; ++i) {
            words.push_back((i * 7919U) % 20U);
            words.push_back(0x1234U);
        }
        cv = fixed_column(words);
        require(ec::fixed_column_dict_beneficial(cv, {}, 16U) && cv.structural_dict_plan.fixed_bytes == 16U &&
                    cv.structural_dict_plan.fixed_distinct.size() == 40U &&
                    cv.structural_dict_plan.fixed_values == FixedDictValues::kLz4,
                "128-bit, 20 distinct: a dictionary, LZ4");
        std::vector<std::uint64_t> unique;
        for (std::size_t i = 0; i < 50000U; ++i) {
            unique.push_back(i);
            unique.push_back(0U);
        }
        cv = fixed_column(unique);
        require(!ec::fixed_column_dict_beneficial(cv, {}, 16U), "128-bit, all distinct: no dictionary");
    }

    cv = fixed_column(shuffled_64(1000000, 1000));
    require(!ec::fixed_column_dict_beneficial(cv), "1,000,000 rows, 1,000 distinct: projected past 100,000");

    // Nulls (W1p3): every slot's raw value is hashed, as Lance does, so a value under the nulls that no valid row
    // holds is one more entry, and one that a valid row holds adds none.
    auto with_nulls = shuffled_64(50000, 1000);
    for (std::size_t i = 0; i < with_nulls.size(); i += 19U) {
        with_nulls[i] = 0xFFFFFFFFFFFFULL;  // 48 bits: not among the 40-bit shuffled values
    }
    cv = fixed_column(with_nulls);
    cv.null_count = (with_nulls.size() + 18U) / 19U;
    require(ec::fixed_column_dict_beneficial(cv) && cv.structural_dict_plan.fixed_distinct.size() == 1001U,
            "nulls over a fresh value: 1,000 values plus the one under the nulls");
    with_nulls = shuffled_64(50000, 1000);
    const auto existing = with_nulls[1];
    for (std::size_t i = 0; i < with_nulls.size(); i += 19U) {
        with_nulls[i] = existing;
    }
    cv = fixed_column(with_nulls);
    cv.null_count = (with_nulls.size() + 18U) / 19U;
    require(ec::fixed_column_dict_beneficial(cv) && cv.structural_dict_plan.fixed_distinct.size() == 1000U,
            "nulls over an existing value: no extra entry");

    cv = fixed_column(shuffled_64(50000, 1));
    require(!ec::fixed_column_dict_beneficial(cv), "a constant column is not a dictionary");
}

/// W1p2: the indices of a dictionary are RLE'd when they have runs (u32 values, runs * 5 < rows * 4), and bitpacked
/// otherwise. A sorted column of 1,000 values is runs of 50; a shuffled one has no runs.
void test_dict_index_rle() {
    auto sorted = shuffled_64(1000000, 1000);
    std::sort(sorted.begin(), sorted.end());
    auto cv = fixed_column(sorted);
    require(ec::fixed_column_dict_beneficial(cv), "sorted 1,000,000 rows, 1,000 distinct: a dictionary");
    require(cv.structural_dict_plan.index_rle, "sorted: the indices are RLE'd");
    require(cv.structural_dict_plan.index_runs.size() == 1000U, "one run per distinct value");
    std::uint64_t covered = 0;
    for (const auto& run : cv.structural_dict_plan.index_runs) {
        covered += run.second;
    }
    require(covered == 1000000U, "the runs cover every row");

    cv = fixed_column(shuffled_64(50000, 1000));
    require(ec::fixed_column_dict_beneficial(cv), "shuffled 50,000 rows, 1,000 distinct: a dictionary");
    require(!cv.structural_dict_plan.index_rle && cv.structural_dict_plan.index_runs.empty(),
            "shuffled: the indices are bitpacked, no runs");
}

int main() {
    test_bitpack();
    test_fixed_rle();
    test_constant();
    test_dict_rle();
    test_dict();
    test_fixed_dict();
    test_dict_index_rle();
    std::cout << "encoding_choice: ok\n";
    return 0;
}
