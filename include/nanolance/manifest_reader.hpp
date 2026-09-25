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
/// Parse a `_versions` filename into the dataset version it denotes.
///
/// Lance has two naming schemes that sort in OPPOSITE directions: V1 `{version}.manifest` (what
/// nanolance writes) and V2 `{u64::MAX - version}.manifest`, 20-digit zero-padded (what pylance
/// writes, so the newest sorts first on an object store). Returns false for anything that is not a
/// manifest, including Lance's detached `d{version}.manifest` files.
///
/// Both the reader and the writer go through this -- the writer used to carry its own copy of the
/// naive "parse the digits" scan, so appending to a pylance dataset numbered the new manifest
/// `u64::MAX`, which reads back as version 0.
bool parse_manifest_version(const std::string& filename, std::uint64_t& version_out);

std::uint64_t highest_manifest_version(const std::filesystem::path& dataset_path, std::string& error);

/// Read raw protobuf body from a single `.manifest` file (writer wrapper: reserved + len + proto + trailer).
bool read_manifest_file(const std::filesystem::path& manifest_path, std::vector<std::uint8_t>& protobuf_body,
                          std::string& error);

/// Decode latest manifest under `dataset_path/_versions/`.
bool load_latest_manifest(const std::filesystem::path& dataset_path, pb::Manifest& out, std::uint64_t& version_out,
                          std::string& error);

}  // namespace nano_lance
