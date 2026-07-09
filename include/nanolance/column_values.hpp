// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nano_lance {

struct VariableWidthColumnValues {
    /// Serialized offsets including terminal offset (num_rows + 1 entries).
    std::vector<std::uint8_t> offsets;
    std::vector<std::uint8_t> data;
    bool large = false;
};

struct BlobV2ExternalColumnValues {
    /// Packed per-row payload (see `blob_v2_external.cpp`) concatenated for all rows.
    std::vector<std::uint8_t> packed_payload;
    /// Byte length of each row's packed record (for Lance control buffer).
    std::vector<std::uint32_t> row_packed_sizes;
    /// Opt-in URI dictionary (nanolance extension): distinct external URIs stored once.
    /// When non-empty, packed rows carry `blob_id` = index here and an empty inline `uri`.
    /// Empty means Lance-compatible per-row inline URIs.
    std::vector<std::string> uri_dictionary;
    /// Dedup lookup used only while building `uri_dictionary` on the write side.
    std::unordered_map<std::string, std::uint32_t> uri_to_id;
};

/// Structural-dictionary plan for a scattered low-cardinality string column, computed once by the
/// write-side "is dictionary encoding beneficial?" heuristic and reused verbatim by the data-file
/// encoder so the (dedup + per-row index) scan runs a single time instead of twice. `distinct` and
/// `indices` are views/ids into the owning `VariableWidthColumnValues::data`, valid only while that
/// buffer is alive and unmodified (true between the heuristic and the immediately following write).
struct StructuralDictPlan {
    bool computed = false;
    std::vector<std::string_view> distinct;  // distinct values in first-appearance order (id == index)
    std::vector<std::uint32_t> indices;      // per-row dictionary index into `distinct`
};

/// Dictionary+RLE plan for a run-length-friendly variable-width column, computed once by the write-side
/// "is dict-RLE beneficial?" heuristic (which already detects each maximal adjacent run while deciding)
/// and reused verbatim by the data-file encoder, so building the (run-keyed, not row-keyed) dictionary
/// and the run sequence happens once instead of being fully rebuilt from a per-row scan. `distinct` is
/// a view into the owning `VariableWidthColumnValues::data`, valid only while that buffer is alive and
/// unmodified. `runs` holds one (dictionary index, run length) pair per detected run, in row order, with
/// run length *not* yet split at Lance's 255-per-sub-run cap (the encoder does that).
struct StructuralDictRlePlan {
    bool computed = false;
    std::vector<std::string_view> distinct;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> runs;
};

struct ColumnValues {
    enum class Kind { FixedWidth, VariableWidth, BlobV2External } kind = Kind::FixedWidth;
    std::vector<std::uint8_t> fixed;
    VariableWidthColumnValues variable;
    BlobV2ExternalColumnValues blob_v2;
    StructuralDictPlan structural_dict_plan;
    StructuralDictRlePlan structural_dict_rle_plan;
};

}  // namespace nano_lance
