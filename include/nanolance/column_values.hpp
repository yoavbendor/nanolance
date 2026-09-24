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

/// RLE plan for a run-length-friendly fixed-width column, computed once by the write-side "is RLE
/// beneficial?" heuristic (which already detects each maximal adjacent run while deciding) and reused
/// verbatim by the data-file encoder. `runs` holds one (row index of the run's first element, run
/// length) pair per detected run -- the row index is a view into the owning `ColumnValues::fixed`
/// buffer (valid only while that buffer is alive and unmodified), avoiding a value-bytes copy per run.
struct FixedRlePlan {
    bool computed = false;
    std::vector<std::pair<std::size_t, std::uint64_t>> runs;
};

struct ColumnValues {
    enum class Kind { FixedWidth, VariableWidth, BlobV2External } kind = Kind::FixedWidth;

    /// Arrow-convention validity bitmap (LSB-first, bit SET means the row is VALID), or empty when
    /// every row is valid. Note the inversion against the wire format: Lance stores a definition
    /// level per value where level 1 means NULL, so both encoding and decoding flip it.
    std::vector<std::uint8_t> validity;
    /// Number of rows whose validity bit is clear. Zero whenever `validity` is empty.
    std::uint64_t null_count = 0;
    /// fixed_size_list only: validity per ELEMENT -- `items_per_row` bits per row, Arrow convention --
    /// for the list's child array. Distinct from `validity`, which is per row. pyarrow marks every
    /// element of a null row null too, so an ordinary nullable vector column carries this even when no
    /// valid row has a null element. Empty when no element is null.
    std::vector<std::uint8_t> item_validity;
    std::uint64_t item_null_count = 0;
    std::uint64_t items_per_row = 0;

    /// List columns only: the list layers above the leaf, OUTERMOST first (docs/NESTED_COLUMNS.md).
    /// When present, everything else in this struct describes the leaf ITEMS -- `fixed`/`variable`
    /// hold one value per item and `validity` is item validity -- and the row count is
    /// `layers.front().length`. Empty for every non-list column, which is then unchanged.
    struct NestedLayer {
        std::vector<std::int64_t> offsets;   // length + 1 entries, starting at 0
        std::vector<std::uint8_t> validity;  // LSB-first; empty when no entry is null
        std::uint64_t null_count = 0;
        std::uint64_t length = 0;
    };
    std::vector<NestedLayer> layers;
    /// Rows accumulated so far. Needed on the write side because validity arrives one batch at a
    /// time and each batch's bits must land at the right absolute row offset -- the value buffers
    /// cannot supply that for a variable-width column, and for a borrowed fixed-width column there
    /// is no owned buffer to measure at all.
    std::uint64_t rows = 0;
    std::vector<std::uint8_t> fixed;
    VariableWidthColumnValues variable;
    BlobV2ExternalColumnValues blob_v2;
    StructuralDictPlan structural_dict_plan;
    StructuralDictRlePlan structural_dict_rle_plan;
    FixedRlePlan fixed_rle_plan;

    /// Write-side zero-copy ingest (nano_lance_writer_set_borrow_buffers): a single-batch fixed-width
    /// column borrows the caller's Arrow buffer instead of copying it into `fixed`. When set, this
    /// view -- NOT `fixed` -- holds the column's bytes; the caller guarantees the memory outlives
    /// commit. All write-side consumers must go through fixed_data()/fixed_size(). The read side never
    /// sets this, so decoded columns behave exactly as before.
    const std::uint8_t* fixed_borrowed = nullptr;
    std::size_t fixed_borrowed_size = 0;

    const std::uint8_t* fixed_data() const { return fixed_borrowed != nullptr ? fixed_borrowed : fixed.data(); }
    std::size_t fixed_size() const { return fixed_borrowed != nullptr ? fixed_borrowed_size : fixed.size(); }
};

}  // namespace nano_lance
