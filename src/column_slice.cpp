// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/column_slice.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <vector>

namespace nano_lance {
namespace {

constexpr std::size_t bitmap_bytes(std::uint64_t rows) {
    return static_cast<std::size_t>((rows + 7U) / 8U);
}

bool bit_set(const std::vector<std::uint8_t>& bitmap, std::uint64_t index) {
    return (bitmap[static_cast<std::size_t>(index >> 3U)] &
            static_cast<std::uint8_t>(1U << (index & 7U))) != 0U;
}

/// Re-pack `[first, first + count)` of an LSB-first validity bitmap to start at bit 0.
///
/// A byte-wise memmove would only be correct when `first % 8 == 0`. Every other offset shifts every
/// bit, which is exactly the case a fragment boundary lands on most of the time -- so this is
/// deliberately the dumb bit-at-a-time loop rather than a word-shifting one. It runs for at most the
/// two fragments a range partially covers, over the rows actually returned, so the simple version
/// costs nothing worth optimising away.
std::vector<std::uint8_t> slice_bitmap(const std::vector<std::uint8_t>& src, std::uint64_t first,
                                       std::uint64_t count, std::uint64_t& out_null_count) {
    std::vector<std::uint8_t> out(bitmap_bytes(count), 0U);
    out_null_count = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (bit_set(src, first + i)) {
            out[static_cast<std::size_t>(i >> 3U)] |= static_cast<std::uint8_t>(1U << (i & 7U));
        } else {
            ++out_null_count;
        }
    }
    return out;
}

std::uint64_t read_offset(const std::vector<std::uint8_t>& offsets, std::uint64_t index, bool large) {
    const auto width = large ? 8U : 4U;
    const auto* p = offsets.data() + static_cast<std::size_t>(index) * width;
    if (large) {
        std::int64_t value = 0;
        std::memcpy(&value, p, sizeof(value));
        return static_cast<std::uint64_t>(value);
    }
    std::int32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return static_cast<std::uint64_t>(value);
}

void write_offset(std::uint8_t* dst, std::uint64_t value, bool large) {
    if (large) {
        const auto narrowed = static_cast<std::int64_t>(value);
        std::memcpy(dst, &narrowed, sizeof(narrowed));
    } else {
        const auto narrowed = static_cast<std::int32_t>(value);
        std::memcpy(dst, &narrowed, sizeof(narrowed));
    }
}

}  // namespace

namespace {

bool slice_leaf(ColumnValues& values, std::uint64_t first, std::uint64_t count, std::uint64_t total,
                std::size_t value_bytes, std::string& error) {
    if (first > total || count > total - first) {
        error = "row range is outside the column";
        return false;
    }
    // The read side never borrows (that is a write-side ingest optimisation), and slicing a borrowed
    // view would hand Arrow a pointer into memory this function does not own.
    if (values.fixed_borrowed != nullptr) {
        error = "cannot slice a column holding a borrowed buffer";
        return false;
    }

    // Validate every buffer BEFORE mutating any of them, so a rejected slice leaves the column as it
    // was rather than half-trimmed.
    if (!values.validity.empty() && values.validity.size() < bitmap_bytes(total)) {
        error = "validity bitmap covers fewer rows than the column claims";
        return false;
    }
    switch (values.kind) {
        case ColumnValues::Kind::FixedWidth:
            if (!values.fixed.empty()) {
                if (value_bytes == 0U) {
                    error = "fixed-width column has no value width";
                    return false;
                }
                if (values.fixed.size() / value_bytes < total) {
                    error = "fixed-width buffer is shorter than the rows the column claims";
                    return false;
                }
            }
            break;
        case ColumnValues::Kind::VariableWidth: {
            const std::size_t offset_width = values.variable.large ? 8U : 4U;
            if (values.variable.offsets.size() < (total + 1U) * offset_width) {
                error = "variable-width offsets cover fewer rows than the column claims";
                return false;
            }
            const auto end = read_offset(values.variable.offsets, first + count, values.variable.large);
            if (end > values.variable.data.size()) {
                error = "variable-width offset runs past the data buffer";
                return false;
            }
            break;
        }
        case ColumnValues::Kind::BlobV2External:
            if (values.blob_v2.row_packed_sizes.size() < total) {
                error = "blob column has fewer packed rows than it claims";
                return false;
            }
            break;
    }

    if (!values.item_validity.empty()) {
        // items_per_row bits per row; the row range selects a contiguous run of them.
        const auto per_row = values.items_per_row;
        if (per_row == 0U || values.item_validity.size() < bitmap_bytes(total * per_row)) {
            error = "element validity bitmap covers fewer elements than the column claims";
            return false;
        }
        std::uint64_t item_nulls = 0;
        values.item_validity = slice_bitmap(values.item_validity, first * per_row, count * per_row, item_nulls);
        values.item_null_count = item_nulls;
        if (item_nulls == 0U) {
            values.item_validity.clear();
        }
    }

    if (!values.validity.empty()) {
        std::uint64_t nulls = 0;
        values.validity = slice_bitmap(values.validity, first, count, nulls);
        values.null_count = nulls;
        if (nulls == 0U) {
            // An all-valid slice of a nullable column: drop the bitmap entirely, which is what the
            // rest of the reader means by "no nulls" and what Arrow prefers.
            values.validity.clear();
        }
    }

    switch (values.kind) {
        case ColumnValues::Kind::FixedWidth: {
            if (!values.fixed.empty()) {
                // In place: a copy would drop the buffer's capacity, which the buffer pool hands the
                // next read (a parallel read slices every row range it decodes).
                const auto begin = static_cast<std::size_t>(first) * value_bytes;
                const auto bytes = static_cast<std::size_t>(count) * value_bytes;
                if (begin != 0U && bytes != 0U) {
                    std::memmove(values.fixed.data(), values.fixed.data() + begin, bytes);
                }
                values.fixed.resize(bytes);
            }
            break;
        }
        case ColumnValues::Kind::VariableWidth: {
            const bool large = values.variable.large;
            const std::size_t offset_width = large ? 8U : 4U;
            const auto data_begin = read_offset(values.variable.offsets, first, large);
            const auto data_end = read_offset(values.variable.offsets, first + count, large);

            // In place, front to back: offset i is read (at first + i) before slot i is written.
            auto& offsets = values.variable.offsets;
            for (std::uint64_t i = 0; i <= count; ++i) {
                const auto raw = read_offset(offsets, first + i, large);
                write_offset(offsets.data() + static_cast<std::size_t>(i) * offset_width, raw - data_begin, large);
            }
            offsets.resize(static_cast<std::size_t>(count + 1U) * offset_width);
            auto& data = values.variable.data;
            const auto data_bytes = static_cast<std::size_t>(data_end - data_begin);
            if (data_begin != 0U && data_bytes != 0U) {
                std::memmove(data.data(), data.data() + data_begin, data_bytes);
            }
            data.resize(data_bytes);
            break;
        }
        case ColumnValues::Kind::BlobV2External: {
            std::uint64_t byte_begin = 0;
            for (std::uint64_t i = 0; i < first; ++i) {
                byte_begin += values.blob_v2.row_packed_sizes[static_cast<std::size_t>(i)];
            }
            std::uint64_t byte_end = byte_begin;
            for (std::uint64_t i = first; i < first + count; ++i) {
                byte_end += values.blob_v2.row_packed_sizes[static_cast<std::size_t>(i)];
            }
            if (byte_end > values.blob_v2.packed_payload.size()) {
                error = "blob packed rows run past the payload buffer";
                return false;
            }
            std::vector<std::uint8_t> payload(
                values.blob_v2.packed_payload.begin() + static_cast<std::ptrdiff_t>(byte_begin),
                values.blob_v2.packed_payload.begin() + static_cast<std::ptrdiff_t>(byte_end));
            std::vector<std::uint32_t> sizes(
                values.blob_v2.row_packed_sizes.begin() + static_cast<std::ptrdiff_t>(first),
                values.blob_v2.row_packed_sizes.begin() + static_cast<std::ptrdiff_t>(first + count));
            values.blob_v2.packed_payload = std::move(payload);
            values.blob_v2.row_packed_sizes = std::move(sizes);
            break;
        }
    }

    values.rows = count;
    // These hold string_views into the buffers just replaced above. Nothing on the read side computes
    // them, but a dangling view is not the kind of thing to leave lying next to a buffer swap.
    values.structural_dict_plan = StructuralDictPlan{};
    values.structural_dict_rle_plan = StructuralDictRlePlan{};
    values.fixed_rle_plan = FixedRlePlan{};
    return true;
}

/// Copy `count` bits of `src` from `from` to `dst` (zeroed) at `at`; returns how many were 0.
std::uint64_t copy_bits(const std::vector<std::uint8_t>& src, std::uint64_t from, std::uint64_t count,
                        std::vector<std::uint8_t>& dst, std::uint64_t at) {
    std::uint64_t zeros = 0;
    std::uint64_t i = 0;
    if (((from | at) & 7U) == 0U) {  // byte-aligned: whole bytes at a time
        const auto bytes = static_cast<std::size_t>(count >> 3U);
        const auto* in = src.data() + (from >> 3U);
        auto* out = dst.data() + (at >> 3U);
        std::memcpy(out, in, bytes);
        for (std::size_t k = 0; k < bytes; ++k) {
            zeros += 8U - static_cast<std::uint64_t>(std::popcount(in[k]));
        }
        i = static_cast<std::uint64_t>(bytes) << 3U;
    }
    for (; i < count; ++i) {
        if (bit_set(src, from + i)) {
            const auto d = at + i;
            dst[static_cast<std::size_t>(d >> 3U)] |= static_cast<std::uint8_t>(1U << (d & 7U));
        } else {
            ++zeros;
        }
    }
    return zeros;
}

/// Kept rows as runs [begin, end): a take, a range or a deletion file keeps long runs of a list's
/// items, which are moved a run at a time.
using RowRuns = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

RowRuns runs_of(const std::vector<std::uint8_t>& keep) {
    RowRuns runs;
    const auto total = static_cast<std::uint64_t>(keep.size());
    for (std::uint64_t i = 0; i < total;) {
        const auto* zero = static_cast<const std::uint8_t*>(
            std::memchr(keep.data() + i, 0, static_cast<std::size_t>(total - i)));
        const auto end = zero == nullptr ? total : static_cast<std::uint64_t>(zero - keep.data());
        if (end > i) {
            runs.emplace_back(i, end);
        }
        i = end;
        while (i < total && keep[static_cast<std::size_t>(i)] == 0U) {
            ++i;
        }
    }
    return runs;
}

/// Keep `runs` (ascending, disjoint) of a leaf's `total` rows, moving them to the front in place.
bool compact_leaf_runs(ColumnValues& values, const RowRuns& runs, std::uint64_t total, std::size_t value_bytes,
                       std::string& error) {
    if (values.fixed_borrowed != nullptr) {
        error = "cannot compact a column holding a borrowed buffer";
        return false;
    }

    std::uint64_t count = 0;
    for (const auto& [b, e] : runs) {
        if (b >= e || e > total) {
            error = "kept rows are outside the column";
            return false;
        }
        count += e - b;
    }
    if (count == total) {
        return true;  // nothing dropped; leave every buffer untouched
    }
    std::vector<std::uint64_t> kept;
    const auto kept_rows = [&]() -> const std::vector<std::uint64_t>& {
        if (kept.size() != count) {
            kept.clear();
            kept.reserve(static_cast<std::size_t>(count));
            for (const auto& [b, e] : runs) {
                for (auto r = b; r < e; ++r) {
                    kept.push_back(r);
                }
            }
        }
        return kept;
    };

    // Validate before mutating, so a rejected compaction leaves the column as it was.
    if (!values.validity.empty() && values.validity.size() < bitmap_bytes(total)) {
        error = "validity bitmap covers fewer rows than the column claims";
        return false;
    }
    switch (values.kind) {
        case ColumnValues::Kind::FixedWidth:
            if (!values.fixed.empty() && (value_bytes == 0U || values.fixed.size() / value_bytes < total)) {
                error = "fixed-width buffer is shorter than the rows the column claims";
                return false;
            }
            break;
        case ColumnValues::Kind::VariableWidth: {
            const std::size_t offset_width = values.variable.large ? 8U : 4U;
            if (values.variable.offsets.size() < (total + 1U) * offset_width) {
                error = "variable-width offsets cover fewer rows than the column claims";
                return false;
            }
            for (const auto& [b, e] : runs) {
                auto previous = read_offset(values.variable.offsets, b, values.variable.large);
                for (auto row = b; row < e; ++row) {
                    const auto end = read_offset(values.variable.offsets, row + 1U, values.variable.large);
                    if (end < previous || end > values.variable.data.size()) {
                        error = "variable-width offset runs past the data buffer";
                        return false;
                    }
                    previous = end;
                }
            }
            break;
        }
        case ColumnValues::Kind::BlobV2External:
            if (values.blob_v2.row_packed_sizes.size() < total) {
                error = "blob column has fewer packed rows than it claims";
                return false;
            }
            break;
    }

    if (!values.item_validity.empty()) {
        const auto per_row = values.items_per_row;
        if (per_row == 0U || values.item_validity.size() < bitmap_bytes(total * per_row)) {
            error = "element validity bitmap covers fewer elements than the column claims";
            return false;
        }
        std::vector<std::uint8_t> bitmap(bitmap_bytes(count * per_row), 0U);
        std::uint64_t item_nulls = 0;
        std::uint64_t dst = 0;
        for (const auto& [b, e] : runs) {
            item_nulls += copy_bits(values.item_validity, b * per_row, (e - b) * per_row, bitmap, dst);
            dst += (e - b) * per_row;
        }
        values.item_null_count = item_nulls;
        values.item_validity = item_nulls == 0U ? std::vector<std::uint8_t>{} : std::move(bitmap);
    }

    if (!values.validity.empty()) {
        std::vector<std::uint8_t> bitmap(bitmap_bytes(count), 0U);
        std::uint64_t nulls = 0;
        std::uint64_t dst = 0;
        for (const auto& [b, e] : runs) {
            nulls += copy_bits(values.validity, b, e - b, bitmap, dst);
            dst += e - b;
        }
        values.null_count = nulls;
        values.validity = nulls == 0U ? std::vector<std::uint8_t>{} : std::move(bitmap);
    }

    switch (values.kind) {
        case ColumnValues::Kind::FixedWidth: {
            if (!values.fixed.empty()) {
                // In place: every run moves toward the front, never past a byte still to be read.
                std::size_t at = 0;
                for (const auto& [b, e] : runs) {
                    const auto bytes = static_cast<std::size_t>(e - b) * value_bytes;
                    const auto from = static_cast<std::size_t>(b) * value_bytes;
                    if (from != at) {
                        std::memmove(values.fixed.data() + at, values.fixed.data() + from, bytes);
                    }
                    at += bytes;
                }
                values.fixed.resize(at);
            }
            break;
        }
        case ColumnValues::Kind::VariableWidth: {
            const bool large = values.variable.large;
            const std::size_t offset_width = large ? 8U : 4U;
            // In place, as for fixed width: row i's new offset slot is at or before its old one, and
            // each run's bytes move toward the front. Every offset a run needs is read before any
            // slot at or after the run's start is written.
            auto& offsets = values.variable.offsets;
            auto& data = values.variable.data;
            std::uint64_t cumulative = 0;
            std::uint64_t i = 0;
            for (const auto& [b, e] : runs) {
                const auto run_begin = read_offset(offsets, b, large);
                const auto run_end = read_offset(offsets, e, large);  // validated above
                const auto shift = run_begin - cumulative;
                for (auto row = b; row < e; ++row) {
                    const auto end = read_offset(offsets, row + 1U, large);
                    write_offset(offsets.data() + static_cast<std::size_t>(++i) * offset_width, end - shift, large);
                }
                if (shift != 0U) {
                    std::memmove(data.data() + cumulative, data.data() + run_begin,
                                 static_cast<std::size_t>(run_end - run_begin));
                }
                cumulative += run_end - run_begin;
            }
            write_offset(offsets.data(), 0U, large);
            offsets.resize(static_cast<std::size_t>(count + 1U) * offset_width);
            data.resize(static_cast<std::size_t>(cumulative));
            break;
        }
        case ColumnValues::Kind::BlobV2External: {
            std::vector<std::uint64_t> starts(static_cast<std::size_t>(total) + 1U, 0U);
            for (std::uint64_t i = 0; i < total; ++i) {
                starts[static_cast<std::size_t>(i) + 1U] =
                    starts[static_cast<std::size_t>(i)] + values.blob_v2.row_packed_sizes[static_cast<std::size_t>(i)];
            }
            if (starts.back() > values.blob_v2.packed_payload.size()) {
                error = "blob packed rows run past the payload buffer";
                return false;
            }
            std::vector<std::uint8_t> payload;
            std::vector<std::uint32_t> sizes;
            sizes.reserve(static_cast<std::size_t>(count));
            const auto& kept_list = kept_rows();
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto row = static_cast<std::size_t>(kept_list[static_cast<std::size_t>(i)]);
                payload.insert(payload.end(),
                               values.blob_v2.packed_payload.begin() + static_cast<std::ptrdiff_t>(starts[row]),
                               values.blob_v2.packed_payload.begin() + static_cast<std::ptrdiff_t>(starts[row + 1U]));
                sizes.push_back(values.blob_v2.row_packed_sizes[row]);
            }
            values.blob_v2.packed_payload = std::move(payload);
            values.blob_v2.row_packed_sizes = std::move(sizes);
            break;
        }
    }

    values.rows = count;
    values.structural_dict_plan = StructuralDictPlan{};
    values.structural_dict_rle_plan = StructuralDictRlePlan{};
    values.fixed_rle_plan = FixedRlePlan{};
    return true;
}

bool compact_leaf(ColumnValues& values, const std::vector<std::uint8_t>& keep, std::uint64_t total,
                  std::size_t value_bytes, std::string& error) {
    if (keep.size() != total) {
        error = "keep mask does not cover the column's rows";
        return false;
    }
    return compact_leaf_runs(values, runs_of(keep), total, value_bytes, error);
}


/// Check a list column's layers describe a consistent tree before cutting it: each layer's offsets
/// have one more entry than it has lists, start at 0, never decrease, and end at the next layer's
/// length (the leaf's item count, for the innermost). Returns that item count.
bool check_layers(const std::vector<ColumnValues::NestedLayer>& layers, std::uint64_t rows, std::uint64_t& items,
                  std::string& error) {
    if (layers.front().length != rows) {
        error = "list column holds " + std::to_string(layers.front().length) + " rows, not " + std::to_string(rows);
        return false;
    }
    for (std::size_t k = 0; k < layers.size(); ++k) {
        const auto& layer = layers[k];
        if (!layer.validity.empty() && layer.validity.size() < bitmap_bytes(layer.length)) {
            error = "nested layer " + std::to_string(k) + " is malformed";
            return false;
        }
        if (!layer.is_list) {
            // A struct: one child per entry.
            if (!layer.offsets.empty() || (k + 1U < layers.size() && layers[k + 1U].length != layer.length)) {
                error = "struct layer " + std::to_string(k) + " is malformed";
                return false;
            }
            continue;
        }
        if (layer.offsets.size() != layer.length + 1U || layer.offsets.front() != 0) {
            error = "list layer " + std::to_string(k) + " is malformed";
            return false;
        }
        for (std::size_t i = 1; i < layer.offsets.size(); ++i) {
            if (layer.offsets[i] < layer.offsets[i - 1U]) {
                error = "list layer " + std::to_string(k) + " has decreasing offsets";
                return false;
            }
        }
        const auto end = static_cast<std::uint64_t>(layer.offsets.back());
        if (k + 1U < layers.size() && end != layers[k + 1U].length) {
            error = "list layer " + std::to_string(k) + " ends past its children";
            return false;
        }
    }
    items = layers.back().is_list ? static_cast<std::uint64_t>(layers.back().offsets.back()) : layers.back().length;
    return true;
}

}  // namespace

bool slice_column_values(ColumnValues& values, std::uint64_t first, std::uint64_t count,
                         std::uint64_t total, std::size_t value_bytes, std::string& error) {
    if (values.layers.empty()) {
        return slice_leaf(values, first, count, total, value_bytes, error);
    }
    // A list column: the row range selects entries of the outermost layer, whose offsets select a
    // contiguous range of the next layer's entries, and so on down to the items.
    if (first > total || count > total - first) {
        error = "row range is outside the column";
        return false;
    }
    std::uint64_t items = 0;
    if (!check_layers(values.layers, total, items, error)) {
        return false;
    }
    std::vector<ColumnValues::NestedLayer> cut(values.layers.size());
    std::uint64_t at = first;
    std::uint64_t n = count;
    for (std::size_t k = 0; k < values.layers.size(); ++k) {
        const auto& layer = values.layers[k];
        auto& out = cut[k];
        out.is_list = layer.is_list;
        out.length = n;
        if (!layer.is_list) {
            // A struct passes the same range straight to its children.
            if (!layer.validity.empty()) {
                out.validity = slice_bitmap(layer.validity, at, n, out.null_count);
                if (out.null_count == 0U) {
                    out.validity.clear();
                }
            }
            continue;
        }
        const auto base = layer.offsets[static_cast<std::size_t>(at)];
        out.offsets.reserve(static_cast<std::size_t>(n + 1U));
        for (std::uint64_t i = 0; i <= n; ++i) {
            out.offsets.push_back(layer.offsets[static_cast<std::size_t>(at + i)] - base);
        }
        if (!layer.validity.empty()) {
            out.validity = slice_bitmap(layer.validity, at, n, out.null_count);
            if (out.null_count == 0U) {
                out.validity.clear();
            }
        }
        at = static_cast<std::uint64_t>(base);
        n = static_cast<std::uint64_t>(out.offsets.back());
    }
    if (!slice_leaf(values, at, n, items, value_bytes, error)) {
        return false;
    }
    values.layers = std::move(cut);
    return true;
}

bool compact_column_values(ColumnValues& values, const std::vector<std::uint8_t>& keep,
                           std::uint64_t total, std::size_t value_bytes, std::string& error) {
    if (values.layers.empty()) {
        return compact_leaf(values, keep, total, value_bytes, error);
    }
    if (keep.size() != total) {
        error = "keep mask does not cover the column's rows";
        return false;
    }
    if (std::find(keep.begin(), keep.end(), std::uint8_t{0}) == keep.end()) {
        return true;  // every row kept
    }
    std::uint64_t items = 0;
    if (!check_layers(values.layers, total, items, error)) {
        return false;
    }
    // Each layer turns "which of my entries survive" into "which of my children survive": a deleted
    // row drops every list and item under it, however deep. Survivors are carried as runs, so a
    // long list costs one run, not a flag per item.
    std::vector<ColumnValues::NestedLayer> cut(values.layers.size());
    RowRuns here = runs_of(keep);
    for (std::size_t k = 0; k < values.layers.size(); ++k) {
        const auto& layer = values.layers[k];
        auto& out = cut[k];
        out.is_list = layer.is_list;
        RowRuns children;
        if (layer.is_list) {
            out.offsets.push_back(0);
        }
        for (const auto& [b, e] : here) {
            if (layer.is_list) {
                // A run of lists keeps one contiguous run of children.
                const auto first = static_cast<std::uint64_t>(layer.offsets[static_cast<std::size_t>(b)]);
                const auto last = static_cast<std::uint64_t>(layer.offsets[static_cast<std::size_t>(e)]);
                if (last > first) {
                    if (!children.empty() && children.back().second == first) {
                        children.back().second = last;
                    } else {
                        children.emplace_back(first, last);
                    }
                }
            } else {
                children.emplace_back(b, e);  // a struct keeps exactly the entries it is told to
            }
            for (auto i = b; i < e; ++i) {
                if (layer.is_list) {
                    const auto begin = layer.offsets[static_cast<std::size_t>(i)];
                    const auto end = layer.offsets[static_cast<std::size_t>(i + 1U)];
                    out.offsets.push_back(out.offsets.back() + (end - begin));
                }
                const bool valid = layer.validity.empty() || bit_set(layer.validity, i);
                if (!valid && out.validity.empty()) {
                    out.validity.assign(bitmap_bytes(out.length + 1U), 0U);
                    for (std::uint64_t v = 0; v < out.length; ++v) {
                        out.validity[static_cast<std::size_t>(v >> 3U)] |= static_cast<std::uint8_t>(1U << (v & 7U));
                    }
                }
                if (!out.validity.empty()) {
                    out.validity.resize(bitmap_bytes(out.length + 1U), 0U);
                    if (valid) {
                        out.validity[static_cast<std::size_t>(out.length >> 3U)] |=
                            static_cast<std::uint8_t>(1U << (out.length & 7U));
                    } else {
                        ++out.null_count;
                    }
                }
                ++out.length;
            }
        }
        here = std::move(children);
    }
    if (!compact_leaf_runs(values, here, items, value_bytes, error)) {
        return false;
    }
    values.layers = std::move(cut);
    return true;
}

}  // namespace nano_lance
