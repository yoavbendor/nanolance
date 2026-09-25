// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/column_values.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nano_lance {

/// Trim a decoded column in place to rows `[first, first + count)` of the `total` rows it holds.
///
/// This is the row-range read's only sharp edge, so it lives on its own and is tested on its own.
///
/// It operates on `ColumnValues` -- the decoder's output, *before* the Arrow arrays are assembled --
/// rather than on a decoded `ArrowArray`. That choice is the whole safety argument:
///
///   - Every value buffer here is byte-addressed. `bool` is one BYTE per value at this stage and is
///     packed to bits only when the Arrow buffer is built, so slicing a value buffer is never
///     bit-arithmetic. The validity bitmap is the single exception, and re-packing one bitmap is a
///     contained problem with an obvious implementation.
///   - The alternative -- setting `ArrowArray::offset` on a decoded batch -- would mean producing
///     offsets from a reader that has never produced them, with `bool`, validity bitmaps and struct
///     children each needing the offset applied at exactly one level. That is the same class of
///     mistake as the writer's `array.offset` bug, and it is avoided entirely by slicing here.
///
/// `value_bytes` is a fixed-width column's per-row width AS THE DECODER PRODUCES IT
/// (`lance_logical_type_value_bytes`), which is 1 for `bool`. It is ignored for other kinds.
///
/// Returns false and sets `error` if the range is out of bounds or a buffer is too short for the
/// rows it claims to hold; `values` is then left untouched rather than half-trimmed.
bool slice_column_values(ColumnValues& values, std::uint64_t first, std::uint64_t count,
                         std::uint64_t total, std::size_t value_bytes, std::string& error);

/// Drop the rows `keep` marks false, compacting the survivors to the front.
///
/// This is how a Lance deletion file is applied. It sits beside `slice_column_values` and shares its
/// argument: everything at this stage is byte-addressed except the validity bitmap, which is the one
/// thing that has to be re-packed.
///
/// `keep` holds one entry per row (non-zero = keep) and must be `total` long. The two compose in one
/// order only -- deletions first, then a row range -- because a row range is expressed in LOGICAL
/// row numbers, which only exist once the deleted rows are gone.
bool compact_column_values(ColumnValues& values, const std::vector<std::uint8_t>& keep,
                           std::uint64_t total, std::size_t value_bytes, std::string& error);

}  // namespace nano_lance
