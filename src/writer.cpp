// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/nano_lance_writer.h"

#include "nanolance/array_accessor.hpp"
#include "nanolance/blob_builder.hpp"
#include "nanolance/blob_v2_external.hpp"
#include "nanolance/data_file_writer.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/schema_mapper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct WriterState {
    std::filesystem::path dataset_path;
    int compression_level = 0;
    std::uint64_t pending_batches = 0;
    std::uint64_t pending_rows = 0;
    nano_lance::LanceSchemaMapping schema_mapping;
    std::vector<nano_lance::ColumnValues> column_values;
    nano_lance::ColumnValues blob_column_values;
    const nano_lance::LanceField* blob_field = nullptr;
    bool ignore_nullability = false;
    bool blob_uri_dictionary = false;
    bool compression = false;
    // Structural (lossless) re-encodings — bitpacking, ConstantLayout, RLE, dictionary, dict-RLE — are
    // independent of zstd byte compression and on by default: they shrink files and usually speed up
    // writes, and stay stock-Lance-readable. `compression` (set_compression) now controls ONLY zstd.
    bool structural = true;
    /// Per-field encoding declarations (set_column_encoding): "plain"/"bitpack"/"bss-zstd"/"zstd".
    /// A declared column skips the commit-time detection scans entirely. Absent == "auto".
    std::map<std::string, std::string> column_encodings;
    /// Borrow the caller's fixed-width Arrow buffers until commit instead of copying (zero-copy
    /// ingest for single-batch columns; multi-batch columns silently fall back to copying).
    bool borrow_buffers = false;
    bool has_schema = false;
    /// After the first successful manifest write, further commits must pass `is_append=true`.
    bool append_only_commits = false;
    bool closed = false;
};

void clear_error(NanoLanceWriter* writer) {
    if (writer != nullptr) {
        writer->last_error[0] = '\0';
    }
}

int set_error(NanoLanceWriter* writer, int code, const std::string& message) {
    if (writer != nullptr) {
        const auto bytes_to_copy = std::min(message.size(), sizeof(writer->last_error) - 1U);
        std::memcpy(writer->last_error, message.data(), bytes_to_copy);
        writer->last_error[bytes_to_copy] = '\0';
    }
    return code;
}

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
bool fixed_column_rle_plan(nano_lance::ColumnValues& cv, std::size_t bpv) {
    cv.fixed_rle_plan = {};
    if (bpv == 0U || cv.fixed_size() == 0U || cv.fixed_size() % bpv != 0U) {
        return false;
    }
    const std::size_t n = cv.fixed_size() / bpv;
    // Prefix-sample pre-check (same pattern as variable_column_dict_beneficial's sampling): even with
    // the reject-early exit, a scattered column scans ~n/2 rows before failing (split_runs grows one
    // per row, crossing n/2 halfway through). A 4096-row prefix predicts that verdict at ~1/25 the
    // cost. Reject only when the sample DECISIVELY fails -- split_runs over 3/4 of the sample, a 1.5x
    // margin above the real n/2 cutoff -- to keep false negatives rare; the cost of one is a valid,
    // slightly larger bitpack fallback, never wrong data.
    constexpr std::size_t kSampleRows = 4096U;
    if (n > kSampleRows * 4U) {
        std::size_t sample_split_runs = 0;
        if (!dispatch_rle_scan(cv.fixed_data(), kSampleRows, bpv, kSampleRows * 3U / 4U,
                               sample_split_runs, nullptr)) {
            return false;
        }
    }
    std::size_t split_runs = 0;
    // reject_at == ceil(n/2) is exactly the previous `split_runs * 2 >= n` cutoff.
    if (!dispatch_rle_scan(cv.fixed_data(), n, bpv, (n + 1U) / 2U, split_runs, nullptr)) {
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
            std::memcmp(cv.variable.data.data() + s, cv.variable.data.data() + start0, len) != 0) {
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
bool variable_column_dict_rle_beneficial(nano_lance::ColumnValues& cv) {
    cv.structural_dict_rle_plan = {};
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
    const char* base = reinterpret_cast<const char*>(cv.variable.data.data());
    const std::size_t data_size = cv.variable.data.size();

    // Pass 1: cheap run-boundary detection only (memcmp, no hashing/allocation beyond the run list
    // itself, which is at most one entry per run -- far fewer than `rows` for anything run-friendly).
    std::vector<std::pair<std::size_t, std::uint64_t>> row_runs;  // (row start, run length)
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
            if (static_cast<std::size_t>(ek - sk) != len0 || std::memcmp(base + sk, base + s0, len0) != 0) {
                break;
            }
            ++run;
        }
        split_runs += (run + 254U) / 255U;  // Lance caps run length at 255
        if (split_runs * 2U >= rows) {
            return false;  // not run-friendly (and therefore not low-cardinality)
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
        const auto [it, inserted] = dict.emplace(val, id);
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
bool variable_column_dict_beneficial(nano_lance::ColumnValues& cv) {
    constexpr std::size_t kMinRows = 100U;
    constexpr std::size_t kDictDivisor = 2U;
    constexpr double kDictSizeRatio = 0.8;
    constexpr std::size_t kMaxCardinality = 100000U;

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
    // Sample distinct-count via sort+unique (one vector allocation total) rather than a hash set (one
    // heap allocation per distinct element even for this small sample) -- no per-row allocation at all.
    constexpr std::size_t kSampleRows = 4096U;
    constexpr double kSampleRejectMargin = 1.5;  // require the sample ratio to exceed the real cutoff by 50%
    if (rows > kSampleRows * 4U) {
        std::vector<std::string_view> sample;
        sample.reserve(kSampleRows);
        for (std::size_t i = 0; i < kSampleRows; ++i) {
            const auto s = read_offset(i);
            const auto e = read_offset(i + 1);
            if (s < 0 || e < s || static_cast<std::size_t>(e) > data_size) {
                return false;
            }
            sample.emplace_back(base + s, static_cast<std::size_t>(e - s));
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
        const auto [it, inserted] = dict.emplace(val, id);
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

WriterState* state_from(NanoLanceWriter* writer) {
    if (writer == nullptr) {
        return nullptr;
    }
    return static_cast<WriterState*>(writer->private_data);
}

const WriterState* state_from(const NanoLanceWriter* writer) {
    if (writer == nullptr) {
        return nullptr;
    }
    return static_cast<const WriterState*>(writer->private_data);
}

std::size_t count_non_blob_physical_columns(const nano_lance::LanceSchemaMapping& mapping,
                                            std::int32_t blob_parent_id) {
    std::size_t count = 0;
    for (const auto& field : mapping.fields) {
        if (!nano_lance::lance_field_is_physical(field)) {
            continue;
        }
        if (blob_parent_id >= 0 && field.parent_id == blob_parent_id) {
            continue;
        }
        ++count;
    }
    return count;
}

/// Next numeric suffix for `data/fragment-<n>.lance` (0 if `data/` is missing or has no matching files).
std::uint64_t next_fragment_numeric_suffix(const std::filesystem::path& dataset_path) {
    const auto data_dir = dataset_path / "data";
    std::error_code ec;
    if (!std::filesystem::exists(data_dir, ec)) {
        return 0;
    }
    std::uint64_t max_seen = 0;
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        constexpr const char kPrefix[] = "fragment-";
        constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1U;
        if (name.size() <= kPrefixLen + 6U) {
            continue;
        }
        if (name.rfind(".lance") != name.size() - 6U) {
            continue;
        }
        if (name.compare(0, kPrefixLen, kPrefix) != 0) {
            continue;
        }
        const auto mid = name.substr(kPrefixLen, name.size() - 6U - kPrefixLen);
        try {
            const auto n = static_cast<std::uint64_t>(std::stoull(mid));
            max_seen = std::max(max_seen, n);
            found = true;
        } catch (...) {
        }
    }
    return found ? max_seen + 1U : 0U;
}

}  // namespace

extern "C" {

void nano_lance_write_options_init(NanoLanceWriteOptions* options) {
    if (options != nullptr) {
        *options = NanoLanceWriteOptions{};
    }
}

int nano_lance_writer_open(NanoLanceWriter* writer, const char* path, const NanoLanceWriteOptions* options) {
    if (writer == nullptr) {
        return NANO_LANCE_INVALID_ARGUMENT;
    }
    if (writer->private_data != nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "close writer before opening another dataset");
    }
    clear_error(writer);

    // A NULL options pointer and a zeroed struct mean the same thing, and both mean the defaults.
    const NanoLanceWriteOptions defaults{};
    const NanoLanceWriteOptions& opts = options != nullptr ? *options : defaults;

    if (path == nullptr || path[0] == '\0') {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "dataset path must not be empty");
    }
    if (opts.compression_level < 0 || opts.compression_level > 22) {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "zstd compression level must be in range 0..22");
    }
    if (opts.num_column_encodings != 0U && opts.column_encodings == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT,
                         "column_encodings is NULL but num_column_encodings is not zero");
    }
    if (opts.append && opts.blob_uri_dictionary) {
        // Same refusal, same code, as set_blob_uri_dictionary on an append writer.
        return set_error(writer, NANO_LANCE_UNSUPPORTED,
                         "blob URI dictionary mode is not supported for append datasets");
    }

    auto state = std::make_unique<WriterState>();
    state->dataset_path = path;
    state->compression_level = opts.compression_level;
    state->compression = opts.compression;
    state->structural = !opts.disable_structural_encoding;
    state->blob_uri_dictionary = opts.blob_uri_dictionary;
    state->borrow_buffers = opts.borrow_buffers;
    state->append_only_commits = opts.append;

    for (std::size_t i = 0; i < opts.num_column_encodings; ++i) {
        const auto& entry = opts.column_encodings[i];
        if (entry.field_name == nullptr || entry.field_name[0] == '\0') {
            return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "field name must not be empty");
        }
        const std::string enc = entry.encoding != nullptr ? entry.encoding : "";
        if (enc == "auto") {
            state->column_encodings.erase(entry.field_name);
        } else if (enc == "plain" || enc == "bitpack" || enc == "bss-zstd" || enc == "zstd") {
            state->column_encodings[entry.field_name] = enc;
        } else {
            return set_error(writer, NANO_LANCE_INVALID_ARGUMENT,
                             "unknown column encoding (expected auto/plain/bitpack/bss-zstd/zstd): " + enc);
        }
    }

    if (opts.append) {
        std::error_code ec;
        if (!std::filesystem::exists(state->dataset_path / "_versions", ec)) {
            return set_error(writer, NANO_LANCE_IO_ERROR, "init_append requires an existing dataset with _versions");
        }

        nano_lance::pb::Manifest manifest{};
        std::uint64_t manifest_version = 0;
        std::string load_error;
        if (!nano_lance::load_latest_manifest(state->dataset_path, manifest, manifest_version, load_error)) {
            return set_error(writer, NANO_LANCE_IO_ERROR, load_error);
        }
        if (!nano_lance::lance_schema_mapping_from_manifest(manifest, state->schema_mapping, load_error)) {
            return set_error(writer, NANO_LANCE_UNSUPPORTED, load_error);
        }

        state->blob_field = nano_lance::find_blob_v2_parent(state->schema_mapping);
        const std::int32_t blob_parent_id = state->blob_field != nullptr ? state->blob_field->id : -1;
        state->column_values.resize(count_non_blob_physical_columns(state->schema_mapping, blob_parent_id));
        state->has_schema = true;
    }

    writer->private_data = state.release();
    return NANO_LANCE_OK;
}

int nano_lance_writer_init(NanoLanceWriter* writer, const char* path, int compression_level) {
    if (writer == nullptr) {
        return NANO_LANCE_INVALID_ARGUMENT;
    }
    // Historically this reset private_data without looking at it, so a caller that re-inits without
    // closing leaks rather than being told. Kept, because changing it would break those callers.
    writer->private_data = nullptr;
    NanoLanceWriteOptions options{};
    options.compression_level = compression_level;
    return nano_lance_writer_open(writer, path, &options);
}

int nano_lance_writer_init_append(NanoLanceWriter* writer, const char* path, int compression_level) {
    NanoLanceWriteOptions options{};
    options.compression_level = compression_level;
    options.append = true;
    const auto rc = nano_lance_writer_open(writer, path, &options);
    if (rc == NANO_LANCE_INVALID_STATE && writer != nullptr && writer->private_data != nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "close writer before init_append");
    }
    return rc;
}

int nano_lance_writer_set_ignore_nullability(NanoLanceWriter* writer, bool ignore_nullability) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "ignore-nullability must be set before writing batches");
    }
    state->ignore_nullability = ignore_nullability;
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_set_blob_uri_dictionary(NanoLanceWriter* writer, bool enable) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "blob URI dictionary must be set before writing batches");
    }
    if (enable && state->append_only_commits) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED, "blob URI dictionary mode is not supported for append datasets");
    }
    state->blob_uri_dictionary = enable;
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_set_compression(NanoLanceWriter* writer, bool enable) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "compression must be set before writing batches");
    }
    state->compression = enable;
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_set_structural_encoding(NanoLanceWriter* writer, bool enable) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE,
                         "structural encoding must be set before writing batches");
    }
    state->structural = enable;
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_set_column_encoding(NanoLanceWriter* writer, const char* field_name,
                                          const char* encoding) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE,
                         "column encodings must be set before writing batches");
    }
    if (field_name == nullptr || field_name[0] == '\0') {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "field name must not be empty");
    }
    const std::string enc = encoding != nullptr ? encoding : "";
    if (enc == "auto") {
        state->column_encodings.erase(field_name);
    } else if (enc == "plain" || enc == "bitpack" || enc == "bss-zstd" || enc == "zstd") {
        state->column_encodings[field_name] = enc;
    } else {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT,
                         "unknown column encoding (expected auto/plain/bitpack/bss-zstd/zstd): " + enc);
    }
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_set_borrow_buffers(NanoLanceWriter* writer, bool enable) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE,
                         "borrow_buffers must be set before writing batches");
    }
    state->borrow_buffers = enable;
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_write_batch(NanoLanceWriter* writer, struct ArrowArray* batch, struct ArrowSchema* schema) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->closed) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is already closed");
    }
    if (batch == nullptr || schema == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "batch and schema must not be null");
    }
    if (batch->length < 0) {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "batch length must not be negative");
    }

    nano_lance::LanceSchemaMapping batch_mapping;
    std::string error;
    if (!nano_lance::map_arrow_schema(*schema, batch_mapping, error, state->ignore_nullability)) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED, error);
    }

    const auto* batch_blob_field = nano_lance::find_blob_v2_parent(batch_mapping);
    if (!state->has_schema) {
        state->schema_mapping = std::move(batch_mapping);
        state->blob_field = nano_lance::find_blob_v2_parent(state->schema_mapping);
        const std::int32_t blob_parent_id = state->blob_field != nullptr ? state->blob_field->id : -1;
        state->column_values.resize(count_non_blob_physical_columns(state->schema_mapping, blob_parent_id));
        state->has_schema = true;
    } else if (!nano_lance::schema_mappings_equal(state->schema_mapping, batch_mapping)) {
        // `state->schema_mapping` is the ingest-shape mapping captured on the first batch; compare the
        // new batch's ingest mapping directly. (Finalization to the packed blob layout happens at commit,
        // not here — finalizing only the candidate made every 2nd+ blob batch look like a schema change.)
        return set_error(writer, NANO_LANCE_UNSUPPORTED,
                         "all batches in a writer session must share one schema (locked by the first "
                         "batch); " +
                             nano_lance::describe_schema_mapping_mismatch(state->schema_mapping,
                                                                          batch_mapping));
    }

    const std::int32_t blob_parent_id = state->blob_field != nullptr ? state->blob_field->id : -1;
    if (!nano_lance::append_batch_column_values(*batch,
                                                state->schema_mapping,
                                                state->column_values,
                                                error,
                                                blob_parent_id,
                                                state->borrow_buffers)) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED, error);
    }
    if (state->blob_field != nullptr) {
        if (!nano_lance::append_blob_v2_batch_column_values(*batch,
                                                            state->schema_mapping,
                                                            *state->blob_field,
                                                            state->blob_uri_dictionary,
                                                            state->blob_column_values,
                                                            error)) {
            return set_error(writer, NANO_LANCE_UNSUPPORTED, error);
        }
    } else if (batch_blob_field != nullptr) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED, "schema gained lance.blob.v2 mid-write (not supported)");
    }

    ++state->pending_batches;
    state->pending_rows += static_cast<std::uint64_t>(batch->length);
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_commit(NanoLanceWriter* writer, bool is_append) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->closed) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is already closed");
    }
    if (state->pending_rows == 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "no pending rows to commit");
    }
    if (state->append_only_commits && !is_append) {
        return set_error(writer, NANO_LANCE_INVALID_STATE,
                         "commit requires is_append=true after the first manifest was written");
    }
    if (state->blob_uri_dictionary && is_append) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED,
                         "blob URI dictionary mode does not support append commits");
    }

    std::error_code error;
    if (is_append && !std::filesystem::exists(state->dataset_path, error)) {
        return set_error(writer, NANO_LANCE_IO_ERROR, "append requested but dataset path does not exist");
    }
    if (!state->has_schema) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "no complete Arrow IPC batches were written");
    }

    std::string writer_error;
    if (!is_append) {
        std::filesystem::create_directories(state->dataset_path / "_versions", error);
        if (error) {
            return set_error(writer, NANO_LANCE_IO_ERROR, "failed to create dataset version directory: " + error.message());
        }
        std::filesystem::create_directories(state->dataset_path / "data", error);
        if (error) {
            return set_error(writer, NANO_LANCE_IO_ERROR, "failed to create dataset data directory: " + error.message());
        }
    }

    nano_lance::LanceSchemaMapping disk_schema = state->schema_mapping;
    if (state->blob_field != nullptr) {
        if (!nano_lance::finalize_blob_v2_schema_for_write(disk_schema, writer_error)) {
            return set_error(writer, NANO_LANCE_UNSUPPORTED, writer_error);
        }
        // Persist the URI dictionary on the blob parent field so the reader can resolve each row's
        // URI by index. Stored in manifest/field metadata; only present in dictionary mode.
        if (state->blob_uri_dictionary && !state->blob_column_values.blob_v2.uri_dictionary.empty()) {
            const auto serialized =
                nano_lance::blob_v2_serialize_uri_dictionary(state->blob_column_values.blob_v2.uri_dictionary);
            for (auto& field : disk_schema.fields) {
                if (field.extension_name == nano_lance::kBlobV2ExtensionName && field.logical_type == "struct" &&
                    field.parent_id == -1) {
                    field.metadata[nano_lance::kBlobV2UriDictMetadataKey] = serialized;
                    break;
                }
            }
        }
    }

    // Tag fields for encoding. zstd (a real CPU-for-size tradeoff) is opt-in via set_compression and
    // applies to variable-width columns and, for float/double, as byte-stream-split + zstd (splitting
    // each value into its mantissa/exponent byte planes before compressing — the same technique stock
    // Lance uses for these types, since compressing already-interleaved float bytes barely helps).
    // Integer bitpacking is a structural, lossless re-encoding and is enabled by the (default-on)
    // structural switch. (Stock Lance reads the encoding from the data-file PageLayout; this metadata
    // is nanolance's own read-side signal and an inert write hint to Lance.)
    for (auto& field : disk_schema.fields) {
        if (!nano_lance::lance_field_is_physical(field) || !field.extension_name.empty()) {
            continue;
        }
        // Declared encodings (set_column_encoding) override the automatic tagging below and later skip
        // the detection scans entirely -- the caller vouched for the column's shape at compile/config
        // time, so nanolance doesn't re-derive it from a full data scan. Type compatibility is checked
        // here so a bad declaration fails the commit loudly instead of writing a broken file.
        const auto declared_it = state->column_encodings.find(field.name);
        if (declared_it != state->column_encodings.end()) {
            const auto& declared = declared_it->second;
            const bool variable = nano_lance::lance_field_is_variable_width(field.logical_type);
            if (declared == "plain") {
                continue;  // no tags: flat/variable pages, no structural encoding, no zstd
            }
            if (declared == "bitpack") {
                if (!nano_lance::lance_logical_type_is_bitpackable_integer(field.logical_type)) {
                    return set_error(writer, NANO_LANCE_INVALID_ARGUMENT,
                                     "column encoding 'bitpack' requires an integer column: " + field.name);
                }
                field.metadata["nanolance:packing"] = "bitpack";
                continue;
            }
            if (declared == "bss-zstd") {
                if (field.logical_type != "float" && field.logical_type != "double") {
                    return set_error(writer, NANO_LANCE_INVALID_ARGUMENT,
                                     "column encoding 'bss-zstd' requires a float/double column: " + field.name);
                }
                field.metadata["nanolance:packing"] = "bss-zstd";
                continue;
            }
            // declared == "zstd"
            if (!variable) {
                return set_error(writer, NANO_LANCE_INVALID_ARGUMENT,
                                 "column encoding 'zstd' requires a string/binary column: " + field.name);
            }
            if (state->compression) {
                field.metadata["lance-encoding:compression"] = "zstd";
            }
            continue;
        }
        if (nano_lance::lance_field_is_variable_width(field.logical_type)) {
            if (state->compression) {
                field.metadata["lance-encoding:compression"] = "zstd";
            }
        } else if (state->structural && nano_lance::lance_logical_type_is_bitpackable_integer(field.logical_type)) {
            field.metadata["nanolance:packing"] = "bitpack";
        } else if (state->compression && (field.logical_type == "float" || field.logical_type == "double")) {
            field.metadata["nanolance:packing"] = "bss-zstd";
        }
    }

    std::vector<nano_lance::ColumnValues> commit_columns;
    if (state->blob_field != nullptr) {
        const auto physical = nano_lance::lance_physical_fields(disk_schema);
        commit_columns.reserve(physical.size());
        std::size_t non_blob_index = 0;
        for (const auto* field : physical) {
            if (field->extension_name == nano_lance::kBlobV2ExtensionName) {
                commit_columns.push_back(state->blob_column_values);
                continue;
            }
            if (non_blob_index >= state->column_values.size()) {
                return set_error(writer, NANO_LANCE_INVALID_STATE, "missing column values for physical field " + field->name);
            }
            commit_columns.push_back(state->column_values[non_blob_index++]);
        }
        if (commit_columns.size() != physical.size() || non_blob_index != state->column_values.size()) {
            return set_error(writer, NANO_LANCE_INVALID_STATE, "blob dataset physical column layout mismatch");
        }
    } else {
        commit_columns = std::move(state->column_values);
    }

    // Constant fixed-width columns -> ConstantLayout (value inline in the page descriptor, zero data
    // bytes). Overrides the bitpack tag for those columns. Tags disk_schema so both the manifest and
    // the data-file descriptor carry packing=constant + the raw value bytes for the reader. These
    // (constant / RLE / dictionary / dict-RLE) are structural encodings, independent of zstd.
    if (state->structural) {
        const auto physical = nano_lance::lance_physical_fields(disk_schema);
        for (std::size_t i = 0; i < physical.size() && i < commit_columns.size(); ++i) {
            const auto* pf = physical[i];
            if (!pf->extension_name.empty()) {
                continue;
            }
            // Declared columns (set_column_encoding) skip ALL detection scans -- the encoding was
            // decided by the caller; these scans are exactly the work the declaration saves.
            if (state->column_encodings.find(pf->name) != state->column_encodings.end()) {
                continue;
            }
            auto& cv = commit_columns[i];
            std::vector<std::uint8_t> value;
            bool constant = false;
            if (cv.kind == nano_lance::ColumnValues::Kind::FixedWidth) {
                const auto bpv = nano_lance::lance_logical_type_value_bytes(pf->logical_type);
                if (bpv != 0U && cv.fixed_size() >= bpv && cv.fixed_size() % bpv == 0U) {
                    // A buffer is all-one-value iff it equals itself shifted by one element, so ONE
                    // overlapped memcmp over the whole column replaces the previous
                    // one-libc-call-per-row loop (memcmp only reads, so overlap is fine; a 1-row
                    // column compares 0 bytes and is correctly constant).
                    constant = std::memcmp(cv.fixed_data(), cv.fixed_data() + bpv,
                                           cv.fixed_size() - bpv) == 0;
                    if (constant) {
                        value.assign(cv.fixed_data(), cv.fixed_data() + bpv);
                    }
                }
            } else if (cv.kind == nano_lance::ColumnValues::Kind::VariableWidth) {
                constant = variable_column_constant_value(cv, value);
            }
            if (constant) {
                for (auto& field : disk_schema.fields) {
                    if (field.id == pf->id) {
                        field.metadata["nanolance:packing"] = "constant";
                        field.metadata["nanolance:const-value"] = std::string(value.begin(), value.end());
                        break;
                    }
                }
                continue;
            }
            // Run-length encoding for repetitive fixed-width integer columns (beats bitpacking when
            // there are long runs, e.g. dictionary indices later).
            if (cv.kind == nano_lance::ColumnValues::Kind::FixedWidth &&
                nano_lance::lance_logical_type_is_bitpackable_integer(pf->logical_type)) {
                const auto bpv = nano_lance::lance_logical_type_value_bytes(pf->logical_type);
                if (fixed_column_rle_plan(cv, bpv)) {
                    for (auto& field : disk_schema.fields) {
                        if (field.id == pf->id) {
                            field.metadata["nanolance:packing"] = "rle";
                            break;
                        }
                    }
                } else if (!bitpack_beats_flat(cv, bpv)) {
                    // Incompressible integers (hashes, ids, random keys): drop the bitpack tag so the
                    // column writes flat pages, which is both smaller and cheaper to read.
                    for (auto& field : disk_schema.fields) {
                        if (field.id == pf->id) {
                            field.metadata.erase("nanolance:packing");
                            break;
                        }
                    }
                }
            } else if (cv.kind == nano_lance::ColumnValues::Kind::VariableWidth &&
                       variable_column_dict_rle_beneficial(cv)) {
                // Low-cardinality run-length string column -> dictionary + RLE'd indices.
                for (auto& field : disk_schema.fields) {
                    if (field.id == pf->id) {
                        field.metadata["nanolance:packing"] = "dict-rle";
                        field.metadata.erase("lance-encoding:compression");
                        break;
                    }
                }
            } else if (cv.kind == nano_lance::ColumnValues::Kind::VariableWidth &&
                       variable_column_dict_beneficial(cv)) {
                // Scattered low-cardinality strings -> structural dictionary + flat indices.
                for (auto& field : disk_schema.fields) {
                    if (field.id == pf->id) {
                        field.metadata["nanolance:packing"] = "dict";
                        field.metadata.erase("lance-encoding:compression");
                        break;
                    }
                }
            }
        }
    }

    nano_lance::DataFileResult data_file;
    const auto data_file_name =
        "fragment-" + std::to_string(next_fragment_numeric_suffix(state->dataset_path)) + ".lance";
    if (!nano_lance::write_lance_data_file(state->dataset_path,
                                           data_file_name,
                                           disk_schema,
                                           commit_columns,
                                           state->pending_rows,
                                           state->compression_level,
                                           state->compression,
                                           data_file,
                                           writer_error)) {
        return set_error(writer, NANO_LANCE_IO_ERROR, writer_error);
    }

    std::uint64_t version = 0;
    if (!nano_lance::write_dataset_manifest(state->dataset_path,
                                            disk_schema,
                                            data_file,
                                            state->pending_rows,
                                            is_append,
                                            version,
                                            writer_error)) {
        return set_error(writer, NANO_LANCE_IO_ERROR, writer_error);
    }

    state->append_only_commits = true;
    state->pending_batches = 0;
    state->pending_rows = 0;
    state->column_values.clear();
    state->blob_column_values = nano_lance::ColumnValues{};
    state->blob_field = nano_lance::find_blob_v2_parent(state->schema_mapping);
    {
        const std::int32_t blob_parent_id = state->blob_field != nullptr ? state->blob_field->id : -1;
        state->column_values.resize(count_non_blob_physical_columns(state->schema_mapping, blob_parent_id));
    }
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_close(NanoLanceWriter* writer) {
    if (writer == nullptr) {
        return NANO_LANCE_INVALID_ARGUMENT;
    }
    auto* state = state_from(writer);
    if (state == nullptr) {
        clear_error(writer);
        return NANO_LANCE_OK;
    }
    state->closed = true;
    delete state;
    writer->private_data = nullptr;
    clear_error(writer);
    return NANO_LANCE_OK;
}

const char* nano_lance_writer_last_error(const NanoLanceWriter* writer) {
    if (writer == nullptr) {
        return "writer is null";
    }
    return writer->last_error;
}

uint64_t nano_lance_writer_pending_batches(const NanoLanceWriter* writer) {
    const auto* state = state_from(writer);
    if (state == nullptr) {
        return 0;
    }
    return state->pending_batches;
}

}  // extern "C"
