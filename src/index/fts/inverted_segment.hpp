// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Internal: the INVERTED (full-text) index's part of a segment build (index/segment.hpp).

#include "index/segment.hpp"
#include "nanolance/fts_search.hpp"
#include "nanolance/fts_tokenizer.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance::index_build {

/// The analyzer settings of the INVERTED index segment in `dir`.
bool load_inverted_params(const std::filesystem::path& dir, fts::AnalyzerParams& out, std::string& error);

/// InvertedIndexDetails for these analyzer settings, as Lance 12 fills them in (`granularity` is field 14).
std::vector<std::uint8_t> inverted_details(const fts::AnalyzerParams& params,
                                           DocGranularity granularity = DocGranularity::Row);

/// The row addresses of the documents of the INVERTED index segment in `dir`.
bool load_inverted_rows(const std::filesystem::path& dir, std::vector<std::uint64_t>& out, std::string& error);

/// The INVERTED index's inputs to a segment build (the rest is SegmentTarget).
struct InvertedSegmentInputs {
    /// An INVERTED index: these rows too, by address, deleted or not -- an old segment's documents,
    /// which Lance keeps (and counts in its statistics) when it merges new rows into the segment.
    const std::vector<std::uint64_t>* rows = nullptr;
};

bool build_inverted_segment(const std::filesystem::path& dataset_path, const std::string& column,
                            const InvertedIndexOptions& options, const SegmentTarget& target,
                            const InvertedSegmentInputs& inputs, std::string& error);

}  // namespace nano_lance::index_build
