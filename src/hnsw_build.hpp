// SPDX-License-Identifier: Apache-2.0
//
// The HNSW graph of one IVF_HNSW_SQ partition, built as lance-index's HnswBuilder builds it (internal).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nano_lance::hnsw_build {

/// lance-index HnswBuildParams; Lance's defaults.
struct Params {
    std::uint16_t max_level = 7;
    std::size_t m = 20;
    std::size_t ef_construction = 150;
};

struct Edge {
    float dist;
    std::uint32_t id;
};

/// The storage a partition's graph is built over, and how Lance measures between its rows.
enum class Storage {
    Sq,    // 8-bit SQ codes: data, n rows of dim codes; start / end are the SQ bounds
    Flat,  // float vectors: vectors, n rows of dim floats (IVF_HNSW_FLAT)
    Pq,    // PQ codes: data, n rows of dim (= code bytes); pq_table as in Lance (IVF_HNSW_PQ)
};

/// One partition's rows and how Lance measures between them.
struct Codes {
    Storage storage = Storage::Sq;
    const std::uint8_t* data = nullptr;  // Sq and Pq: n rows of dim bytes
    const float* vectors = nullptr;      // Flat: n rows of dim floats
    std::size_t n = 0;
    std::size_t dim = 0;
    bool dot = false;  // dot product; else squared L2 (also for cosine, over normalized vectors)
    double start = 0;  // the SQ bounds
    double end = 0;
    // Pq: the pairwise table [m][2^nbits][2^nbits] of sub-vector distances (PQ's pairwise_distance_table),
    // and the sub-vector count and bit width (4-bit codes are two sub-vectors per byte).
    const float* pq_table = nullptr;
    std::size_t m = 0;
    std::uint32_t nbits = 8;
};

struct Graph {
    std::uint32_t entry_point = 0;
    /// Nodes on each level, params.max_level entries (the ones above the graph's height 0).
    std::vector<std::size_t> level_count;
    /// nodes[i][level]: node i's neighbours on that level (its height + 1 levels), with their
    /// distances, in Lance's order.
    std::vector<std::vector<std::vector<Edge>>> nodes;
};

/// Build the graph over `codes`. Nodes are inserted on nanolance's threads (parallel.hpp), as Lance
/// inserts them on its own, so the edges can differ from run to run unless threads() is 1; the node
/// levels and the entry point are Lance's (its fixed-seed level draws).
Graph build(const Codes& codes, const Params& params);

}  // namespace nano_lance::hnsw_build
