// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/data_file_writer.hpp"
#include "nanolance/schema_mapper.hpp"

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace nano_lance {

/// How a commit relates to what is already at the path, as pylance's write modes name it.
enum class CommitMode {
    Create,     // fail if the dataset exists
    Append,     // add fragments to the latest version (its schema)
    Overwrite,  // a new version holding only these fragments (creating the dataset if need be)
};

/// A data file written for a commit, and its row count.
struct NewFragment {
    DataFileResult data_file;
    std::uint64_t rows = 0;
};

struct CommitExtras {
    /// Replaces the schema metadata (Arrow's), when set.
    const std::map<std::string, std::vector<std::uint8_t>>* schema_metadata = nullptr;
    /// Upserted into the table metadata.
    std::map<std::string, std::string> table_metadata;
};

/// Publish one new version holding `fragments` (possibly none), numbered after the latest one. Table
/// config and metadata carry over; the version records its time and nanolance as its writer.
bool commit_dataset_version(const std::filesystem::path& dataset_path, const LanceSchemaMapping& mapping,
                            const std::vector<NewFragment>& fragments, CommitMode mode, std::uint64_t& version,
                            std::string& error, const CommitExtras& extras = {});

/// The version nanolance records in the manifests it writes.
const char* nanolance_writer_version();

bool write_dataset_manifest(const std::filesystem::path& dataset_path,
                            const LanceSchemaMapping& mapping,
                            const DataFileResult& data_file,
                            std::uint64_t rows,
                            bool is_append,
                            std::uint64_t& version,
                            std::string& error);

/// Encode + atomically publish a fully-formed manifest as `<dataset_path>/_versions/<manifest.version>.manifest`
/// (creates `_versions/` if needed). The single source of truth for the manifest framing/footer, shared by
/// write_dataset_manifest and the dataset stitcher.
bool publish_manifest(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                      std::string& error);

}  // namespace nano_lance
