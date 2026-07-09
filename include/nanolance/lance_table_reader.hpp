// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <nanoarrow/nanoarrow.h>

#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance {

/// Read all committed rows from a nano_lance_writer dataset into Arrow batches (writer parity only).
/// Rebuilds ingest-shaped schemas (e.g. dematerialized `lance.blob.v2` children).
///
/// \p trusted_input (default false) opts a self-produced pipeline out of the untrusted-input DoS/OOM
/// budget checks (declared zstd size, row/column/manifest-element counts vs. `ReadLimits`) — set it only
/// when the dataset is known-good (e.g. round-tripping your own writer output), never for a file that
/// could originate from another party. It does NOT disable any bounds check: every offset/size is still
/// validated against the real buffer with overflow-safe arithmetic before use, exactly as in the default
/// path. Because those budget checks already cost nothing measurable (validated once per page/header,
/// not per value — see docs/SAFETY.md), this is an escape hatch for exotic legitimate files that exceed
/// the default budget, not a speed option.
bool lance_table_read_dataset(const std::filesystem::path& dataset_path, ArrowSchema& out_schema,
                              std::vector<ArrowArray>& out_batches, std::string& error,
                              bool trusted_input = false);

/// Same as lance_table_read_dataset but only reads the named top-level columns (and their children).
/// All other columns are skipped during decode — significantly faster when the table has many columns
/// and only a small subset is needed (e.g. fuselance only needs filename + blob columns).
bool lance_table_read_dataset_projected(const std::filesystem::path& dataset_path,
                                        const std::vector<std::string>& column_names,
                                        ArrowSchema& out_schema,
                                        std::vector<ArrowArray>& out_batches,
                                        std::string& error, bool trusted_input = false);

}  // namespace nano_lance
