// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Table-driven tests for the repetition/definition unraveler. The level streams and expected
// offsets/validity are Lance's own, from the unit tests in lance-encoding's `repdef.rs`, so a pass
// here means the port agrees with the reference implementation case by case -- not just with a
// reading of it.

#include "nanolance/repdef.hpp"

#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace rd = nano_lance::repdef;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

std::vector<bool> bits(const rd::UnraveledLayer& layer) {
    std::vector<bool> out;
    for (std::uint64_t i = 0; i < layer.length; ++i) {
        out.push_back(layer.validity.empty() || ((layer.validity[i >> 3U] >> (i & 7U)) & 1U) != 0U);
    }
    return out;
}

struct Case {
    std::string name;
    std::vector<std::uint16_t> rep;
    bool has_rep;
    std::vector<std::uint16_t> def;
    bool has_def;
    std::vector<std::uint8_t> layers;
    std::uint64_t num_items;
    // Expected, innermost first: validity per layer, and offsets for list layers ({} for items).
    std::vector<std::vector<bool>> validity;
    std::vector<std::vector<std::int64_t>> offsets;
};

void run(const Case& c) {
    std::vector<rd::UnraveledLayer> out;
    std::string error;
    if (!rd::unravel(c.rep, c.has_rep, c.def, c.has_def, c.layers, c.num_items, out, error)) {
        check(false, c.name + ": refused: " + error);
        return;
    }
    check(out.size() == c.layers.size(), c.name + ": layer count");
    for (std::size_t i = 0; i < out.size() && i < c.validity.size(); ++i) {
        check(bits(out[i]) == c.validity[i], c.name + ": validity of layer " + std::to_string(i));
        check(out[i].offsets == c.offsets[i], c.name + ": offsets of layer " + std::to_string(i));
        std::uint64_t nulls = 0;
        for (const bool b : c.validity[i]) {
            nulls += b ? 0U : 1U;
        }
        check(out[i].null_count == nulls, c.name + ": null count of layer " + std::to_string(i));
        check(nulls != 0U || out[i].validity.empty(), c.name + ": all-valid layer keeps no bitmap");
    }
}

void refuse(const std::string& name, const std::vector<std::uint16_t>& rep, bool has_rep,
            const std::vector<std::uint16_t>& def, bool has_def, const std::vector<std::uint8_t>& layers,
            std::uint64_t num_items, const std::string& fragment) {
    std::vector<rd::UnraveledLayer> out;
    std::string error;
    const bool ok = rd::unravel(rep, has_rep, def, has_def, layers, num_items, out, error);
    check(!ok, name + ": accepted malformed levels");
    check(error.find(fragment) != std::string::npos, name + ": refused with '" + error + "'");
}

}  // namespace

// ── Serializer: property test against the unraveler ─────────────────────────────────────────────
//
// Random nested columns -- lists and structs at random depths, nulls and empties at every layer, and
// null lists that still point at "garbage" children as Arrow allows -- are serialized over a random
// row range, unravelled back, and both sides rendered row by row as values. Equal renderings mean the
// round trip preserved everything a reader can observe.

struct RandomColumn {
    std::vector<bool> is_list;                               // outermost first
    std::vector<std::vector<std::int64_t>> offsets;          // lists only
    std::vector<std::vector<std::uint8_t>> validity;
    std::vector<std::uint8_t> item_validity;
    std::uint64_t rows = 0;
};

std::vector<std::uint8_t> random_bits(std::mt19937& rng, std::uint64_t n, int null_percent) {
    std::vector<std::uint8_t> bits((n + 7U) / 8U, 0U);
    for (std::uint64_t i = 0; i < n; ++i) {
        if (static_cast<int>(rng() % 100U) >= null_percent) {
            bits[i >> 3U] |= static_cast<std::uint8_t>(1U << (i & 7U));
        }
    }
    return bits;
}

RandomColumn random_column(std::mt19937& rng) {
    RandomColumn c;
    const auto depth = 1U + rng() % 4U;
    bool any_list = false;
    for (unsigned k = 0; k < depth; ++k) {
        const bool list = (rng() % 3U) != 0U || (k + 1U == depth && !any_list);
        c.is_list.push_back(list);
        any_list = any_list || list;
    }
    c.rows = 1U + rng() % 40U;
    std::uint64_t n = c.rows;
    const int null_percent = static_cast<int>(rng() % 40U);
    for (unsigned k = 0; k < depth; ++k) {
        c.validity.push_back((rng() % 4U) == 0U ? std::vector<std::uint8_t>{} : random_bits(rng, n, null_percent));
        if (!c.is_list[k]) {
            c.offsets.emplace_back();
            continue;  // a struct: one child per entry
        }
        std::vector<std::int64_t> off{0};
        for (std::uint64_t i = 0; i < n; ++i) {
            off.push_back(off.back() + static_cast<std::int64_t>(rng() % 4U));  // null lists keep garbage
        }
        n = static_cast<std::uint64_t>(off.back());
        c.offsets.push_back(std::move(off));
    }
    c.item_validity = (rng() % 3U) == 0U ? std::vector<std::uint8_t>{} : random_bits(rng, n, null_percent);
    return c;
}

bool bit(const std::vector<std::uint8_t>& v, std::uint64_t i) {
    return v.empty() || ((v[i >> 3U] >> (i & 7U)) & 1U) != 0U;
}

std::string render_input(const RandomColumn& c, std::size_t k, std::uint64_t idx) {
    if (k == c.is_list.size()) {
        return bit(c.item_validity, idx) ? "v" + std::to_string(idx) : "null";
    }
    if (!bit(c.validity[k], idx)) {
        return "null";
    }
    if (!c.is_list[k]) {
        return "{" + render_input(c, k + 1U, idx) + "}";
    }
    std::string s = "[";
    for (auto i = c.offsets[k][idx]; i < c.offsets[k][idx + 1U]; ++i) {
        s += (i == c.offsets[k][idx] ? "" : ",") + render_input(c, k + 1U, static_cast<std::uint64_t>(i));
    }
    return s + "]";
}

std::string render_output(const std::vector<rd::UnraveledLayer>& u, const std::vector<std::uint64_t>& items,
                          std::size_t depth, std::size_t k, std::uint64_t idx) {
    if (k == depth) {
        return bit(u[0].validity, idx) ? "v" + std::to_string(items[idx]) : "null";
    }
    const auto& layer = u[depth - k];
    if (!bit(layer.validity, idx)) {
        return "null";
    }
    if (!rd::is_list_layer(layer.kind)) {
        return "{" + render_output(u, items, depth, k + 1U, idx) + "}";
    }
    std::string s = "[";
    for (auto i = layer.offsets[idx]; i < layer.offsets[idx + 1U]; ++i) {
        s += (i == layer.offsets[idx] ? "" : ",") + render_output(u, items, depth, k + 1U, static_cast<std::uint64_t>(i));
    }
    return s + "]";
}

void serializer_round_trips(int seeds) {
    for (int seed = 0; seed < seeds; ++seed) {
        std::mt19937 rng(static_cast<unsigned>(seed));
        const auto c = random_column(rng);
        std::vector<rd::SerializeLayer> layers;
        for (std::size_t k = 0; k < c.is_list.size(); ++k) {
            layers.push_back({c.is_list[k], &c.offsets[k], &c.validity[k]});
        }
        const std::uint64_t first = rng() % c.rows;
        const std::uint64_t count = 1U + rng() % (c.rows - first);
        rd::Serialized ser;
        std::string error;
        const auto where = "seed " + std::to_string(seed);
        if (!rd::serialize(layers, c.item_validity, first, count, ser, error)) {
            check(false, where + ": serialize refused: " + error);
            continue;
        }
        std::vector<rd::UnraveledLayer> u;
        if (!rd::unravel(ser.rep, ser.has_rep, ser.def, ser.has_def, ser.layers, ser.items.size(), u, error)) {
            check(false, where + ": unravel refused its output: " + error);
            continue;
        }
        check(u.back().length == count, where + ": row count");
        for (std::uint64_t r = 0; r < count && r < u.back().length; ++r) {
            const auto want = render_input(c, 0, first + r);
            const auto got = render_output(u, ser.items, c.is_list.size(), 0, r);
            if (want != got) {
                check(false, where + " row " + std::to_string(r) + ": wrote " + want + ", read " + got);
                break;
            }
        }
    }
}

int main() {
    serializer_round_trips(20000);

    using V = std::vector<bool>;
    constexpr bool T = true;
    constexpr bool F = false;

    // repdef.rs test_repdef_basic: [[I], [I, I]], NULL, [[NULL, NULL], NULL, [NULL, I, I, NULL]]
    run({"basic",
         {2, 1, 0, 2, 2, 0, 1, 1, 0, 0, 0}, true,
         {0, 0, 0, 3, 1, 1, 2, 1, 0, 0, 1}, true,
         {rd::kNullableItem, rd::kNullableList, rd::kNullableList}, 9,
         {V{T, T, T, F, F, F, T, T, F}, V{T, T, T, F, T}, V{T, F, T}},
         {{}, {0, 1, 3, 5, 5, 9}, {0, 2, 2, 5}}});

    // test_repdef_simple_null_empty_list: the same levels read as a null list, then an empty one.
    run({"null_list",
         {1, 0, 1, 1, 0, 0}, true, {0, 0, 2, 0, 1, 0}, true,
         {rd::kNullableItem, rd::kNullableList}, 5,
         {V{T, T, T, F, T}, V{T, F, T}},
         {{}, {0, 2, 2, 5}}});
    run({"empty_list",
         {1, 0, 1, 1, 0, 0}, true, {0, 0, 2, 0, 1, 0}, true,
         {rd::kNullableItem, rd::kEmptyableList}, 5,
         {V{T, T, T, F, T}, V{T, T, T}},
         {{}, {0, 2, 2, 5}}});

    // test_repdef_empty_list_at_end: the trailing empty list has a level and no value.
    run({"empty_list_at_end",
         {1, 0, 1, 0, 0, 1}, true, {0, 0, 0, 1, 0, 2}, true,
         {rd::kNullableItem, rd::kEmptyableList}, 5,
         {V{T, T, T, F, T}, V{T, T, T}},
         {{}, {0, 2, 5, 5}}});

    // test_repdef_abnormal_nulls: a null list that had garbage children in Arrow has none here.
    run({"abnormal_nulls",
         {1, 0, 1, 1, 0, 0}, true, {0, 0, 1, 0, 0, 0}, true,
         {rd::kAllValidItem, rd::kNullableList}, 5,
         {V{T, T, T, T, T}, V{T, F, T}},
         {{}, {0, 2, 2, 5}}});

    // test_repdef_empty_list_no_null: definition levels present, yet nothing is null.
    run({"empty_lists_no_null",
         {1, 0, 0, 0, 1, 1, 1, 0}, true, {0, 0, 0, 0, 1, 1, 0, 0}, true,
         {rd::kAllValidItem, rd::kEmptyableList}, 6,
         {V(6, T), V{T, T, T, T}},
         {{}, {0, 4, 4, 4, 6}}});

    // test_repdef_null_struct_valid_list: AllValidList<NullableStruct<NullableItem>>.
    run({"null_struct_in_valid_list",
         {1, 0, 0, 0}, true, {2, 0, 2, 2}, true,
         {rd::kNullableItem, rd::kNullableItem, rd::kAllValidList}, 4,
         {V{F, T, F, F}, V{F, T, F, F}, V{T}},
         {{}, {}, {0, 4}}});

    // No definition levels at all: every level is an item.
    run({"no_def", {1, 0, 1, 1, 0, 0}, true, {}, false,
         {rd::kAllValidItem, rd::kAllValidList}, 6,
         {V(6, T), V{T, T, T}},
         {{}, {0, 2, 3, 6}}});

    // [[1, 2], [], None, [None, 4]]: 6 levels, 4 values -- an empty list and a null list both take a
    // level and no value. (C2 checks these levels against the ones pylance actually writes.)
    run({"null_and_empty_list",
         {1, 0, 1, 1, 1, 0}, true, {0, 0, 3, 2, 1, 0}, true,
         {rd::kNullableItem, rd::kNullAndEmptyList}, 4,
         {V{T, T, F, T}, V{T, T, F, T}},
         {{}, {0, 2, 2, 2, 4}}});

    // Not a list: definition levels alone, the flat nullable column the reader already handles.
    run({"flat_nullable", {}, false, {0, 1, 0}, true, {rd::kNullableItem}, 3, {V{T, F, T}}, {{}}});

    // Malformed input is refused by name, never read past.
    refuse("def_too_high", {1, 0}, true, {0, 3}, true, {rd::kNullableItem, rd::kNullableList}, 2, "definition level above");
    refuse("rep_too_high", {1, 2}, true, {}, false, {rd::kAllValidItem, rd::kAllValidList}, 2, "repetition level above");
    refuse("mid_row_start", {0, 1}, true, {}, false, {rd::kAllValidItem, rd::kAllValidList}, 2, "row boundary");
    refuse("length_mismatch", {1, 0}, true, {0}, true, {rd::kNullableItem, rd::kNullableList}, 2, "counts differ");
    refuse("item_count", {1, 0, 1}, true, {}, false, {rd::kAllValidItem, rd::kAllValidList}, 5, "items");
    refuse("unknown_layer", {1}, true, {}, false, {rd::kAllValidItem, 9}, 1, "unknown");
    refuse("list_innermost", {1}, true, {}, false, {rd::kAllValidList}, 1, "item layer");
    refuse("rep_without_list", {1}, true, {}, false, {rd::kAllValidItem}, 1, "no list layer");
    refuse("list_without_rep", {}, false, {}, false, {rd::kAllValidItem, rd::kAllValidList}, 0, "no repetition");

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "repdef unravel tests passed\n";
    return 0;
}
