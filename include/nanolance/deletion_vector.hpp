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
///
/// `max_values` caps how many offsets the parse may produce before it gives up. It matters because
/// this format amplifies: a run container is four bytes on disk and can emit 65,536 values, and a
/// bitmap holds up to 65,536 containers -- roughly 640 KiB of input for 16 GiB of output. Every other
/// decode path budgets its output against `default_read_limits()`; 0 means "use that budget", and the
/// real caller passes the manifest's `num_deleted_rows`, which is exact.
/// Write the deletion file of fragment `fragment_id` listing `deleted` (its physical row offsets, all of
/// them -- a deletion file replaces the fragment's previous one), named as Lance names them
/// (`_deletions/{fragment}-{read_version}-{id}.arrow`), and describe it in `out`.
bool write_deletion_file(const std::filesystem::path& dataset_path, std::uint64_t fragment_id,
                         std::uint64_t read_version, const std::vector<std::uint32_t>& deleted,
                         pb::DeletionFile& out, std::string& error);

bool parse_roaring_bitmap(const std::vector<std::uint8_t>& bytes,
                          std::vector<std::uint32_t>& out_sorted_values, std::string& error,
                          std::uint64_t max_values = 0);

/// Parse an Arrow IPC file holding a single non-null `uint32` column. Exposed for testing.
///
/// `max_values` bounds the parse the same way it does for `parse_roaring_bitmap`, and for the same
/// reason: a compressed buffer's uncompressed size is declared, not derived. Both the Arrow
/// length prefix and the zstd frame header state it, and both come from the file -- making them
/// agree proves only that the file is self-consistent about its lie. 0 means "use
/// `default_read_limits()`"; the real caller passes the manifest's `num_deleted_rows`.
bool parse_arrow_ipc_uint32_column(const std::vector<std::uint8_t>& bytes,
                                   std::vector<std::uint32_t>& out_values, std::string& error,
                                   std::uint64_t max_values = 0);

}  // namespace nano_lance
