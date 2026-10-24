// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/expr.hpp"
#include "nanolance/lance_table_reader.hpp"

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

/// Answering a filter from a version's scalar indices (BTree, Bitmap, LabelList; built by Lance or by
/// nanolance): which rows of which fragments may pass. A scan then reads only those rows and still
/// applies the whole filter to them, so an index changes how much is read, never what a read returns.
namespace nano_lance {

/// `filter` must be bound. Nothing in `out` when no index applies to it. False only when an index
/// that applies cannot be read.
bool index_candidates(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                      const expr::Expression& filter, IndexCandidates& out, std::string& error);

}  // namespace nano_lance
