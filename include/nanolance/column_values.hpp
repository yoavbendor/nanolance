#pragma once

#include <cstdint>
#include <string>
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

struct ColumnValues {
    enum class Kind { FixedWidth, VariableWidth, BlobV2External } kind = Kind::FixedWidth;
    std::vector<std::uint8_t> fixed;
    VariableWidthColumnValues variable;
    BlobV2ExternalColumnValues blob_v2;
};

}  // namespace nano_lance
