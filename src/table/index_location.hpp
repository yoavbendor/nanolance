// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Where an index's files go, and the field and the path its metadata names (internal).

#pragma once

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance::index_location {

/// The field at dotted `path` (its parts in `parts`), or null.
const pb::Field* find_field(const pb::Manifest& manifest, const std::string& path, std::vector<std::string>& parts);

/// The path of a JsonIndexDetails (its field 1; empty when absent). False on a malformed message.
bool json_index_path(const std::vector<std::uint8_t>& details, std::string& path);

/// Creates the directory `dir` an index's files go in (its parents included). Nothing on an object store,
/// which has no directories (io::is_object_store_output).
bool create_index_dir(const std::filesystem::path& dir, std::string& error);

/// Removes an index's directory after a failed build or a superseded segment: the directory on local
/// disk; on an object store, every object under it (best effort: an unreferenced index directory is
/// harmless, and cleanup removes it).
void remove_index_dir(const std::filesystem::path& dir);

}  // namespace nano_lance::index_location
