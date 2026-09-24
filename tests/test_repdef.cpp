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

int main() {
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
