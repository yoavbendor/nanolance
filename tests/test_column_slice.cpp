// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// slice_column_values() is the row-range read's only sharp edge, so it is brute-forced here rather
// than spot-checked: every (total, first, count) combination up to a size that covers several
// bitmap bytes, compared against a reference built from plain per-row vectors.
//
// The validity bitmap is the reason. Value buffers are byte-addressed at this stage (bool included
// -- it is a byte per value until the Arrow buffer is assembled), but the bitmap is LSB-first bits,
// so any `first % 8 != 0` shifts every bit. A fragment boundary lands on such an offset seven times
// out of eight.

#include "nanolance/column_slice.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string where(std::uint64_t total, std::uint64_t first, std::uint64_t count) {
    return " (total=" + std::to_string(total) + " first=" + std::to_string(first) +
           " count=" + std::to_string(count) + ")";
}

// --- reference model: one entry per row, no packing anywhere ------------------------------------

bool row_is_valid(std::uint64_t row) {
    // Deliberately not a multiple or divisor of 8, so validity does not line up with byte edges.
    return row % 3U != 0U;
}

std::vector<std::uint8_t> pack_validity(std::uint64_t first, std::uint64_t count) {
    std::vector<std::uint8_t> out((count + 7U) / 8U, 0U);
    for (std::uint64_t i = 0; i < count; ++i) {
        if (row_is_valid(first + i)) {
            out[static_cast<std::size_t>(i >> 3U)] |= static_cast<std::uint8_t>(1U << (i & 7U));
        }
    }
    return out;
}

std::uint64_t expected_nulls(std::uint64_t first, std::uint64_t count) {
    std::uint64_t nulls = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!row_is_valid(first + i)) {
            ++nulls;
        }
    }
    return nulls;
}

std::string string_for(std::uint64_t row) {
    // Varying lengths, including empty, so offset rebasing cannot pass by accident on uniform rows.
    if (row % 5U == 0U) {
        return "";
    }
    return std::string(static_cast<std::size_t>(row % 7U) + 1U, static_cast<char>('a' + (row % 26U)));
}

// --- builders ------------------------------------------------------------------------------------

constexpr std::size_t kValueBytes = 4;

nano_lance::ColumnValues make_fixed(std::uint64_t total, bool nullable) {
    nano_lance::ColumnValues values;
    values.kind = nano_lance::ColumnValues::Kind::FixedWidth;
    values.rows = total;
    values.fixed.resize(static_cast<std::size_t>(total) * kValueBytes);
    for (std::uint64_t i = 0; i < total; ++i) {
        const auto value = static_cast<std::uint32_t>(i * 7U + 1U);
        std::memcpy(values.fixed.data() + static_cast<std::size_t>(i) * kValueBytes, &value, kValueBytes);
    }
    if (nullable) {
        values.validity = pack_validity(0, total);
        values.null_count = expected_nulls(0, total);
    }
    return values;
}

nano_lance::ColumnValues make_variable(std::uint64_t total, bool large, bool nullable) {
    nano_lance::ColumnValues values;
    values.kind = nano_lance::ColumnValues::Kind::VariableWidth;
    values.variable.large = large;
    values.rows = total;
    const std::size_t width = large ? 8U : 4U;
    values.variable.offsets.resize((static_cast<std::size_t>(total) + 1U) * width);
    std::uint64_t cumulative = 0;
    auto put = [&](std::uint64_t index, std::uint64_t value) {
        if (large) {
            const auto v = static_cast<std::int64_t>(value);
            std::memcpy(values.variable.offsets.data() + static_cast<std::size_t>(index) * width, &v, sizeof(v));
        } else {
            const auto v = static_cast<std::int32_t>(value);
            std::memcpy(values.variable.offsets.data() + static_cast<std::size_t>(index) * width, &v, sizeof(v));
        }
    };
    put(0, 0);
    for (std::uint64_t i = 0; i < total; ++i) {
        const auto s = string_for(i);
        values.variable.data.insert(values.variable.data.end(), s.begin(), s.end());
        cumulative += s.size();
        put(i + 1U, cumulative);
    }
    if (nullable) {
        values.validity = pack_validity(0, total);
        values.null_count = expected_nulls(0, total);
    }
    return values;
}

// --- checks --------------------------------------------------------------------------------------

void check_validity(const nano_lance::ColumnValues& sliced, std::uint64_t first, std::uint64_t count,
                    const std::string& ctx) {
    const auto nulls = expected_nulls(first, count);
    require(sliced.null_count == nulls,
            "null_count " + std::to_string(sliced.null_count) + " != " + std::to_string(nulls) + ctx);
    if (nulls == 0U) {
        // An all-valid slice drops the bitmap: that is what the rest of the reader means by "no nulls".
        require(sliced.validity.empty(), "an all-valid slice must drop its bitmap" + ctx);
        return;
    }
    const auto expected = pack_validity(first, count);
    require(sliced.validity.size() == expected.size(), "sliced bitmap has the wrong byte length" + ctx);
    // Compare BIT by bit rather than byte by byte: the trailing bits of the last byte are padding and
    // a byte comparison would let a wrong padding value fail a correct slice (or hide a wrong one).
    for (std::uint64_t i = 0; i < count; ++i) {
        const bool got = (sliced.validity[static_cast<std::size_t>(i >> 3U)] &
                          static_cast<std::uint8_t>(1U << (i & 7U))) != 0U;
        require(got == row_is_valid(first + i),
                "validity bit " + std::to_string(i) + " is wrong" + ctx);
    }
}

void check_fixed(std::uint64_t total, std::uint64_t first, std::uint64_t count, bool nullable) {
    const auto ctx = where(total, first, count) + (nullable ? " nullable" : "");
    auto values = make_fixed(total, nullable);
    std::string error;
    require(nano_lance::slice_column_values(values, first, count, total, kValueBytes, error), error + ctx);

    require(values.rows == count, "rows not updated" + ctx);
    require(values.fixed.size() == static_cast<std::size_t>(count) * kValueBytes,
            "sliced buffer has the wrong length" + ctx);
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint32_t got = 0;
        std::memcpy(&got, values.fixed.data() + static_cast<std::size_t>(i) * kValueBytes, kValueBytes);
        require(got == static_cast<std::uint32_t>((first + i) * 7U + 1U),
                "value " + std::to_string(i) + " is wrong" + ctx);
    }
    if (nullable) {
        check_validity(values, first, count, ctx);
    }
}

void check_variable(std::uint64_t total, std::uint64_t first, std::uint64_t count, bool large,
                    bool nullable) {
    const auto ctx = where(total, first, count) + (large ? " large" : " small") +
                     (nullable ? " nullable" : "");
    auto values = make_variable(total, large, nullable);
    std::string error;
    require(nano_lance::slice_column_values(values, first, count, total, 0U, error), error + ctx);

    const std::size_t width = large ? 8U : 4U;
    require(values.variable.offsets.size() == (static_cast<std::size_t>(count) + 1U) * width,
            "sliced offsets have the wrong length" + ctx);
    auto offset_at = [&](std::uint64_t index) -> std::uint64_t {
        if (large) {
            std::int64_t v = 0;
            std::memcpy(&v, values.variable.offsets.data() + static_cast<std::size_t>(index) * width, sizeof(v));
            return static_cast<std::uint64_t>(v);
        }
        std::int32_t v = 0;
        std::memcpy(&v, values.variable.offsets.data() + static_cast<std::size_t>(index) * width, sizeof(v));
        return static_cast<std::uint64_t>(v);
    };
    require(offset_at(0) == 0U, "sliced offsets must be rebased to 0" + ctx);
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto begin = offset_at(i);
        const auto end = offset_at(i + 1U);
        require(end >= begin, "offsets are not monotonic" + ctx);
        require(end <= values.variable.data.size(), "offset runs past the sliced data" + ctx);
        const std::string got(reinterpret_cast<const char*>(values.variable.data.data()) + begin,
                              static_cast<std::size_t>(end - begin));
        require(got == string_for(first + i), "value " + std::to_string(i) + " is wrong" + ctx);
    }
    require(offset_at(count) == values.variable.data.size(),
            "the terminal offset must equal the sliced data length" + ctx);
    if (nullable) {
        check_validity(values, first, count, ctx);
    }
}

}  // namespace

int main() {
    // Brute force. 40 rows spans five bitmap bytes, so every (first % 8, count % 8) pair appears many
    // times over, including counts that end mid-byte and ranges that start and end in the same byte.
    constexpr std::uint64_t kMaxTotal = 40;
    for (std::uint64_t total = 0; total <= kMaxTotal; ++total) {
        for (std::uint64_t first = 0; first <= total; ++first) {
            for (std::uint64_t count = 0; count + first <= total; ++count) {
                for (const bool nullable : {false, true}) {
                    check_fixed(total, first, count, nullable);
                    check_variable(total, first, count, /*large=*/false, nullable);
                    check_variable(total, first, count, /*large=*/true, nullable);
                }
            }
        }
    }

    // Out-of-range requests are refused, and refusal leaves the column untouched rather than
    // half-trimmed -- a caller that ignores the error must not end up with a silently wrong column.
    {
        auto values = make_fixed(10, /*nullable=*/true);
        const auto before_fixed = values.fixed;
        const auto before_validity = values.validity;
        std::string error;
        require(!nano_lance::slice_column_values(values, 8, 5, 10, kValueBytes, error),
                "a range past the end must be refused");
        require(!error.empty(), "a refused slice must say why");
        require(values.fixed == before_fixed && values.validity == before_validity,
                "a refused slice must leave the column untouched");
        require(!nano_lance::slice_column_values(values, 11, 0, 10, kValueBytes, error),
                "an offset past the end must be refused");
    }

    // A borrowed buffer is a write-side ingest optimisation; slicing one would hand Arrow a pointer
    // into memory this code does not own.
    {
        nano_lance::ColumnValues values;
        const std::vector<std::uint8_t> owned(40, 0U);
        values.fixed_borrowed = owned.data();
        values.fixed_borrowed_size = owned.size();
        std::string error;
        require(!nano_lance::slice_column_values(values, 0, 1, 10, kValueBytes, error),
                "slicing a borrowed buffer must be refused");
    }

    // A column with no nulls has no bitmap to begin with; slicing must not invent one.
    {
        auto values = make_fixed(20, /*nullable=*/false);
        std::string error;
        require(nano_lance::slice_column_values(values, 3, 7, 20, kValueBytes, error), error);
        require(values.validity.empty() && values.null_count == 0U,
                "slicing a null-free column must not produce a bitmap");
    }

    std::cout << "column slice ok\n";
    return 0;
}
