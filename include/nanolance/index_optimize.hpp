// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/// Keeping indexes up to date, as Lance's optimize_indices does (lance/src/index/append.rs).
///
/// An index covers the fragments it was built on. Rows appended later, and rows compaction moved
/// into new fragments, are searched without it until it is optimized. Optimizing an index writes
/// one new segment and commits it in place of the segments it replaces:
///
///   num_indices_to_merge = N (Lance's default 1)  the last N segments are replaced by one covering
///                                                 their fragments that still exist and every
///                                                 fragment no segment covers
///   num_indices_to_merge = 0                      a new segment over the uncovered fragments alone,
///                                                 the old ones kept
///   retrain (vector indexes)                      a new model trained on every fragment, replacing
///                                                 every segment
///
/// A new segment takes the parameters of the segment it follows: a scalar index's type, an INVERTED
/// index's analyzer, a vector index's trained model (centroids, PQ codebook) -- new rows are
/// assigned to the existing partitions and encoded with the existing codebook, nothing is retrained.
/// An index with nothing to fold in (every fragment covered, at most one segment selected) is left
/// as it is. All the indexes optimized are committed together, as one version.
namespace nano_lance {

struct OptimizeIndicesOptions {
    /// The indexes to optimize, by name; empty: every index of the dataset.
    std::vector<std::string> index_names;
    std::optional<std::uint32_t> num_indices_to_merge;
    bool retrain = false;
};

struct OptimizeIndicesResult {
    std::vector<std::string> optimized;  // the indexes given a new segment
    bool committed = false;              // false when no index had anything to fold in
    std::uint64_t version = 0;           // the version committed (or the latest, when none was)
};

/// Optimize the indexes of the latest version. Fails, committing nothing, when an index to optimize
/// is of a kind nanolance cannot build (IVF_HNSW_*, NGRAM, ZONEMAP, ...): name the others instead.
bool dataset_optimize_indices(const std::filesystem::path& dataset_path, const OptimizeIndicesOptions& options,
                              OptimizeIndicesResult& result, std::string& error);

}  // namespace nano_lance
