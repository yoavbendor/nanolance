// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/column_values.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

/// Whether a structural encoding pays for a column the writer buffered: each check scans the column's
/// values and, where it says yes, leaves the plan it computed in the ColumnValues (runs, dictionary).
namespace nano_lance::encoding_choice {

/// Is bitpacking actually smaller than storing the values flat?
///
/// This mirrors Lance's own rule (`rust/lance-encoding/src/compression.rs`: a bitpacked block is
/// only offered when its estimated size is strictly below `raw_bytes`) rather than inventing a
/// threshold. Costing it per 1024-value chunk is what makes the answer right: each chunk carries its
/// own width word, so a column that needs the full width is strictly LARGER bitpacked than flat, and
/// pays a FastLanes transpose on every read for it.
///
/// nanolance used to bitpack every integer column unconditionally. On a column of random `uint64`
/// ids -- the `high_card` bench shape, and any hash/uuid column -- that made the file 2.4% bigger and
/// the read 0.85 ms instead of 0.50 ms. Stock Lance writes `Flat(64)` for the same data.
bool bitpack_beats_flat(const nano_lance::ColumnValues& cv, std::size_t bpv);

bool fixed_column_rle_plan(nano_lance::ColumnValues& cv, std::size_t bpv);

bool variable_column_constant_value(const nano_lance::ColumnValues& cv, std::vector<std::uint8_t>& value_out);

bool variable_column_dict_rle_beneficial(nano_lance::ColumnValues& cv);

bool variable_column_dict_beneficial(nano_lance::ColumnValues& cv);

}  // namespace nano_lance::encoding_choice
