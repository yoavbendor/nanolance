// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/// What a dataset's manifests say about it -- versions, fragments, configuration -- for the APIs that
/// present a dataset the way Lance does (the pylance-compatible Python module, the lance-c compatible
/// C API). Reads of the rows themselves are in lance_table_reader.hpp.
namespace nano_lance {

struct DatasetFile {
    std::string path;                   // relative to <dataset>/data
    std::vector<std::int32_t> fields;   // field ids it holds
    std::uint32_t major_version = 0;
    std::uint32_t minor_version = 0;
    std::uint64_t size_bytes = 0;
};

struct DatasetFragmentInfo {
    std::uint64_t id = 0;
    std::uint64_t physical_rows = 0;  // rows in the data files, deleted ones included
    std::uint64_t deleted_rows = 0;
    std::vector<DatasetFile> files;
    bool has_deletion_file = false;
    std::string deletion_file;  // relative to the dataset, when present

    std::uint64_t rows() const { return physical_rows - deleted_rows; }
};

struct DatasetVersionInfo {
    std::uint64_t version = 0;
    std::int64_t timestamp_ns = 0;  // since the epoch, UTC; 0 if the manifest did not record one
    std::string tag;
    std::string writer_library;
    std::string writer_version;
    std::string transaction_file;  // relative to <dataset>/_transactions; empty when none was written
};

/// One version of a dataset, as its manifest describes it.
struct DatasetInfo {
    DatasetVersionInfo version;
    std::uint64_t latest_version = 0;
    std::string data_storage_version;  // e.g. "2.2"
    std::vector<DatasetFragmentInfo> fragments;  // in manifest order
    std::uint64_t max_fragment_id = 0;
    bool has_max_fragment_id = false;
    std::map<std::string, std::string> config;
    std::map<std::string, std::string> table_metadata;
    std::map<std::string, std::string> schema_metadata;
    std::uint64_t reader_feature_flags = 0;
    std::uint64_t writer_feature_flags = 0;

    std::uint64_t num_rows() const {
        std::uint64_t n = 0;
        for (const auto& f : fragments) {
            n += f.rows();
        }
        return n;
    }
    bool stable_row_ids() const { return (reader_feature_flags & 2U) != 0U; }
};

/// Describe version `version` of the dataset, or its latest when `has_version` is false.
bool dataset_info(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                  DatasetInfo& out, std::string& error);

/// Every version of the dataset, oldest first.
bool dataset_versions(const std::filesystem::path& dataset_path, std::vector<DatasetVersionInfo>& out,
                      std::string& error);

/// The newest version number; 0 with an error when the path holds no dataset.
bool dataset_latest_version(const std::filesystem::path& dataset_path, std::uint64_t& out, std::string& error);

/// Make `version` the latest again: a new version with its schema and fragments (Lance's restore).
bool dataset_restore(const std::filesystem::path& dataset_path, std::uint64_t version, std::uint64_t& new_version,
                     std::string& error);

/// Upsert (and, for `remove`, delete) table config keys in a new version.
bool dataset_update_config(const std::filesystem::path& dataset_path,
                           const std::map<std::string, std::string>& upsert, const std::vector<std::string>& remove,
                           std::uint64_t& new_version, std::string& error);

/// Replace the table metadata (`replace`) or upsert into it, in a new version.
bool dataset_update_table_metadata(const std::filesystem::path& dataset_path,
                                   const std::map<std::string, std::string>& values, bool replace,
                                   std::uint64_t& new_version, std::string& error);

/// Replace the schema metadata (`replace`) or upsert into it, in a new version.
bool dataset_update_schema_metadata(const std::filesystem::path& dataset_path,
                                    const std::map<std::string, std::string>& values, bool replace,
                                    std::uint64_t& new_version, std::string& error);

/// A field of a dataset's schema, as its manifest holds it (pre-order; a child names its parent).
struct DatasetField {
    std::int32_t id = 0;
    std::int32_t parent_id = -1;
    std::string name;
    std::string logical_type;
    bool nullable = true;
    std::int32_t encoding = 0;  // Lance's Encoding: 0 none, 1 plain, 2 var-binary, 3 dictionary, 4 RLE
    std::map<std::string, std::string> metadata;
};

bool dataset_fields(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                    std::vector<DatasetField>& out, std::string& error);

/// One field's metadata change: set (a value) or remove (none) keys, or replace the whole map
/// with the keys given a value (Lance's UpdateMap).
struct FieldMetadataUpdate {
    std::vector<std::pair<std::string, std::optional<std::string>>> entries;
    bool replace = false;
};

/// Apply `updates` (by field id) in a new version. A field id the schema lacks is an error.
bool dataset_update_field_metadata(const std::filesystem::path& dataset_path,
                                   const std::map<std::int32_t, FieldMetadataUpdate>& updates,
                                   std::uint64_t& new_version, std::string& error);

/// Bytes on disk per field of `version`'s schema, in schema (pre-order) order: the sizes of the pages
/// of each field's columns, summed over the data files (Lance's data_stats; a column a file does not
/// map to a field counts for the field before it).
bool dataset_data_stats(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                        std::vector<std::pair<std::int32_t, std::uint64_t>>& out, std::string& error);

}  // namespace nano_lance
