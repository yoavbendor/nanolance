// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/column_slice.hpp"

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

bool slice_column_values(ColumnValues& values, std::uint64_t first, std::uint64_t count,
                         std::uint64_t total, std::size_t value_bytes, std::string& error) {
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
                const auto begin = static_cast<std::size_t>(first) * value_bytes;
                const auto bytes = static_cast<std::size_t>(count) * value_bytes;
                std::vector<std::uint8_t> sliced(values.fixed.begin() + static_cast<std::ptrdiff_t>(begin),
                                                 values.fixed.begin() +
                                                     static_cast<std::ptrdiff_t>(begin + bytes));
                values.fixed = std::move(sliced);
            }
            break;
        }
        case ColumnValues::Kind::VariableWidth: {
            const bool large = values.variable.large;
            const std::size_t offset_width = large ? 8U : 4U;
            const auto data_begin = read_offset(values.variable.offsets, first, large);
            const auto data_end = read_offset(values.variable.offsets, first + count, large);

            std::vector<std::uint8_t> offsets((count + 1U) * offset_width);
            for (std::uint64_t i = 0; i <= count; ++i) {
                const auto raw = read_offset(values.variable.offsets, first + i, large);
                write_offset(offsets.data() + static_cast<std::size_t>(i) * offset_width,
                             raw - data_begin, large);
            }
            std::vector<std::uint8_t> data(
                values.variable.data.begin() + static_cast<std::ptrdiff_t>(data_begin),
                values.variable.data.begin() + static_cast<std::ptrdiff_t>(data_end));
            values.variable.offsets = std::move(offsets);
            values.variable.data = std::move(data);
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

bool compact_column_values(ColumnValues& values, const std::vector<std::uint8_t>& keep,
                           std::uint64_t total, std::size_t value_bytes, std::string& error) {
    if (keep.size() != total) {
        error = "keep mask does not cover the column's rows";
        return false;
    }
    if (values.fixed_borrowed != nullptr) {
        error = "cannot compact a column holding a borrowed buffer";
        return false;
    }

    std::vector<std::uint64_t> kept;
    kept.reserve(static_cast<std::size_t>(total));
    for (std::uint64_t i = 0; i < total; ++i) {
        if (keep[static_cast<std::size_t>(i)] != 0U) {
            kept.push_back(i);
        }
    }
    if (kept.size() == total) {
        return true;  // nothing deleted in this column's rows; leave every buffer untouched
    }
    const auto count = static_cast<std::uint64_t>(kept.size());

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
            break;
        }
        case ColumnValues::Kind::BlobV2External:
            if (values.blob_v2.row_packed_sizes.size() < total) {
                error = "blob column has fewer packed rows than it claims";
                return false;
            }
            break;
    }

    if (!values.validity.empty()) {
        std::vector<std::uint8_t> bitmap(bitmap_bytes(count), 0U);
        std::uint64_t nulls = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            if (bit_set(values.validity, kept[static_cast<std::size_t>(i)])) {
                bitmap[static_cast<std::size_t>(i >> 3U)] |= static_cast<std::uint8_t>(1U << (i & 7U));
            } else {
                ++nulls;
            }
        }
        values.null_count = nulls;
        values.validity = nulls == 0U ? std::vector<std::uint8_t>{} : std::move(bitmap);
    }

    switch (values.kind) {
        case ColumnValues::Kind::FixedWidth: {
            if (!values.fixed.empty()) {
                std::vector<std::uint8_t> out(static_cast<std::size_t>(count) * value_bytes);
                for (std::uint64_t i = 0; i < count; ++i) {
                    std::memcpy(out.data() + static_cast<std::size_t>(i) * value_bytes,
                                values.fixed.data() + static_cast<std::size_t>(kept[static_cast<std::size_t>(i)]) * value_bytes,
                                value_bytes);
                }
                values.fixed = std::move(out);
            }
            break;
        }
        case ColumnValues::Kind::VariableWidth: {
            const bool large = values.variable.large;
            const std::size_t offset_width = large ? 8U : 4U;
            std::vector<std::uint8_t> offsets((count + 1U) * offset_width);
            std::vector<std::uint8_t> data;
            std::uint64_t cumulative = 0;
            write_offset(offsets.data(), 0U, large);
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto row = kept[static_cast<std::size_t>(i)];
                const auto begin = read_offset(values.variable.offsets, row, large);
                const auto end = read_offset(values.variable.offsets, row + 1U, large);
                if (end < begin || end > values.variable.data.size()) {
                    error = "variable-width offset runs past the data buffer";
                    return false;
                }
                data.insert(data.end(), values.variable.data.begin() + static_cast<std::ptrdiff_t>(begin),
                            values.variable.data.begin() + static_cast<std::ptrdiff_t>(end));
                cumulative += end - begin;
                write_offset(offsets.data() + static_cast<std::size_t>(i + 1U) * offset_width, cumulative, large);
            }
            values.variable.offsets = std::move(offsets);
            values.variable.data = std::move(data);
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
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto row = static_cast<std::size_t>(kept[static_cast<std::size_t>(i)]);
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

}  // namespace nano_lance
