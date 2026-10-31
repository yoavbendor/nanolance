// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/column_values.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
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

/// The field-level knobs of the `lance-encoding:` family that move nanolance's own choices, named as
/// pylance names them. A field that sets none of them keeps nanolance's defaults, so its layout is unchanged.
struct ColumnEncodingOptions {
    /// `lance-encoding:rle-threshold` was given. pylance then takes RLE only when the run count is
    /// below `rle_threshold` times the row count; `rle_threshold == 0` disables RLE for the column.
    bool rle_set = false;
    double rle_threshold = 0.0;
    /// `lance-encoding:dict-divisor` (pylance default 2): a dictionary may hold at most rows / divisor
    /// distinct values. 0 is read as 1, as pylance does.
    std::size_t dict_divisor = 2;
    /// `lance-encoding:dict-size-ratio` (pylance default 0.8, valid in (0, 1]): a dictionary is kept
    /// only when its encoded size is below this fraction of the flat size.
    double dict_size_ratio = 0.8;
    /// `lance-encoding:dict-values-compression`: empty (pylance's default, lz4), "none" or "lz4". Other values
    /// are not handled for fixed-width dictionaries, which then stay as they were.
    std::string dict_values_compression;
    /// `lance-encoding:dict-values-compression-level` for zstd (Lance's default 3).
    int dict_values_level = 3;
};

bool fixed_column_rle_plan(nano_lance::ColumnValues& cv, std::size_t bpv,
                           const ColumnEncodingOptions& options = {});

bool variable_column_constant_value(const nano_lance::ColumnValues& cv, std::vector<std::uint8_t>& value_out);

bool variable_column_dict_rle_beneficial(nano_lance::ColumnValues& cv, const ColumnEncodingOptions& options = {});

bool variable_column_dict_beneficial(nano_lance::ColumnValues& cv, const ColumnEncodingOptions& options = {});

/// Lance's should_dictionary_encode for a 64-bit fixed-width column (W1p): a 4,096-value sample in 32 blocks
/// (a unique ratio of 0.98 or more rejects it, and the projected cardinality must stay within rows / divisor,
/// capped at 100,000), then the distinct values within that threshold and the size budget (dict-size-ratio
/// of the flat size). On success `cv.structural_dict_plan` holds the dictionary as pylance stores its values:
/// InlineBitpacking when one 1,024-value block is smaller than the raw block, else OutOfLineBitpacking above
/// 1,024 entries, else the general codec (lz4 by default, none for Flat). `value_bytes` 16 is a 128-bit column
/// (decimal128, fixed_size_binary(16)), as Lance 12 dictionary-encodes it: never bitpacked, the codec stores it.
bool fixed_column_dict_beneficial(nano_lance::ColumnValues& cv, const ColumnEncodingOptions& options = {},
                                  std::size_t value_bytes = 8U);

}  // namespace nano_lance::encoding_choice
