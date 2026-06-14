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
bool lance_table_read_dataset(const std::filesystem::path& dataset_path, ArrowSchema& out_schema,
                              std::vector<ArrowArray>& out_batches, std::string& error);

}  // namespace nano_lance
