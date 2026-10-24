// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Nearest-neighbour search and vector index builds (nanolance/vector_search.hpp) on a small two-fragment
// dataset of 8-dimensional vectors: the exact search without an index; IVF_FLAT, IVF_PQ, IVF_SQ and
// IVF_HNSW_SQ built by dataset_create_vector_index (their manifest entries, index.idx / auxiliary.idx read
// back by read_vector_index_model, read_vector_index_partition and vector_index_details); an IVF_FLAT
// search probing every partition equals the exact one, the quantized kinds find a vector's own row first;
// a row appended after the build is found through the exact search of the unindexed fragment; metric
// names parse; build and search refusals.

#include "nanolance/vector_search.hpp"

#include "index_test_util.hpp"

#include "nanolance/manifest_reader.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using index_test::Column;
using index_test::require;

namespace {

constexpr int kDim = 8;
constexpr std::int64_t kRows = 300;

std::vector<std::vector<float>> make_vectors(std::int64_t n) {
    std::mt19937 rng(7);
    std::normal_distribution<float> normal(0.0F, 1.0F);
    std::vector<std::vector<float>> out(static_cast<std::size_t>(n), std::vector<float>(kDim));
    for (auto& v : out) {
        for (auto& x : v) {
            x = normal(rng);
        }
    }
    return out;
}

const auto kVectors = make_vectors(kRows + 1);
const std::vector<Column> kColumns = {{"id", Column::Kind::Int64}, {"vec", Column::Kind::Vector, kDim}};

void fill(ArrowArray* a, std::size_t c, std::int64_t row) {
    if (c == 0) {
        require(ArrowArrayAppendInt(a, row) == NANOARROW_OK, "id");
    } else {
        index_test::append_vector(a, kVectors[static_cast<std::size_t>(row)]);
    }
}

// Row address of row `i`: fragment 0 holds 0..199, fragment 1 200..299, fragment 2 the appended one.
std::uint64_t addr_of(std::int64_t i) {
    const auto u = static_cast<std::uint64_t>(i);
    return i < 200 ? u : (i < kRows ? (1ULL << 32U) | (u - 200U) : (2ULL << 32U));
}

nano_lance::NearestQuery query_for(std::int64_t row, std::uint64_t k) {
    nano_lance::NearestQuery q;
    q.column = "vec";
    q.key = kVectors[static_cast<std::size_t>(row)];
    q.k = k;
    return q;
}

nano_lance::NearestResult nearest(const std::filesystem::path& path, const nano_lance::NearestQuery& q) {
    nano_lance::NearestResult out;
    std::string error;
    require(nano_lance::dataset_nearest(path, q, out, error), "nearest: " + error);
    require(out.row_ids.size() == out.distances.size(), "one distance per row");
    for (std::size_t i = 1; i < out.distances.size(); ++i) {
        require(out.distances[i - 1] <= out.distances[i], "nearest first");
    }
    return out;
}

void build(const std::filesystem::path& path, const std::string& type, std::uint32_t partitions) {
    nano_lance::VectorIndexOptions o;
    o.type = type;
    o.name = "vec_idx";
    o.replace = true;
    o.num_partitions = partitions;
    o.num_sub_vectors = 4;
    o.seed = 1;
    o.max_iters = 10;
    std::uint64_t version = 0;
    std::string error;
    require(nano_lance::dataset_create_vector_index(path, "vec", o, version, error), type + " build: " + error);
}

nano_lance::pb::IndexMetadata the_index(const std::filesystem::path& path) {
    nano_lance::pb::Manifest manifest;
    std::uint64_t version = 0;
    std::string error;
    require(nano_lance::load_latest_manifest(path, manifest, version, error), "manifest: " + error);
    require(manifest.indices.size() == 1U, "one index entry");
    return manifest.indices.front();
}

}  // namespace

int main() {
    const auto path = index_test::scratch("nano_lance_vector_search_test");
    index_test::write_fragment(path, false, kColumns, 0, 200, fill);
    index_test::write_fragment(path, true, kColumns, 200, kRows - 200, fill);
    std::string error;

    {
        nano_lance::VectorMetric m{};
        require(nano_lance::parse_vector_metric("Euclidean", m) && m == nano_lance::VectorMetric::L2, "euclidean is l2");
        require(nano_lance::parse_vector_metric("COSINE", m) && m == nano_lance::VectorMetric::Cosine, "cosine");
        require(nano_lance::parse_vector_metric("dot", m) && m == nano_lance::VectorMetric::Dot, "dot");
        require(!nano_lance::parse_vector_metric("manhattan", m), "an unknown metric");
        require(std::string(nano_lance::vector_metric_name(nano_lance::VectorMetric::Hamming)) == "hamming", "names");
    }

    // Without an index: exact, the row itself first at distance 0.
    const auto exact = nearest(path, query_for(42, 10));
    require(exact.row_ids.size() == 10U && exact.row_ids.front() == addr_of(42) && exact.distances.front() == 0.0F,
            "the exact search finds the row itself first");

    // IVF_FLAT: its files and model, and with every partition probed, the exact answer.
    build(path, "IVF_FLAT", 4);
    {
        const auto index = the_index(path);
        require(nano_lance::is_vector_index_url(index.details_type_url), "VectorIndexDetails");
        require(nano_lance::vector_index_type(index.details_value) == "IVF_FLAT", "the details name IVF_FLAT");
        require((index.fragment_ids == std::vector<std::uint32_t>{0, 1}), "both fragments covered");
        const auto dir = path / "_indices" / nano_lance::pb::uuid_string(index.uuid);
        require(std::filesystem::is_regular_file(dir / "index.idx") &&
                    std::filesystem::is_regular_file(dir / "auxiliary.idx"),
                "index.idx and auxiliary.idx");
        nano_lance::VectorIndexModel model;
        require(nano_lance::read_vector_index_model(dir, model, error), "model: " + error);
        require(model.partitions == 4U && model.dim == static_cast<std::size_t>(kDim) &&
                    model.centroids.size() == 4U * kDim,
                "4 centroids of dimension 8");
        std::uint64_t total = 0;
        for (const auto n : model.partition_sizes) {
            total += n;
        }
        require(total == static_cast<std::uint64_t>(kRows), "the partitions hold every row");
        require(model.index_metadata.find("IVF_FLAT") != std::string::npos, "lance:index names the type");
        nano_lance::VectorIndexPartition part;
        require(nano_lance::read_vector_index_partition(dir, 0, true, part, error), "partition: " + error);
        require(part.column == "flat" && part.width == static_cast<std::size_t>(kDim) &&
                    part.floats.size() == part.ids.size() * kDim && part.ids.size() == model.partition_sizes[0],
                "partition 0's rows and vectors");
        require(!nano_lance::read_vector_index_partition(dir, 99, false, part, error), "no partition 99");
        nano_lance::VectorIndexDetails details;
        require(nano_lance::vector_index_details(dir, details, error), "details: " + error);
        require(details.type == "IVF_FLAT" && details.metric == "l2" && !details.pq, "described as IVF_FLAT, l2");

        auto q = query_for(42, 10);
        q.minimum_nprobes = 4;
        q.maximum_nprobes = 4;
        const auto all = nearest(path, q);
        require(all.row_ids == exact.row_ids, "IVF_FLAT probing every partition is exact");
        q.use_index = false;
        require(nearest(path, q).row_ids == exact.row_ids, "use_index=false is the exact search");
    }

    // The quantized kinds: each finds a vector's own row among its nearest.
    for (const char* type : {"IVF_PQ", "IVF_SQ", "IVF_HNSW_SQ"}) {
        build(path, type, 2);
        const auto index = the_index(path);
        require(nano_lance::vector_index_type(index.details_value) == type, std::string("the details name ") + type);
        auto q = query_for(7, 5);
        q.minimum_nprobes = 2;
        q.maximum_nprobes = 2;
        q.refine_factor = 4;
        const auto r = nearest(path, q);
        require(!r.row_ids.empty() && r.row_ids.front() == addr_of(7), std::string(type) + " finds the row itself");
    }

    // A row appended after the build is searched exactly.
    index_test::write_fragment(path, true, kColumns, kRows, 1, fill);
    {
        auto q = query_for(kRows, 3);
        const auto r = nearest(path, q);
        require(r.row_ids.front() == addr_of(kRows), "the unindexed row is found");
        q.fast_search = true;
        const auto fast = nearest(path, q);
        require(std::find(fast.row_ids.begin(), fast.row_ids.end(), addr_of(kRows)) == fast.row_ids.end(),
                "fast_search leaves the unindexed fragment out");
    }

    // Refusals.
    {
        nano_lance::VectorIndexOptions o;
        o.type = "IVF_PQ";
        o.num_sub_vectors = 3;  // does not divide 8
        o.replace = true;
        std::uint64_t v = 0;
        require(!nano_lance::dataset_create_vector_index(path, "vec", o, v, error), "num_sub_vectors must divide dim");
        o.type = "IVF_NOPE";
        require(!nano_lance::dataset_create_vector_index(path, "vec", o, v, error), "an unknown type");
        o.type = "IVF_FLAT";
        require(!nano_lance::dataset_create_vector_index(path, "id", o, v, error), "an int column is no vector");
        auto q = query_for(1, 3);
        q.key.resize(3);
        nano_lance::NearestResult out;
        require(!nano_lance::dataset_nearest(path, q, out, error), "a query of the wrong dimension");
    }

    std::filesystem::remove_all(path);
    std::cout << "vector_search: ok (exact, IVF_FLAT, IVF_PQ, IVF_SQ, IVF_HNSW_SQ, unindexed rows)\n";
    return 0;
}
