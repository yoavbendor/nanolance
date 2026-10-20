// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nano_lance {

/// One base path of a dataset (Lance's BasePath, manifest field 18): a directory outside the
/// dataset's own where data files can live. Ids count from 1 in manifest order; 0 and an absent
/// base_id both mean the dataset itself.
struct BasePath {
    std::uint32_t id = 0;
    std::string name;
    bool has_name = false;
    bool is_dataset_root = false;  // true: the files are under <path>/data (a dataset of its own)
    std::string path;
};

/// One BasePath message (Lance's lance.table.BasePath), as a Transaction's UpdateBases carries it.
bool decode_base_path_message(const std::uint8_t* data, std::size_t size, BasePath& out, std::string& error);

/// The manifest's base paths. Empty when it has none.
bool read_base_paths(const pb::Manifest& manifest, std::vector<BasePath>& out, std::string& error);

/// The branch a manifest belongs to (Manifest field 20); empty for the main branch.
bool read_manifest_branch(const pb::Manifest& manifest, std::string& out, std::string& error);

/// Replace the manifest's base paths (field 18 of its unknown bytes).
void write_base_paths(pb::Manifest& manifest, const std::vector<BasePath>& bases);

/// DataFile.base_id (field 7): nullopt when absent, which means the dataset's own directory.
bool data_file_base_id(const pb::DataFile& file, std::optional<std::uint32_t>& out, std::string& error);
void set_data_file_base_id(pb::DataFile& file, std::optional<std::uint32_t> id);

/// DeletionFile.base_id (field 7), with the same meaning.
bool deletion_file_base_id(const pb::DeletionFile& file, std::optional<std::uint32_t>& out, std::string& error);
void set_deletion_file_base_id(pb::DeletionFile& file, std::optional<std::uint32_t> id);

/// IndexMetadata.base_id (field 9): nullopt when absent, which means the dataset's own directory.
bool index_base_id(const pb::IndexMetadata& index, std::optional<std::uint32_t>& out, std::string& error);

/// The directory a data file of `manifest` lives in: `<dataset>/data` when it has no base, else the
/// base's directory (`<path>` when the base is not a dataset root, `<path>/data` when it is). False
/// (and `error`) when its base_id names no base path. A base_id of 0 names base path 0 like any other
/// id (a branch's inherited files do); only an absent base_id means the dataset's own directory.
bool data_file_dir(const std::filesystem::path& dataset_path, const std::vector<BasePath>& bases,
                   const pb::DataFile& file, std::filesystem::path& out, std::string& error);

/// The `_deletions` directory of a deletion file with `base_id` (absent: the dataset's own). A base
/// it names must be a dataset root, as Lance requires.
bool deletion_dir(const std::filesystem::path& dataset_path, const std::vector<BasePath>& bases,
                  std::optional<std::uint32_t> base_id, std::filesystem::path& out, std::string& error);

/// The `_deletions` directory a deletion file of `manifest` lives in (its base_id, if any, looked up in
/// the manifest's base paths).
bool deletion_file_dir(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                       const pb::DeletionFile& file, std::filesystem::path& out, std::string& error);
bool deletion_file_dir(const std::filesystem::path& dataset_path, const std::vector<BasePath>& bases,
                       const pb::DeletionFile& file, std::filesystem::path& out, std::string& error);

/// The directory an index of `manifest` lives in: `<dataset>/_indices/<uuid>`, or the base path's
/// (IndexMetadata.base_id) with the uuid appended.
bool index_dir_of(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                  const pb::IndexMetadata& index, std::filesystem::path& out, std::string& error);

/// The `_indices` directory of an index with `base_id` (absent: the dataset's own). A base that is not
/// a dataset root is the indices directory itself, as Lance reads it.
bool indices_dir(const std::filesystem::path& dataset_path, const std::vector<BasePath>& bases,
                 std::optional<std::uint32_t> base_id, std::filesystem::path& out, std::string& error);

}  // namespace nano_lance
