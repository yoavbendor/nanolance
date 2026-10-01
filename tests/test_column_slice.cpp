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
#include <random>
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

    // gather_column_values must give exactly what copying the column and compacting it gives --
    // flat, variable-width, fixed_size_list with element nulls, and lists of lists and of structs --
    // and leave its source as it was (it reads a cached decode that other takes share).
    {
        std::mt19937_64 rng(42);
        const auto bit = [&](unsigned percent) { return rng() % 100U < percent; };
        const auto bitmap = [&](std::uint64_t n, unsigned null_percent, std::uint64_t& nulls) {
            std::vector<std::uint8_t> bits((n + 7U) / 8U, 0U);
            nulls = 0;
            for (std::uint64_t i = 0; i < n; ++i) {
                if (bit(null_percent)) {
                    ++nulls;
                } else {
                    bits[i >> 3U] |= static_cast<std::uint8_t>(1U << (i & 7U));
                }
            }
            if (nulls == 0U) {
                bits.clear();
            }
            return bits;
        };
        for (int trial = 0; trial < 400; ++trial) {
            const int shape = trial % 5;  // 0 fixed, 1 utf8, 2 large utf8 in a list, 3 fsl, 4 list<struct<list<utf8>>>
            const std::uint64_t rows = 1U + rng() % 300U;
            nano_lance::ColumnValues src;
            std::size_t value_bytes = 4;
            std::uint64_t leaf = rows;
            if (shape >= 2 && shape != 3) {
                // Lists above the leaf, outermost first.
                const int depth = shape == 2 ? 1 : 3;
                std::uint64_t level = rows;
                for (int k = 0; k < depth; ++k) {
                    nano_lance::ColumnValues::NestedLayer layer;
                    layer.is_list = !(shape == 4 && k == 1);
                    layer.length = level;
                    layer.validity = bitmap(level, 10, layer.null_count);
                    if (layer.is_list) {
                        layer.offsets.push_back(0);
                        for (std::uint64_t i = 0; i < level; ++i) {
                            layer.offsets.push_back(layer.offsets.back() + static_cast<std::int64_t>(rng() % 4U));
                        }
                        level = static_cast<std::uint64_t>(layer.offsets.back());
                    }
                    src.layers.push_back(std::move(layer));
                }
                leaf = level;
            }
            if (shape == 0 || shape == 3) {
                src.kind = nano_lance::ColumnValues::Kind::FixedWidth;
                if (shape == 3) {
                    src.items_per_row = 3;
                    value_bytes = 12;
                    src.item_validity = bitmap(leaf * 3U, 5, src.item_null_count);
                }
                src.fixed.resize(leaf * value_bytes);
                for (auto& b : src.fixed) {
                    b = static_cast<std::uint8_t>(rng());
                }
            } else {
                src.kind = nano_lance::ColumnValues::Kind::VariableWidth;
                src.variable.large = shape == 2;
                value_bytes = 0;
                const std::size_t width = src.variable.large ? 8U : 4U;
                src.variable.offsets.assign(width, 0U);
                std::uint64_t end = 0;
                for (std::uint64_t i = 0; i < leaf; ++i) {
                    const auto n = rng() % 9U;
                    for (std::uint64_t c = 0; c < n; ++c) {
                        src.variable.data.push_back(static_cast<std::uint8_t>('a' + rng() % 26U));
                    }
                    end += n;
                    src.variable.offsets.resize(src.variable.offsets.size() + width);
                    std::memcpy(src.variable.offsets.data() + (i + 1U) * width, &end, width);
                }
            }
            src.validity = bitmap(leaf, 15, src.null_count);
            src.rows = leaf;
            std::vector<std::uint8_t> keep(rows, 0U);
            const unsigned density = static_cast<unsigned>(rng() % 101U);
            for (auto& k : keep) {
                k = bit(density) ? 1U : 0U;
            }
            const auto before = src;
            auto compacted = src;
            nano_lance::ColumnValues gathered;
            std::string e1;
            std::string e2;
            require(nano_lance::compact_column_values(compacted, keep, rows, value_bytes, e1), e1);
            require(nano_lance::gather_column_values(src, keep, rows, value_bytes, gathered, e2), e2);
            const auto label = "gather trial " + std::to_string(trial) + " shape " + std::to_string(shape);
            require(gathered.kind == compacted.kind && gathered.rows == compacted.rows, label + ": rows");
            require(gathered.fixed == compacted.fixed, label + ": fixed values");
            require(gathered.variable.data == compacted.variable.data &&
                        gathered.variable.offsets == compacted.variable.offsets,
                    label + ": variable values");
            require(gathered.validity == compacted.validity && gathered.null_count == compacted.null_count,
                    label + ": validity");
            require(gathered.item_validity == compacted.item_validity &&
                        gathered.item_null_count == compacted.item_null_count &&
                        gathered.items_per_row == compacted.items_per_row,
                    label + ": element validity");
            require(gathered.layers.size() == compacted.layers.size(), label + ": layers");
            for (std::size_t k = 0; k < gathered.layers.size(); ++k) {
                const auto& g = gathered.layers[k];
                const auto& c = compacted.layers[k];
                require(g.is_list == c.is_list && g.offsets == c.offsets && g.validity == c.validity &&
                            g.null_count == c.null_count && g.length == c.length,
                        label + ": layer " + std::to_string(k));
            }
            require(src.fixed == before.fixed && src.variable.data == before.variable.data &&
                        src.variable.offsets == before.variable.offsets && src.validity == before.validity &&
                        src.rows == before.rows && src.layers.size() == before.layers.size(),
                    label + ": the source was changed");
        }
    }

    std::cout << "column slice ok\n";
    return 0;
}
