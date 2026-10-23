// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Internal: the vector index's part of a segment build (index/segment.hpp).

#include "index/segment.hpp"
#include "nanolance/vector_search.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance::index_build {

/// A trained IVF (and PQ) model, as an index's files hold it.
struct VectorModel {
    std::string type;  // "IVF_FLAT" / "IVF_PQ" / "IVF_RQ" / "IVF_HNSW_SQ"
    VectorMetric metric = VectorMetric::L2;
    std::size_t dim = 0;
    std::size_t partitions = 0;
    std::vector<float> centroids;  // partitions * dim
    /// Hamming (IVF_FLAT over uint8 vectors): the centroids as bytes, partitions * dim; `centroids` is empty.
    std::vector<std::uint8_t> binary_centroids;
    std::uint32_t nbits = 8;
    std::size_t m = 0;             // PQ sub-vectors
    std::vector<float> codebook;   // [m][2^nbits][dim / m]
    std::vector<std::uint64_t> lengths;  // rows per partition
    std::vector<std::uint8_t> rq_signs;  // IVF_RQ: the fast rotation's sign bytes (the shared model)
    double sq_start = 0;  // IVF_HNSW_SQ: the SQ bounds and the graph parameters
    double sq_end = 0;
    std::uint32_t hnsw_m = 20;
    std::uint32_t hnsw_ef_construction = 150;
    std::uint32_t hnsw_max_level = 7;
    /// Which parts the model carries. A model read from an index has them all; one handed in from
    /// outside (lance-c's precomputed models) may carry the IVF centroids alone, and the rest is
    /// trained as for a new index.
    bool has_codebook = true;
    bool has_sq = true;
};

/// The model of the vector index segment in `dir` (an _indices/<uuid> directory).
bool load_vector_model(const std::filesystem::path& dir, VectorModel& out, std::string& error);

/// The vector index's inputs to a segment build (the rest is SegmentTarget).
struct VectorSegmentInputs {
    /// A vector index: this model instead of training one (its type, metric, PQ settings win).
    const VectorModel* model = nullptr;
    /// A vector index with `model`: adjust its partitions to this target size as Lance does when it
    /// merges rows in (split the oversized, join the undersized); 0 leaves them as they are.
    std::size_t rebalance_target = 0;
    /// A vector index: these details (VectorIndexDetails) instead of the options' own.
    const std::vector<std::uint8_t>* details = nullptr;
    /// A vector index: train the model only -- the IVF centroids, and for IVF_PQ the codebook --
    /// into this, and write nothing.
    VectorModel* trained = nullptr;
};

bool build_vector_segment(const std::filesystem::path& dataset_path, const std::string& column,
                          const VectorIndexOptions& options, const SegmentTarget& target,
                          const VectorSegmentInputs& inputs, std::string& error);

}  // namespace nano_lance::index_build
