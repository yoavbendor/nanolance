// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/// Nearest-neighbour search over a vector column (a fixed-size list of floats), as Lance's
/// `nearest` answers it: from the column's IVF_FLAT / IVF_PQ index where there is one (built by
/// Lance or by nanolance; docs/VECTOR_INDEX.md), exactly over the fragments it does not cover, and
/// exactly over everything without one.
namespace nano_lance {

enum class VectorMetric { L2, Cosine, Dot };

/// "l2" / "euclidean", "cosine", "dot" (any case). False for anything else.
bool parse_vector_metric(const std::string& name, VectorMetric& out);
const char* vector_metric_name(VectorMetric metric);  // "l2", "cosine", "dot"

struct NearestQuery {
    bool has_version = false;
    std::uint64_t version = 0;
    std::string column;
    std::vector<float> key;  // the query vector
    std::uint64_t k = 10;
    /// Partitions to probe: at least `minimum_nprobes` (Lance's adaptive count when it is 1 and
    /// there is no maximum), at most `maximum_nprobes` (all when unset). `nprobes` sets both.
    std::uint32_t minimum_nprobes = 1;
    std::optional<std::uint32_t> maximum_nprobes;
    /// Re-score the best `k * refine_factor` index candidates exactly and return the best k.
    std::optional<std::uint32_t> refine_factor;
    /// The distance; unset: the index's (L2 without one). A metric other than the index's searches
    /// exactly, as Lance does.
    std::optional<VectorMetric> metric;
    bool use_index = true;
    /// Only rows with lower_bound <= distance < upper_bound.
    std::optional<float> lower_bound;
    std::optional<float> upper_bound;
    /// An SQL filter: applied before the search with `prefilter` (k matching rows), else to the k
    /// nearest (fewer may pass).
    std::optional<std::string> filter;
    bool prefilter = false;
    /// Search only the fragments the index covers.
    bool fast_search = false;
};

struct NearestResult {
    std::vector<std::uint64_t> row_ids;  // row addresses, nearest first (ties by row id)
    std::vector<float> distances;
    std::vector<std::string> plan;  // what answered, one line per step
};

bool dataset_nearest(const std::filesystem::path& dataset_path, const NearestQuery& query, NearestResult& out,
                     std::string& error);

struct VectorIndexOptions {
    std::string type;  // "IVF_FLAT" or "IVF_PQ"
    std::string name;  // empty: <column>_idx
    VectorMetric metric = VectorMetric::L2;
    bool replace = false;
    /// Partitions: as given, else rows / target_partition_size (Lance's 4096 for IVF_FLAT, 8192 for
    /// IVF_PQ), at least 1 and at most 4096.
    std::optional<std::uint32_t> num_partitions;
    std::optional<std::uint32_t> target_partition_size;
    std::uint32_t num_sub_vectors = 0;  // IVF_PQ: required; must divide the dimension
    std::uint32_t num_bits = 8;         // IVF_PQ: 8 or 4
    std::uint32_t max_iters = 50;       // k-means iterations (IVF and PQ), as Lance's defaults
    std::uint32_t sample_rate = 256;    // k-means trains on sample_rate * k vectors at most
    std::optional<std::uint64_t> seed;  // the random sample and initial centroids
};

/// Build an IVF_FLAT / IVF_PQ index on `column` (a fixed-size list of float32) in Lance's format --
/// one pylance and LanceDB search as their own -- and commit it as the next version. Null vectors
/// and vectors with NaN or infinite values are left out, as Lance leaves them out.
bool dataset_create_vector_index(const std::filesystem::path& dataset_path, const std::string& column,
                                 const VectorIndexOptions& options, std::uint64_t& new_version, std::string& error);

/// What Lance calls a vector index, from its manifest details (VectorIndexDetails): "IVF_PQ",
/// "IVF_FLAT", "IVF_SQ", "IVF_RQ", "IVF_HNSW_PQ", ... Empty when the details do not say.
std::string vector_index_type(const std::vector<std::uint8_t>& details);

/// The same, falling back to what the index's files say (`dir`: _indices/<uuid>).
std::string vector_index_type(const std::filesystem::path& dir, const std::vector<std::uint8_t>& details);

/// Whether an index's details type is a vector index's (Lance has used more than one package name).
bool is_vector_index_url(const std::string& url);

}  // namespace nano_lance
