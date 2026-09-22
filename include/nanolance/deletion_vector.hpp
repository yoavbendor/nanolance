// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance {

/// Read a fragment's deletion file into the sorted row offsets it marks deleted.
///
/// Lance stores these at `_deletions/{fragment_id}-{read_version}-{id}.{suffix}` in one of two
/// shapes, chosen by density (it switches above 5000 deleted rows):
///
///   - `.arrow` — an Arrow IPC **file** holding one non-null `uint32` column of row offsets, with
///     the body zstd-compressed. Sparse deletions.
///   - `.bin`   — a roaring bitmap in the portable serialization format. Dense deletions.
///
/// Both are parsed here directly rather than through a library. The Arrow IPC route would otherwise
/// mean enabling nanoarrow's `NANOARROW_IPC_WITH_ZSTD`, which pulls a `find_package(zstd REQUIRED)`
/// into a build that deliberately vendors zstd -- a portability regression on exactly the platforms
/// CI has only just started covering.
///
/// Offsets are *physical* row offsets within the fragment, which is what the caller must filter on
/// before any logical row numbering applies.
bool read_deletion_vector(const std::filesystem::path& dataset_path, std::uint64_t fragment_id,
                          const pb::DeletionFile& deletion_file,
                          std::vector<std::uint32_t>& out_sorted_offsets, std::string& error);

/// Parse a roaring bitmap in the portable serialization format. Exposed for testing.
bool parse_roaring_bitmap(const std::vector<std::uint8_t>& bytes,
                          std::vector<std::uint32_t>& out_sorted_values, std::string& error);

/// Parse an Arrow IPC file holding a single non-null `uint32` column. Exposed for testing.
bool parse_arrow_ipc_uint32_column(const std::vector<std::uint8_t>& bytes,
                                   std::vector<std::uint32_t>& out_values, std::string& error);

}  // namespace nano_lance
