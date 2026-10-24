// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// encoding/layout/full_zip: pages of levels written then cut back into levels, for variable width (4 and 8-byte
// lengths, with and without nulls, empty values) and fixed width (a slot even for a null); list pages, whose
// control words carry repetition and whose empty or null lists are the control word alone; control word
// widths 0, 1, 2 and 4 bytes; the repetition index both ways (u32, and every entry width a reader accepts);
// the exact bytes of a nullable variable-width row; and the refusals: a page cut inside a control word, a
// fixed slot, a length or a value, and an index without one whole entry per row.

#include "encoding/layout/full_zip.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

namespace fz = nano_lance::full_zip;

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;

Bytes value_of(std::size_t n, std::size_t seed) {
    Bytes out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(1U + (seed * 29U + i * 13U) % 250U);
    }
    return out;
}

void test_control_bytes() {
    require(fz::control_bytes(0U, 0U) == 0U, "no levels: no control word");
    require(fz::control_bytes(0U, 1U) == 1U && fz::control_bytes(3U, 5U) == 1U, "up to 8 bits: 1 byte");
    require(fz::control_bytes(1U, 8U) == 2U && fz::control_bytes(8U, 8U) == 2U, "up to 16 bits: 2 bytes");
    require(fz::control_bytes(16U, 1U) == 4U && fz::control_bytes(16U, 16U) == 4U, "more: 4 bytes");
}

/// Rows of optional values, written as a variable-width page and read back, with their repetition index.
void variable_round_trip(const std::vector<std::optional<Bytes>>& rows, std::size_t length_bytes) {
    const auto label = std::to_string(rows.size()) + " rows, " + std::to_string(length_bytes) + "-byte lengths";
    bool nulls = false;
    for (const auto& r : rows) {
        nulls = nulls || !r;
    }
    fz::RowShape shape;
    shape.bits_def = nulls ? 1U : 0U;
    shape.length_bytes = length_bytes;
    Bytes page;
    std::vector<std::uint64_t> starts;
    for (const auto& r : rows) {
        starts.push_back(page.size());
        static constexpr std::uint8_t kEmpty = 0U;
        if (r) {
            fz::append_level(page, shape.control(), 0U, r->empty() ? &kEmpty : r->data(), r->size(), length_bytes);
        } else {
            fz::append_level(page, shape.control(), 1U, nullptr, 0U, length_bytes);
        }
    }
    starts.push_back(page.size());

    std::size_t at = 0;
    std::string error;
    fz::Level level;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        require(at == starts[i], label + ": row " + std::to_string(i) + " starts where it was written");
        require(fz::next_level(page, at, shape, level, error), label + ": " + error);
        require(level.visible, label + ": every row owns a slot");
        require(level.def == (rows[i] ? 0U : 1U), label + ": definition level");
        require(level.has_value == rows[i].has_value(), label + ": a value only for a valid row");
        if (rows[i]) {
            require(Bytes(page.begin() + static_cast<std::ptrdiff_t>(level.value_at),
                          page.begin() + static_cast<std::ptrdiff_t>(level.value_at + level.value_size)) == *rows[i],
                    label + ": value " + std::to_string(i) + " back");
        }
    }
    require(at == page.size(), label + ": the page ends after its rows");

    const auto index = fz::repetition_index(starts);
    const auto width = fz::repetition_index_width(index.size(), rows.size());
    require(width == 4U, label + ": u32 index");
    for (std::size_t i = 0; i < starts.size(); ++i) {
        require(fz::repetition_index_entry(index, width, i) == starts[i], label + ": index entry back");
    }

    if (nulls) {
        std::uint64_t levels = 0;
        std::uint64_t visible = 0;
        require(fz::count_levels(page, shape, levels, visible, error) && levels == rows.size() &&
                    visible == rows.size(),
                label + ": count_levels: " + error);
    }
}

void test_variable() {
    for (const std::size_t length_bytes : {4U, 8U}) {
        variable_round_trip({Bytes{1, 2, 3}}, length_bytes);
        variable_round_trip({Bytes{}, Bytes{}, Bytes{7}}, length_bytes);
        variable_round_trip({std::nullopt}, length_bytes);
        variable_round_trip({value_of(5000U, 1U), std::nullopt, Bytes{}, value_of(1U, 2U), std::nullopt}, length_bytes);
        std::vector<std::optional<Bytes>> many;
        for (std::size_t i = 0; i < 300U; ++i) {
            many.push_back(i % 7U == 3U ? std::nullopt : std::optional<Bytes>(value_of(i % 40U, i)));
        }
        variable_round_trip(many, length_bytes);
    }

    // The exact bytes: a null is its definition byte alone; a valid value is [0][u32 length][bytes].
    Bytes page;
    const Bytes ab = {'a', 'b'};
    fz::append_level(page, 1U, 0U, ab.data(), ab.size(), 4U);
    fz::append_level(page, 1U, 1U, nullptr, 0U, 4U);
    require(page == Bytes{0, 2, 0, 0, 0, 'a', 'b', 1}, "nullable variable-width row bytes");

    // No control word at all: [u64 length][bytes].
    page.clear();
    fz::append_level(page, 0U, 0U, ab.data(), ab.size(), 8U);
    require(page == Bytes{2, 0, 0, 0, 0, 0, 0, 0, 'a', 'b'}, "all-valid row bytes");
}

void test_fixed() {
    // Fixed width: every row has its slot, null or not.
    fz::RowShape shape;
    shape.bits_def = 1U;
    shape.value_bytes = 6U;
    Bytes page;
    for (std::size_t i = 0; i < 10U; ++i) {
        const auto slot = value_of(6U, i);
        fz::append_level(page, shape.control(), i % 3U == 0U ? 1U : 0U, slot.data(), slot.size(), 0U);
    }
    require(page.size() == 10U * 7U, "fixed rows are control word + slot");
    std::size_t at = 0;
    std::string error;
    fz::Level level;
    for (std::size_t i = 0; i < 10U; ++i) {
        require(fz::next_level(page, at, shape, level, error), "fixed: " + error);
        require(level.has_value && level.value_size == 6U && level.def == (i % 3U == 0U ? 1U : 0U),
                "fixed level " + std::to_string(i));
        require(Bytes(page.begin() + static_cast<std::ptrdiff_t>(level.value_at),
                      page.begin() + static_cast<std::ptrdiff_t>(level.value_at + 6U)) == value_of(6U, i),
                "fixed slot back");
    }
    // Without nulls there is no control word: the page is the slots.
    shape.bits_def = 0U;
    page.clear();
    const auto slot = value_of(6U, 1U);
    fz::append_level(page, shape.control(), 0U, slot.data(), slot.size(), 0U);
    require(page == slot, "all-valid fixed rows are the slots alone");
}

void test_lists() {
    // A list<binary> page: bits_rep 1, bits_def 2 (0 valid item, 1 null item, 2 null or empty list), so
    // max_visible_def 1. Levels: [a, null] [] [b].
    fz::RowShape shape;
    shape.bits_rep = 1U;
    shape.bits_def = 2U;
    shape.max_visible_def = 1U;
    shape.length_bytes = 4U;
    const auto word = [](std::uint32_t rep, std::uint32_t def) { return (rep << 2U) | def; };
    Bytes page;
    const Bytes a = {'a'};
    const Bytes b = {'b', 'b'};
    fz::append_level(page, shape.control(), word(1U, 0U), a.data(), a.size(), 4U);
    fz::append_level(page, shape.control(), word(0U, 1U), nullptr, 0U, 4U);
    fz::append_level(page, shape.control(), word(1U, 2U), nullptr, 0U, 4U);
    fz::append_level(page, shape.control(), word(1U, 0U), b.data(), b.size(), 4U);

    struct Want {
        std::uint32_t rep, def;
        bool visible, has_value;
    };
    const Want want[4] = {{1U, 0U, true, true}, {0U, 1U, true, false}, {1U, 2U, false, false}, {1U, 0U, true, true}};
    std::size_t at = 0;
    std::string error;
    fz::Level level;
    for (const auto& w : want) {
        require(fz::next_level(page, at, shape, level, error), "list: " + error);
        require(level.rep == w.rep && level.def == w.def && level.visible == w.visible &&
                    level.has_value == w.has_value,
                "list level");
    }
    require(at == page.size(), "list page fully read");
    std::uint64_t levels = 0;
    std::uint64_t visible = 0;
    require(fz::count_levels(page, shape, levels, visible, error) && levels == 4U && visible == 3U,
            "list count_levels: " + error);

    // Two-byte and four-byte control words carry wide levels.
    shape.bits_rep = 4U;
    shape.bits_def = 9U;
    shape.max_visible_def = 0x1FFU;
    page.clear();
    fz::append_level(page, shape.control(), (0xAU << 9U) | 0x101U, nullptr, 0U, 4U);
    at = 0;
    require(shape.control() == 2U && fz::next_level(page, at, shape, level, error) && level.rep == 0xAU &&
                level.def == 0x101U && !level.has_value,
            "2-byte control word");
    shape.bits_rep = 16U;
    shape.bits_def = 16U;
    shape.max_visible_def = 0xFFFFU;
    page.clear();
    fz::append_level(page, shape.control(), (0xBEEFU << 16U) | 0x1234U, nullptr, 0U, 4U);
    at = 0;
    require(shape.control() == 4U && fz::next_level(page, at, shape, level, error) && level.rep == 0xBEEFU &&
                level.def == 0x1234U,
            "4-byte control word");
}

void test_index_widths() {
    // Every width a reader accepts, hand-built: entries 0, 3, 9 for two rows.
    for (const std::size_t width : {1U, 2U, 4U, 8U}) {
        Bytes index(3U * width, 0U);
        index[1U * width] = 3U;
        index[2U * width] = 9U;
        require(fz::repetition_index_width(index.size(), 2U) == width, "index width " + std::to_string(width));
        require(fz::repetition_index_entry(index, width, 2U) == 9U, "index entry at width " + std::to_string(width));
    }
    require(fz::repetition_index_width(0U, 2U) == 0U, "an empty index refused");
    require(fz::repetition_index_width(10U, 2U) == 0U, "an index without whole entries refused");
    require(fz::repetition_index_width(9U, 2U) == 0U, "3-byte entries refused");
    require(fz::repetition_index_width(48U, 2U) == 0U, "16-byte entries refused");

    // A page past 4 GiB needs u64 entries.
    const std::vector<std::uint64_t> big = {0U, std::uint64_t{1} << 33U};
    const auto index = fz::repetition_index(big);
    require(index.size() == 16U && fz::repetition_index_entry(index, 8U, 1U) == big[1], "u64 index past 4 GiB");
}

void test_refusals() {
    fz::RowShape shape;
    shape.bits_def = 9U;  // 2-byte control words
    shape.length_bytes = 4U;
    Bytes page;
    const auto v = value_of(10U, 3U);
    fz::append_level(page, shape.control(), 0U, v.data(), v.size(), 4U);
    std::string error;
    fz::Level level;
    const auto cut = [&](std::size_t size) {
        std::size_t at = 0;
        return fz::next_level(std::span(page).first(size), at, shape, level, error);
    };
    require(!cut(1U) && error.find("control word") != std::string::npos, "cut in the control word: " + error);
    require(!cut(4U) && error.find("length") != std::string::npos, "cut in the length: " + error);
    require(!cut(page.size() - 1U) && error.find("runs past") != std::string::npos, "cut in the value: " + error);
    require(cut(page.size()), "the whole row reads");

    fz::RowShape fixed;
    fixed.value_bytes = 8U;
    std::size_t at = 0;
    const Bytes short_slot(7U, 1U);
    require(!fz::next_level(short_slot, at, fixed, level, error) && error.find("value") != std::string::npos,
            "cut in a fixed slot");

    std::uint64_t levels = 0;
    std::uint64_t visible = 0;
    fz::RowShape no_control;
    no_control.length_bytes = 4U;
    require(!fz::count_levels(page, no_control, levels, visible, error), "counting levels needs control words");
    page.pop_back();
    require(!fz::count_levels(page, shape, levels, visible, error), "a cut page is refused by count_levels");
}

}  // namespace

int main() {
    test_control_bytes();
    test_variable();
    test_fixed();
    test_lists();
    test_index_widths();
    test_refusals();
    std::cout << "full_zip: ok\n";
    return 0;
}
