// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace nano_lance {

/// Publish `manifest` (a version's manifest, changed) as the version after the one it was read at
/// (`manifest.version`), stamped with the time and nanolance as its writer. Fails with "commit
/// conflict" if another writer took that version since.
bool commit_next_version(const std::filesystem::path& dataset_path, pb::Manifest manifest, std::uint64_t& new_version,
                         std::string& error);

}  // namespace nano_lance
