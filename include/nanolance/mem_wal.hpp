// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nano_lance {

/// The system index that records MemWAL's configuration (Lance's `MEM_WAL_INDEX_NAME`).
inline constexpr const char* kMemWalIndexName = "__lance_mem_wal";

/// The MemWAL index's details (lance.table.MemWalIndexDetails) as the manifest holds them: the value
/// of its index_details Any. `out` is empty when the version has no MemWAL index.
bool dataset_mem_wal_details(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                             std::optional<std::vector<std::uint8_t>>& out, std::string& error);

/// Initialize MemWAL: commit the `__lance_mem_wal` index, built from `details` (the encoded
/// MemWalIndexDetails), on top of `base_version`, as Lance's InitializeMemWalBuilder::execute does
/// (a CreateIndex transaction; the index covers no fragments and has no files). The caller checks
/// its arguments against that version. A concurrent commit makes this fail with "commit conflict".
bool dataset_initialize_mem_wal(const std::filesystem::path& dataset_path, std::uint64_t base_version,
                                const std::vector<std::uint8_t>& details, std::uint64_t& new_version,
                                std::string& error);

}  // namespace nano_lance
