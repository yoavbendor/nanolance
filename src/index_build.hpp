// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Internal: building one index segment over chosen fragments of a version, without committing it.
// The public builders (scalar_index.hpp, vector_search.hpp, fts_search.hpp) are this plus a commit;
// optimize_indices (index_maintenance.hpp) is this with the parameters -- and for a vector index the
// trained model -- of the segments it replaces.

#include "lance_minimal.pb.hpp"
#include "nanolance/fts_search.hpp"
#include "nanolance/scalar_index.hpp"
#include "nanolance/vector_search.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance::index_build {

/// A trained IVF (and PQ) model, as an index's files hold it.
struct VectorModel {
    std::string type;  // "IVF_FLAT" / "IVF_PQ"
    VectorMetric metric = VectorMetric::L2;
    std::size_t dim = 0;
    std::size_t partitions = 0;
    std::vector<float> centroids;  // partitions * dim
    std::uint32_t nbits = 8;
    std::size_t m = 0;             // PQ sub-vectors
    std::vector<float> codebook;   // [m][2^nbits][dim / m]
    std::vector<std::uint64_t> lengths;  // rows per partition
};

/// The model of the vector index segment in `dir` (an _indices/<uuid> directory).
bool load_vector_model(const std::filesystem::path& dir, VectorModel& out, std::string& error);

/// The analyzer settings of the INVERTED index segment in `dir`.
bool load_inverted_params(const std::filesystem::path& dir, fts::AnalyzerParams& out, std::string& error);

/// The row addresses of the documents of the INVERTED index segment in `dir`.
bool load_inverted_rows(const std::filesystem::path& dir, std::vector<std::uint64_t>& out, std::string& error);

/// Where a segment is built: on `manifest` (version `version`), over `fragments` (every fragment of
/// the version when null). The segment's files are written under _indices/<uuid>/ and its manifest
/// entry is returned in `out`; nothing is committed.
struct SegmentTarget {
    const pb::Manifest* manifest = nullptr;
    std::uint64_t version = 0;
    const std::vector<std::uint64_t>* fragments = nullptr;
    pb::IndexMetadata* out = nullptr;
    /// The fragments the entry claims; `fragments` when null.
    const std::vector<std::uint64_t>* coverage = nullptr;
    /// An INVERTED index: these rows too, by address, deleted or not -- an old segment's documents,
    /// which Lance keeps (and counts in its statistics) when it merges new rows into the segment.
    const std::vector<std::uint64_t>* rows = nullptr;
    /// A vector index: this model instead of training one (its type, metric, PQ settings win).
    const VectorModel* model = nullptr;
    /// A vector index with `model`: adjust its partitions to this target size as Lance does when it
    /// merges rows in (split the oversized, join the undersized); 0 leaves them as they are.
    std::size_t rebalance_target = 0;
    /// A vector index: these details (VectorIndexDetails) instead of the options' own.
    const std::vector<std::uint8_t>* details = nullptr;
};

bool build_scalar_segment(const std::filesystem::path& dataset_path, const std::string& column,
                          ScalarIndexType type, const ScalarIndexOptions& options, const SegmentTarget& target,
                          std::string& error);
bool build_vector_segment(const std::filesystem::path& dataset_path, const std::string& column,
                          const VectorIndexOptions& options, const SegmentTarget& target, std::string& error);
bool build_inverted_segment(const std::filesystem::path& dataset_path, const std::string& column,
                            const InvertedIndexOptions& options, const SegmentTarget& target, std::string& error);

}  // namespace nano_lance::index_build
