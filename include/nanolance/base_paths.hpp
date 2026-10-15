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

/// The manifest's base paths. Empty when it has none.
bool read_base_paths(const pb::Manifest& manifest, std::vector<BasePath>& out, std::string& error);

/// Replace the manifest's base paths (field 18 of its unknown bytes).
void write_base_paths(pb::Manifest& manifest, const std::vector<BasePath>& bases);

/// DataFile.base_id (field 7): nullopt when absent, which means the dataset's own directory.
bool data_file_base_id(const pb::DataFile& file, std::optional<std::uint32_t>& out, std::string& error);
void set_data_file_base_id(pb::DataFile& file, std::optional<std::uint32_t> id);

/// DeletionFile.base_id (field 7), with the same meaning.
bool deletion_file_base_id(const pb::DeletionFile& file, std::optional<std::uint32_t>& out, std::string& error);
void set_deletion_file_base_id(pb::DeletionFile& file, std::optional<std::uint32_t> id);

/// The directory a data file of `manifest` lives in: `<dataset>/data` when it has no base, else the
/// base's directory (`<path>` when the base is not a dataset root, `<path>/data` when it is). False
/// (and `error`) when its base_id names no base path.
bool data_file_dir(const std::filesystem::path& dataset_path, const std::vector<BasePath>& bases,
                   const pb::DataFile& file, std::filesystem::path& out, std::string& error);

}  // namespace nano_lance
