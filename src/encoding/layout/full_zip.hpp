// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

/// The full-zip page layout's row grammar, both directions (Lance's `encodings/logical/primitive.rs`,
/// `FullZipLayout` in `encodings_v2_1.proto`). A full-zip page stores each row whole, one after the other:
///
///     per level: [control word: 0, 1, 2 or 4 bytes, `rep << bits_def | def`, little-endian]
///                then, for a level that owns a value slot (def <= max_visible_def):
///                  fixed width:     [value_bytes]             present even for a null
///                  variable width:  [length: 4 or 8 bytes][the bytes]   only for a valid value (def 0)
///
/// Without lists there is one level per row. With lists (`bits_rep` > 0) there is one per level of
/// Lance's `ControlWordIterator`, and a level above `max_visible_def` -- an empty or null list -- is the
/// control word alone. A variable-width page's second buffer is the repetition index: rows + 1 byte offsets
/// where each row starts, so a reader can fetch one row without the page.
///
/// What the values are (FSST, zstd, a packed struct) is the caller's business; this module only cuts the
/// page into levels.
namespace nano_lance::full_zip {

/// Bytes of a control word for these widths: 0 when both are 0, else 1, 2 or 4.
constexpr std::size_t control_bytes(std::uint32_t bits_rep, std::uint32_t bits_def) {
    const auto bits = bits_rep + bits_def;
    return bits == 0U ? 0U : bits <= 8U ? 1U : bits <= 16U ? 2U : 4U;
}

/// What cutting a page into levels needs, from the page's layout.
struct RowShape {
    std::uint32_t bits_rep = 0;
    std::uint32_t bits_def = 0;
    /// Highest definition level that owns a value slot; anything above is an empty or null list.
    std::uint32_t max_visible_def = 0xFFFFU;
    std::size_t value_bytes = 0;   // fixed width; 0 for variable width
    std::size_t length_bytes = 0;  // variable width: 4 or 8; 0 for fixed width

    std::size_t control() const { return control_bytes(bits_rep, bits_def); }
};

/// One level of a page.
struct Level {
    std::uint32_t rep = 0;
    std::uint32_t def = 0;
    /// It owns a value slot (it is not an empty or null list).
    bool visible = false;
    /// The slot's bytes are in the page: always for a visible fixed-width level, for a valid one only
    /// otherwise. `value_at` / `value_size` locate them (after the length prefix, for variable width).
    bool has_value = false;
    std::size_t value_at = 0;
    std::size_t value_size = 0;
};

// ---- Writing ----

/// Appends one level: its control word (`control` bytes of it) and, when `value` is not null, the value --
/// behind a `length_bytes` length prefix when that is not 0 (variable width), as is otherwise (a fixed slot).
void append_level(std::vector<std::uint8_t>& out, std::size_t control, std::uint32_t word,
                  const std::uint8_t* value, std::size_t value_size, std::size_t length_bytes);

/// The repetition index for rows starting at `starts` (rows + 1 entries, the last one the page's size):
/// u32 entries when the page fits in 4 GiB, u64 otherwise.
std::vector<std::uint8_t> repetition_index(std::span<const std::uint64_t> starts);

// ---- Reading (the input is untrusted) ----

/// Reads the level at `at` of `data` and moves `at` past it. False, with `error`, when the page ends inside
/// the control word, the value slot, the length or the value.
bool next_level(std::span<const std::uint8_t> data, std::size_t& at, const RowShape& shape, Level& out,
                std::string& error);

/// The levels in `data` (whole rows of a page that has control words) and how many own a value slot.
bool count_levels(std::span<const std::uint8_t> data, const RowShape& shape, std::uint64_t& levels,
                  std::uint64_t& visible, std::string& error);

/// Entry width of a repetition index of `index_bytes` for `rows` rows: 1, 2, 4 or 8, or 0 when it does not
/// have one whole entry per row plus one.
std::size_t repetition_index_width(std::uint64_t index_bytes, std::uint64_t rows);

/// Entry `k` of a repetition index of `width`-byte entries (in bounds: the caller checked the width).
std::uint64_t repetition_index_entry(std::span<const std::uint8_t> index, std::size_t width, std::uint64_t k);

}  // namespace nano_lance::full_zip
