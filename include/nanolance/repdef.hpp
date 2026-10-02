// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Repetition/definition-level unravelling: turns one page's level streams back into per-layer list
// offsets and validity. A port of `RepDefUnraveler` in lance-encoding's `repdef.rs`; see
// docs/NESTED_COLUMNS.md for where it sits in the read path.
//
// Pure computation over untrusted input: no I/O, and every level is range-checked, so it can be
// fuzzed on its own.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nano_lance::repdef {

/// Lance's `RepDefLayer` / `DefinitionInterpretation`, as the descriptor's `layers` field spells it.
enum LayerKind : std::uint8_t {
    kAllValidItem = 1,
    kAllValidList = 2,
    kNullableItem = 3,
    kNullableList = 4,
    kEmptyableList = 5,
    kNullAndEmptyList = 6,
};

inline bool is_list_layer(std::uint8_t kind) {
    return kind == kAllValidList || kind == kNullableList || kind == kEmptyableList || kind == kNullAndEmptyList;
}

/// One layer of the result. Items and structs have validity only; lists also have offsets.
struct UnraveledLayer {
    std::uint8_t kind = kAllValidItem;
    std::uint64_t length = 0;               // entries in this layer (items, structs or lists)
    std::vector<std::int64_t> offsets;      // lists only: length + 1 entries, starting at 0
    std::vector<std::uint8_t> validity;     // LSB-first bits; empty when every entry is valid
    std::uint64_t null_count = 0;
};

/// Pass as `num_items` when the page does not declare its item count (a constant page): the count
/// is then whatever the levels say, and nothing is checked against it.
inline constexpr std::uint64_t kInferItems = ~std::uint64_t{0};

/// Unravel one page.
///
/// `rep` / `def` are the page's levels, one entry per item plus one per empty or null list; an empty
/// vector means the stream is absent (`has_rep` / `has_def` say which, since a present stream can
/// also be empty for a zero-row page). `layers` is the descriptor's layer list, innermost first.
/// `num_items` is how many item slots (values) the page holds.
///
/// On success `out` holds one entry per layer, in the same innermost-first order, and the last
/// layer's `length` is the page's row count. Returns false with `error` set on any inconsistency:
/// a level out of range, streams of different lengths, an item count that does not match, or a
/// stream that does not start at a row boundary.
bool unravel(const std::vector<std::uint16_t>& rep, bool has_rep, const std::vector<std::uint16_t>& def,
             bool has_def, const std::vector<std::uint8_t>& layers, std::uint64_t num_items,
             std::vector<UnraveledLayer>& out, std::string& error);

/// One layer of a nested column as the writer holds it, OUTERMOST first: a list (offsets into the
/// next layer) or a struct (one child per entry). Pointers, not copies: the column owns the data.
struct SerializeLayer {
    bool is_list = true;
    const std::vector<std::int64_t>* offsets = nullptr;  // lists: one more entry than the layer has
    const std::vector<std::uint8_t>* validity = nullptr; // LSB-first; null or empty = all valid
};

/// Levels for a run of rows, the inverse of `unravel`.
struct Serialized {
    std::vector<std::uint16_t> rep;       // empty when the column has no list layer
    std::vector<std::uint16_t> def;       // empty when nothing in the rows is null or empty
    std::vector<std::uint8_t> layers;     // the descriptor's layer list, innermost first
    std::vector<std::uint64_t> items;     // leaf index of every value slot, in order
    std::vector<std::uint8_t> is_slot;    // per level: 1 if it owns a value slot (sizes a chunk's levels)
    bool has_rep = false;
    bool has_def = false;
};

/// Serialize rows `[first_row, first_row + num_rows)` of a nested column to repetition and
/// definition levels -- what Lance's `RepDefBuilder` produces from Arrow arrays.
///
/// Every layer's kind is chosen from the rows serialized: a list is `NullableList` only if one of
/// them is null, `EmptyableList` only if one is empty, and so on, as Lance does. A null or empty
/// list takes one level and no value slot, and its children (Arrow allows garbage there) are never
/// visited; a null struct with no list below it keeps its value slot, as in Lance. `items` lists the
/// leaf entries that get a slot, so the caller can gather exactly those values.
bool serialize(const std::vector<SerializeLayer>& layers, const std::vector<std::uint8_t>& item_validity,
               std::uint64_t first_row, std::uint64_t num_rows, Serialized& out, std::string& error);

}  // namespace nano_lance::repdef
