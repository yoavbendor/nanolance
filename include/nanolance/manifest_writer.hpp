#pragma once

#include "nanolance/data_file_writer.hpp"
#include "nanolance/schema_mapper.hpp"

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace nano_lance {

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
