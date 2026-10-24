// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// One partition's HNSW graph (index/vector/hnsw_build.hpp), built over float vectors (IVF_HNSW_FLAT's
// storage) on one thread: every node has level-0 neighbours, none links to itself or past the rows, a
// node's edges on a level are at most 2m (level 0) or m and each names a node on that level; the
// level counts fall level by level and the entry point is on the top level; the distances are squared L2;
// a greedy walk from the entry point over the graph finds a stored vector's own node; the levels and
// the entry point do not change with the thread count. SQ codes build too.

#include "index/vector/hnsw_build.hpp"

#include "index_test_util.hpp"

#include "nanolance/parallel.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using index_test::require;
namespace hb = nano_lance::hnsw_build;

namespace {

constexpr std::size_t kN = 400;
constexpr std::size_t kDim = 6;

float l2(const float* a, const float* b) {
    float s = 0;
    for (std::size_t i = 0; i < kDim; ++i) {
        s += (a[i] - b[i]) * (a[i] - b[i]);
    }
    return s;
}

std::size_t height_of(const hb::Graph& g, std::size_t node) { return g.nodes[node].size(); }

}  // namespace

int main() {
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(0.0F, 1.0F);
    std::vector<float> vectors(kN * kDim);
    for (auto& x : vectors) {
        x = u(rng);
    }
    hb::Codes codes;
    codes.storage = hb::Storage::Flat;
    codes.vectors = vectors.data();
    codes.n = kN;
    codes.dim = kDim;
    hb::Params params;
    params.m = 8;
    params.ef_construction = 40;
    params.max_level = 4;

    nano_lance::parallel::set_threads(1);
    const auto g = hb::build(codes, params);
    require(g.nodes.size() == kN, "one node per row");
    require(g.level_count.size() == params.max_level, "a count per level");
    require(g.entry_point < kN, "the entry point is a node");
    std::size_t top = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        require(height_of(g, i) >= 1U, "every node is on level 0");
        top = std::max(top, height_of(g, i));
        for (std::size_t level = 0; level < height_of(g, i); ++level) {
            const auto& edges = g.nodes[i][level];
            require(edges.size() <= (level == 0 ? 2 * params.m : params.m), "at most 2m / m edges");
            if (level == 0) {
                require(!edges.empty(), "every node has level-0 neighbours");
            }
            for (std::size_t e = 0; e < edges.size(); ++e) {
                require(edges[e].id < kN && edges[e].id != i, "an edge to another row");
                require(height_of(g, edges[e].id) > level, "the neighbour is on that level too");
                require(std::fabs(edges[e].dist - l2(&vectors[i * kDim], &vectors[edges[e].id * kDim])) < 1e-4F,
                        "the edge's distance is squared L2");
            }
        }
    }
    require(height_of(g, g.entry_point) == top, "the entry point is on the top level");
    for (std::size_t l = 1; l < g.level_count.size(); ++l) {
        require(g.level_count[l] <= g.level_count[l - 1], "fewer nodes on each higher level");
    }

    // A greedy walk on level 0 from the entry point, towards a stored vector, reaches its node.
    std::size_t found = 0;
    for (std::size_t target = 0; target < kN; target += 37) {
        const float* q = &vectors[target * kDim];
        std::size_t at = g.entry_point;
        for (std::size_t level = height_of(g, at); level-- > 0;) {
            for (bool moved = true; moved;) {
                moved = false;
                for (const auto& e : g.nodes[at][level]) {
                    if (l2(&vectors[e.id * kDim], q) < l2(&vectors[at * kDim], q)) {
                        at = e.id;
                        moved = true;
                    }
                }
            }
        }
        found += at == target ? 1U : 0U;
    }
    require(found >= 9U, "the greedy walk finds most stored vectors: " + std::to_string(found) + " of 11");

    // The levels and entry point are Lance's fixed-seed draws, whatever the threads.
    nano_lance::parallel::set_threads(4);
    const auto g4 = hb::build(codes, params);
    require(g4.entry_point == g.entry_point && g4.level_count == g.level_count, "the same levels on 4 threads");
    for (std::size_t i = 0; i < kN; ++i) {
        require(height_of(g4, i) == height_of(g, i), "each node's height is the same");
    }

    // SQ codes: the same structure over 8-bit codes.
    std::vector<std::uint8_t> sq(kN * kDim);
    for (std::size_t i = 0; i < sq.size(); ++i) {
        sq[i] = static_cast<std::uint8_t>(vectors[i] * 255.0F);
    }
    hb::Codes sq_codes;
    sq_codes.storage = hb::Storage::Sq;
    sq_codes.data = sq.data();
    sq_codes.n = kN;
    sq_codes.dim = kDim;
    sq_codes.start = 0.0;
    sq_codes.end = 1.0;
    const auto gs = hb::build(sq_codes, params);
    require(gs.nodes.size() == kN && gs.entry_point == g.entry_point, "an SQ graph, with the same levels");
    for (std::size_t i = 0; i < kN; ++i) {
        require(!gs.nodes[i][0].empty(), "every SQ node has level-0 neighbours");
    }

    std::cout << "hnsw_build: ok (graph structure, distances, levels, greedy search)\n";
    return 0;
}
