// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/// Lance's scalar indices, built the way Lance builds them and in its file layout, so that Lance
/// (pylance, LanceDB, lance-c) uses an index nanolance built exactly as one it built itself:
///
///   BTREE       sorted values with their row addresses (_indices/<uuid>/page_data.lance), in pages
///               of 4096 rows, and each page's min, max and null count (page_lookup.lance).
///   BITMAP      one row bitmap per distinct value, the null value first
///               (bitmap_page_lookup.lance). For columns with few distinct values.
///   LABEL_LIST  a bitmap index over the elements of a list column, plus the rows whose list is null
///               (global buffer 1): array_has_any / array_has_all / array_contains.
namespace nano_lance {

enum class ScalarIndexType { BTree, Bitmap, LabelList };

/// "BTREE", "BITMAP", "LABEL_LIST" (any case) as a type; false for anything else.
bool parse_scalar_index_type(const std::string& name, ScalarIndexType& type);

struct ScalarIndexOptions {
    /// Empty: "<column>_idx", Lance's default.
    std::string name;
    /// An index of the same name is replaced; without `replace`, that is an error.
    bool replace = true;
};

/// Index `column` (a top-level column or a `struct.child` path) of the latest version, committed as
/// the next version. The index covers every fragment of the version it was built on; rows appended
/// later are scanned until the index is rebuilt.
bool dataset_create_scalar_index(const std::filesystem::path& dataset_path, const std::string& column,
                                 ScalarIndexType type, const ScalarIndexOptions& options,
                                 std::uint64_t& new_version, std::string& error);

/// Remove the index `name` (every segment of it) from the latest version, committed as the next
/// version, as Lance's drop_index does: its files stay, unreferenced. "not found" when there is none.
bool dataset_drop_index(const std::filesystem::path& dataset_path, const std::string& name,
                        std::uint64_t& new_version, std::string& error);

/// An index of a version, as its manifest describes it.
struct IndexInfo {
    std::string name;
    std::string uuid;
    std::string type;  // "BTree", "Bitmap", "LabelList", "Inverted", "Vector", ...: from its details
    std::vector<std::string> fields;  // column paths
    std::vector<std::uint32_t> fragment_ids;
    std::uint64_t dataset_version = 0;
    std::uint32_t index_version = 0;
    std::string type_url;              // the index details' type, e.g. "/lance.table.BTreeIndexDetails"
    std::vector<std::int32_t> field_ids;
    std::uint64_t created_at = 0;      // milliseconds since the epoch
    std::uint64_t size_bytes = 0;      // its files
    std::uint64_t rows_indexed = 0;    // the rows of the fragments it covers, deleted ones aside
};

/// The index lines (index_search.hpp's IndexCandidates::used) a filtered read of `version` would use.
bool dataset_explain_filter(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                            const std::string& filter, std::vector<std::string>& lines, std::string& error);

/// The indices of `version` (the latest when `has_version` is false), system indices left out.
bool dataset_list_indices(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                          std::vector<IndexInfo>& out, std::string& error);

}  // namespace nano_lance
