// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance {

/// Highest numeric `N` for existing `_versions/N.manifest` (0 if none).
std::uint64_t highest_manifest_version(const std::filesystem::path& dataset_path, std::string& error);

/// Read raw protobuf body from a single `.manifest` file (writer wrapper: reserved + len + proto + trailer).
bool read_manifest_file(const std::filesystem::path& manifest_path, std::vector<std::uint8_t>& protobuf_body,
                          std::string& error);

/// Decode latest manifest under `dataset_path/_versions/`.
bool load_latest_manifest(const std::filesystem::path& dataset_path, pb::Manifest& out, std::uint64_t& version_out,
                          std::string& error);

}  // namespace nano_lance
