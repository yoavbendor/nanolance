// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Internal: the scalar index's part of a segment build (index/segment.hpp).

#include "index/segment.hpp"
#include "nanolance/scalar_index.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace nano_lance::index_build {

/// An NGRAM index's posting lists: token -> the row ids holding it (as stored, sorted when merged).
using NGramPostings = std::map<std::uint32_t, std::vector<std::uint64_t>>;

/// The posting lists of the ngram_postings.lance in `dir`.
bool load_ngram_postings(const std::filesystem::path& dir, NGramPostings& out, std::string& error);

/// The scalar index's inputs to a segment build (the rest is SegmentTarget).
struct ScalarSegmentInputs {
    /// An NGRAM index: these posting lists (an old segment's, already filtered) merged with the new rows'.
    const NGramPostings* postings = nullptr;
};

bool build_scalar_segment(const std::filesystem::path& dataset_path, const std::string& column,
                          ScalarIndexType type, const ScalarIndexOptions& options, const SegmentTarget& target,
                          const ScalarSegmentInputs& inputs, std::string& error);

}  // namespace nano_lance::index_build
