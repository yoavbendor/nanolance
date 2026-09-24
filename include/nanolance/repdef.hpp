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

}  // namespace nano_lance::repdef
