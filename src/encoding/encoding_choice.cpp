// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Choosing a column's structural encoding at commit (RLE, bitpacking, constant, dictionary,
// dictionary + RLE): scans of the buffered values that decide whether an encoding pays.

#include "encoding/encoding_choice.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nano_lance::encoding_choice {

// One RLE scan pass over `n` fixed-width values of size sizeof(T) at `data`. With `runs_out == nullptr`
// it only counts split runs (Lance caps a run at 255, so a run of length L counts as ceil(L/255)) and
// rejects as soon as split_runs reaches `reject_at` -- zero allocation, single inlined
// load-and-compare per row instead of a libc memcmp call per row (which dominated the write profile
// for scattered integer columns). With `runs_out` set it records one (row index, run length) pair per
// run and never rejects (only already-accepted columns take that pass).
template <class T>
bool typed_rle_scan(const std::uint8_t* data, std::size_t n, std::size_t reject_at,
                    std::size_t& split_runs_out,
                    std::vector<std::pair<std::size_t, std::uint64_t>>* runs_out) {
    std::size_t split_runs = 0;
    std::size_t i = 0;
    while (i < n) {
        T head;
        std::memcpy(&head, data + i * sizeof(T), sizeof(T));
        std::size_t run = 1;
        while (i + run < n) {
            T next;
            std::memcpy(&next, data + (i + run) * sizeof(T), sizeof(T));
            if (next != head) {
                break;
            }
            ++run;
        }
        split_runs += (run + 254U) / 255U;
        if (runs_out == nullptr && split_runs >= reject_at) {
            return false;  // not run-friendly; counting pass bails the moment the verdict is decided
        }
        if (runs_out != nullptr) {
            runs_out->emplace_back(i, run);
        }
        i += run;
    }
    split_runs_out = split_runs;
    return true;
}

bool dispatch_rle_scan(const std::uint8_t* data, std::size_t n, std::size_t bpv, std::size_t reject_at,
                       std::size_t& split_runs_out,
                       std::vector<std::pair<std::size_t, std::uint64_t>>* runs_out) {
    switch (bpv) {
        case 1U:
            return typed_rle_scan<std::uint8_t>(data, n, reject_at, split_runs_out, runs_out);
        case 2U:
            return typed_rle_scan<std::uint16_t>(data, n, reject_at, split_runs_out, runs_out);
        case 4U:
            return typed_rle_scan<std::uint32_t>(data, n, reject_at, split_runs_out, runs_out);
        case 8U:
            return typed_rle_scan<std::uint64_t>(data, n, reject_at, split_runs_out, runs_out);
        default:
            return false;  // caller gates on bitpackable integer types, so bpv is always 1/2/4/8
    }
}

template <class T>
std::size_t bitpack_words_for_column(const std::uint8_t* data, std::size_t n) {
    // One FastLanes chunk per 1024 values; each chunk stores its own width, so each is costed on its
    // own maximum.
    constexpr std::size_t kBits = sizeof(T) * 8U;
    std::size_t total_words = 0;
    for (std::size_t base = 0; base < n; base += 1024U) {
        const std::size_t count = std::min<std::size_t>(1024U, n - base);
        T bits_or = 0;
        for (std::size_t i = 0; i < count; ++i) {
            T v = 0;
            std::memcpy(&v, data + (base + i) * sizeof(T), sizeof(T));
            bits_or = static_cast<T>(bits_or | v);
        }
        std::size_t width = 0;
        while (bits_or != 0U) {
            ++width;
            bits_or = static_cast<T>(bits_or >> 1U);
        }
        total_words += 1U + (1024U * width) / kBits;  // one inline width word + the packed words
    }
    return total_words;
}

/// Is bitpacking actually smaller than storing the values flat?
///
/// This mirrors Lance's own rule (`rust/lance-encoding/src/compression.rs`: a bitpacked block is
/// only offered when its estimated size is strictly below `raw_bytes`) rather than inventing a
/// threshold. Costing it per 1024-value chunk is what makes the answer right: each chunk carries its
/// own width word, so a column that needs the full width is strictly LARGER bitpacked than flat, and
/// pays a FastLanes transpose on every read for it.
///
/// nanolance used to bitpack every integer column unconditionally. On a column of random `uint64`
/// ids -- the `high_card` bench shape, and any hash/uuid column -- that made the file 2.4% bigger and
/// the read 0.85 ms instead of 0.50 ms. Stock Lance writes `Flat(64)` for the same data.
bool bitpack_beats_flat(const nano_lance::ColumnValues& cv, std::size_t bpv) {
    if (bpv == 0U || cv.fixed_size() == 0U || cv.fixed_size() % bpv != 0U) {
        return true;  // not our call to make; leave the column as it was tagged
    }
    const std::size_t n = cv.fixed_size() / bpv;
    const auto* data = cv.fixed_data();
    std::size_t words = 0;
    switch (bpv) {
        case 1U: words = bitpack_words_for_column<std::uint8_t>(data, n); break;
        case 2U: words = bitpack_words_for_column<std::uint16_t>(data, n); break;
        case 4U: words = bitpack_words_for_column<std::uint32_t>(data, n); break;
        default: words = bitpack_words_for_column<std::uint64_t>(data, n); break;
    }
    return words * bpv < cv.fixed_size();
}

// Decide whether RLE beats bitpacking for a fixed-width column. Lance requires 8-bit run lengths, so
// runs longer than 255 are split into <=255 sub-runs; we count those split runs. Two passes (same
// structure as variable_column_dict_rle_beneficial): pass 1 counts only, with the reject-early exit
// and zero allocation -- the common case is a scattered column that fails, and the previous version
// recorded ~n/2 (row, run) pairs into a vector before bailing, all thrown away. Pass 2 records the
// runs for the encoder's plan and only runs for genuinely beneficial columns.
bool fixed_column_rle_plan(nano_lance::ColumnValues& cv, std::size_t bpv, const ColumnEncodingOptions& options) {
    cv.fixed_rle_plan = {};
    if (bpv == 0U || cv.fixed_size() == 0U || cv.fixed_size() % bpv != 0U) {
        return false;
    }
    // `lance-encoding:rle-threshold=0`: RLE is off for this column (pylance takes RLE only below the threshold).
    if (options.rle_set && options.rle_threshold <= 0.0) {
        return false;
    }
    const std::size_t n = cv.fixed_size() / bpv;
    // The run cutoff: nanolance's default is ceil(n/2) (a run ratio below one half). An explicit
    // rle-threshold t takes RLE only when split_runs < n * t, pylance's guard, so the cutoff is ceil(n * t).
    const std::size_t reject_at =
        options.rle_set ? static_cast<std::size_t>(std::ceil(static_cast<double>(n) * options.rle_threshold))
                        : (n + 1U) / 2U;
    // Prefix-sample pre-check (same pattern as variable_column_dict_beneficial's sampling): even with
    // the reject-early exit, a scattered column scans ~n/2 rows before failing (split_runs grows one
    // per row, crossing n/2 halfway through). A 4096-row prefix predicts that verdict at ~1/25 the
    // cost. Reject only when the sample DECISIVELY fails -- split_runs over 3/4 of the sample, a 1.5x
    // margin above the real n/2 cutoff -- to keep false negatives rare; the cost of one is a valid,
    // slightly larger bitpack fallback, never wrong data.
    constexpr std::size_t kSampleRows = 4096U;
    if (n > kSampleRows * 4U) {
        // The sample's own cutoff: the same 1.5x margin above the real cutoff, capped at the sample size.
        const std::size_t sample_reject =
            options.rle_set ? std::min<std::size_t>(
                                  kSampleRows, static_cast<std::size_t>(std::ceil(
                                                   static_cast<double>(kSampleRows) * options.rle_threshold * 1.5)))
                            : kSampleRows * 3U / 4U;
        std::size_t sample_split_runs = 0;
        if (!dispatch_rle_scan(cv.fixed_data(), kSampleRows, bpv, sample_reject, sample_split_runs, nullptr)) {
            return false;
        }
    }
    std::size_t split_runs = 0;
    // reject_at == ceil(n/2) is exactly the previous `split_runs * 2 >= n` cutoff.
    if (!dispatch_rle_scan(cv.fixed_data(), n, bpv, reject_at, split_runs, nullptr)) {
        return false;
    }
    // One chunk for the whole column: run buffers must fit the miniblock (12-bit word => 32760 bytes).
    const std::size_t values_size = split_runs * bpv;
    const std::size_t lengths_size = split_runs;  // 1 byte each
    if ((values_size + lengths_size + 32U) > 32760U) {
        return false;
    }
    std::vector<std::pair<std::size_t, std::uint64_t>> runs;
    std::size_t split_runs_again = 0;
    if (!dispatch_rle_scan(cv.fixed_data(), n, bpv, n + 1U, split_runs_again, &runs)) {
        return false;
    }
    cv.fixed_rle_plan.computed = true;
    cv.fixed_rle_plan.runs = std::move(runs);
    return true;
}

// True if every row of a variable-width column is identical; returns that value in `value_out`.
bool variable_column_constant_value(const nano_lance::ColumnValues& cv, std::vector<std::uint8_t>& value_out) {
    const std::size_t ow = cv.variable.large ? 8U : 4U;
    if (cv.variable.offsets.size() < 2U * ow) {
        return false;
    }
    auto read_offset = [&](std::size_t index) -> std::int64_t {
        const auto* p = cv.variable.offsets.data() + index * ow;
        if (cv.variable.large) {
            std::int64_t v = 0;
            std::memcpy(&v, p, 8);
            return v;
        }
        std::int32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    };
    const std::size_t rows = cv.variable.offsets.size() / ow - 1U;
    if (rows == 0U) {
        return false;
    }
    const auto start0 = read_offset(0);
    const auto end0 = read_offset(1);
    if (start0 < 0 || end0 < start0 || static_cast<std::size_t>(end0) > cv.variable.data.size()) {
        return false;
    }
    const auto len = static_cast<std::size_t>(end0 - start0);
    for (std::size_t i = 1; i < rows; ++i) {
        const auto s = read_offset(i);
        const auto e = read_offset(i + 1);
        if (e - s != static_cast<std::int64_t>(len) ||
            (len != 0U && std::memcmp(cv.variable.data.data() + s, cv.variable.data.data() + start0, len) != 0)) {
            return false;
        }
    }
    value_out.assign(cv.variable.data.begin() + start0, cv.variable.data.begin() + end0);
    return true;
}

// Decide whether dictionary + RLE wins for a variable-width column, and on success build the plan the
// data-file encoder needs (distinct values + per-run dictionary index), so the encoder doesn't have to
// rebuild it from a second, per-ROW hashmap scan. Two passes: the first is the original cheap,
// zero-allocation run-only scan (needed since most columns -- e.g. a near-unique/high-cardinality
// string column -- fail this check, and building a dictionary for a column we're about to reject would
// be wasted allocation on the common path); only once that pass confirms the column is genuinely
// run-friendly does a second pass build the dictionary, with one hash-map insert per RUN (using the
// row/length boundaries the first pass already found, so no re-scanning for run boundaries), not per row.
bool variable_column_dict_rle_beneficial(nano_lance::ColumnValues& cv, const ColumnEncodingOptions& options) {
    cv.structural_dict_rle_plan = {};
    const std::size_t ow = cv.variable.large ? 8U : 4U;
    if (cv.variable.offsets.size() < 2U * ow) {
        return false;
    }
    // `lance-encoding:rle-threshold=0` turns RLE off; a positive threshold t moves the run cutoff to n * t
    // (pylance's guard, see fixed_column_rle_plan). The default cutoff is half the rows.
    if (options.rle_set && options.rle_threshold <= 0.0) {
        return false;
    }
    auto read_offset = [&](std::size_t index) -> std::int64_t {
        const auto* p = cv.variable.offsets.data() + index * ow;
        if (cv.variable.large) {
            std::int64_t v = 0;
            std::memcpy(&v, p, 8);
            return v;
        }
        std::int32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    };
    const std::size_t rows = cv.variable.offsets.size() / ow - 1U;
    if (rows == 0U) {
        return false;
    }
    const char* base = reinterpret_cast<const char*>(cv.variable.data.data());
    const std::size_t data_size = cv.variable.data.size();

    // Pass 1: cheap run-boundary detection only (memcmp, no hashing/allocation beyond the run list
    // itself, which is at most one entry per run -- far fewer than `rows` for anything run-friendly).
    std::vector<std::pair<std::size_t, std::uint64_t>> row_runs;  // (row start, run length)
    // The run cutoff: split runs at or above it are not run-friendly. The default is half the rows.
    const std::size_t run_cutoff =
        options.rle_set ? static_cast<std::size_t>(std::ceil(static_cast<double>(rows) * options.rle_threshold))
                        : (rows + 1U) / 2U;
    std::size_t split_runs = 0;
    std::size_t i = 0;
    while (i < rows) {
        const auto s0 = read_offset(i);
        const auto e0 = read_offset(i + 1);
        if (s0 < 0 || e0 < s0 || static_cast<std::size_t>(e0) > data_size) {
            return false;
        }
        const std::size_t len0 = static_cast<std::size_t>(e0 - s0);
        std::size_t run = 1;
        while (i + run < rows) {
            const auto sk = read_offset(i + run);
            const auto ek = read_offset(i + run + 1);
            if (sk < 0 || ek < sk || static_cast<std::size_t>(ek) > data_size) {
                return false;
            }
            if (static_cast<std::size_t>(ek - sk) != len0 || (len0 != 0U && std::memcmp(base + sk, base + s0, len0) != 0)) {
                break;
            }
            ++run;
        }
        split_runs += (run + 254U) / 255U;  // Lance caps run length at 255
        if (split_runs >= run_cutoff) {
            return false;  // not run-friendly (and therefore not low-cardinality)
        }
        if (split_runs * 5U + 32U > 32760U) {
            return false;  // already too many runs for the one-chunk page below: stop scanning
        }
        row_runs.emplace_back(i, run);
        i += run;
    }
    const std::size_t values_size = split_runs * 4U;  // u32 dictionary indices, one per split run
    if ((values_size + split_runs + 32U) > 32760U) {
        return false;
    }

    // Pass 2 (only reached once genuinely beneficial): build the run-keyed dictionary.
    std::unordered_map<std::string_view, std::uint32_t> dict;
    std::vector<std::string_view> distinct;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> runs;
    runs.reserve(row_runs.size());
    for (const auto& [row, run] : row_runs) {
        const auto s0 = read_offset(row);
        const auto e0 = read_offset(row + 1);
        const std::string_view val(base + s0, static_cast<std::size_t>(e0 - s0));
        const auto id = static_cast<std::uint32_t>(distinct.size());
        const auto [it, inserted] = dict.try_emplace(val, id);  // no node allocated for a repeat
        if (inserted) {
            distinct.push_back(val);
        }
        runs.emplace_back(it->second, run);
    }
    cv.structural_dict_rle_plan.computed = true;
    cv.structural_dict_rle_plan.distinct = std::move(distinct);
    cv.structural_dict_rle_plan.runs = std::move(runs);
    return true;
}

// Decide whether a structural dictionary (flat bitpacked indices + dictionary buffer) wins for a
// scattered low-cardinality string column. Mirrors Lance defaults: dict-divisor=2, dict-size-ratio=0.8,
// min 100 rows, max 100k distinct values.
bool variable_column_dict_beneficial(nano_lance::ColumnValues& cv, const ColumnEncodingOptions& options) {
    constexpr std::size_t kMinRows = 100U;
    constexpr std::size_t kMaxCardinality = 100000U;
    // lance-encoding:dict-divisor / dict-size-ratio move the two cutoffs (defaults: 2 and 0.8, pylance's).
    const std::size_t kDictDivisor = std::max<std::size_t>(options.dict_divisor, 1U);
    const double kDictSizeRatio = options.dict_size_ratio;

    cv.structural_dict_plan = {};
    const std::size_t ow = cv.variable.large ? 8U : 4U;
    if (cv.variable.offsets.size() < 2U * ow) {
        return false;
    }
    auto read_offset = [&](std::size_t index) -> std::int64_t {
        const auto* p = cv.variable.offsets.data() + index * ow;
        if (cv.variable.large) {
            std::int64_t v = 0;
            std::memcpy(&v, p, 8);
            return v;
        }
        std::int32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    };
    const std::size_t rows = cv.variable.offsets.size() / ow - 1U;
    if (rows < kMinRows) {
        return false;
    }
    const char* base = reinterpret_cast<const char*>(cv.variable.data.data());
    const std::size_t data_size = cv.variable.data.size();

    // Cheap pre-check on a prefix sample before committing to the full scan below: building the real
    // dictionary means one heap-allocating hash-map insert per distinct value, and for a genuinely
    // high-cardinality column (near-unique IDs, free-text labels) that means tens of thousands of
    // allocations just to conclude "not beneficial" and throw the whole map away. A column's
    // cardinality is normally fairly uniform across a write batch, so a small prefix sample is a
    // reasonable predictor of the full-column ratio; only skip the full scan when the sample already
    // shows the ratio decisively blown (not merely close), to keep false negatives rare -- the cost of
    // a false negative here is a slightly larger encoding (falls back to zstd/plain), never wrong data.
    // Sample distinct-count via sort+unique of the values' 64-bit hashes (one vector allocation total, and
    // integer compares instead of string compares) rather than a hash set (one heap allocation per
    // distinct element even for this small sample). A collision could only undercount by one.
    constexpr std::size_t kSampleRows = 4096U;
    constexpr double kSampleRejectMargin = 1.5;  // require the sample ratio to exceed the real cutoff by 50%
    if (rows > kSampleRows * 4U) {
        std::vector<std::size_t> sample;
        sample.reserve(kSampleRows);
        const std::hash<std::string_view> hash;
        for (std::size_t i = 0; i < kSampleRows; ++i) {
            const auto s = read_offset(i);
            const auto e = read_offset(i + 1);
            if (s < 0 || e < s || static_cast<std::size_t>(e) > data_size) {
                return false;
            }
            sample.push_back(hash(std::string_view(base + s, static_cast<std::size_t>(e - s))));
        }
        std::sort(sample.begin(), sample.end());
        const auto sample_distinct_count =
            static_cast<std::size_t>(std::unique(sample.begin(), sample.end()) - sample.begin());
        const auto sample_threshold =
            static_cast<double>(kSampleRows) / static_cast<double>(kDictDivisor) * kSampleRejectMargin;
        if (static_cast<double>(sample_distinct_count) > sample_threshold) {
            return false;
        }
    }

    // Build the dictionary (distinct values + per-row indices) exactly as the data-file encoder would,
    // so on success the encoder can reuse this scan instead of repeating the dedup + index pass.
    std::unordered_map<std::string_view, std::uint32_t> dict;
    dict.reserve(rows / 4U);
    std::vector<std::string_view> distinct;
    std::vector<std::uint32_t> indices;
    indices.reserve(rows);
    std::size_t raw_bytes = 0;
    std::size_t dict_data = 0;
    for (std::size_t i = 0; i < rows; ++i) {
        const auto s = read_offset(i);
        const auto e = read_offset(i + 1);
        if (s < 0 || e < s || static_cast<std::size_t>(e) > data_size) {
            return false;
        }
        const auto len = static_cast<std::size_t>(e - s);
        raw_bytes += len;
        const std::string_view val(base + s, len);
        const auto id = static_cast<std::uint32_t>(distinct.size());
        const auto [it, inserted] = dict.try_emplace(val, id);  // no node allocated for a repeat
        if (inserted) {
            if (distinct.size() >= kMaxCardinality) {
                return false;
            }
            distinct.push_back(val);
            dict_data += len;
            // Early exit: distinct.size() only grows, so once it crosses rows/kDictDivisor the final
            // cardinality check below is already decided -- no need to keep building the hash map (and
            // the indices/distinct vectors) for a column that's already too high-cardinality to dict,
            // e.g. a near-unique string column, which would otherwise pay for a full O(n) hash+insert
            // scan just to be thrown away.
            if (distinct.size() > rows / kDictDivisor) {
                return false;
            }
        }
        indices.push_back(it->second);
    }
    if (distinct.empty()) {
        return false;
    }
    const std::size_t dict_bytes = 8U + (distinct.size() + 1U) * 4U + dict_data;
    const std::size_t index_bytes = rows * 4U;
    const std::size_t raw_total = raw_bytes + (rows + 1U) * ow;
    const std::size_t encoded_total = dict_bytes + index_bytes;
    if (encoded_total >= static_cast<std::size_t>(static_cast<double>(raw_total) * kDictSizeRatio)) {
        return false;
    }
    cv.structural_dict_plan.computed = true;
    cv.structural_dict_plan.distinct = std::move(distinct);
    cv.structural_dict_plan.indices = std::move(indices);
    return true;
}

}  // namespace nano_lance::encoding_choice
