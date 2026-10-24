// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The vector index's part of a segment build (index/vector/vector_segment.hpp) on a two-fragment dataset:
// training an IVF_PQ model only (nothing written), an uncommitted segment over fragment 0 with that model
// and one over fragment 1 with the same model; load_vector_model reads each segment's model back with the
// trained centroids and codebook unchanged and the partition lengths summing to the fragment's rows.

#include "index/vector/vector_segment.hpp"

#include "index_test_util.hpp"

#include "nanolance/manifest_reader.hpp"
#include "nanolance/vector_search.hpp"

#include "lance_minimal.pb.hpp"

#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using index_test::Column;
using index_test::require;
namespace ib = nano_lance::index_build;

namespace {

constexpr int kDim = 8;

std::vector<float> vector_of(std::int64_t row) {
    std::mt19937 rng(static_cast<unsigned>(row) + 1U);
    std::uniform_real_distribution<float> u(-1.0F, 1.0F);
    std::vector<float> v(kDim);
    for (auto& x : v) {
        x = u(rng);
    }
    return v;
}

void fill(ArrowArray* a, std::size_t c, std::int64_t row) {
    if (c == 0) {
        require(ArrowArrayAppendInt(a, row) == NANOARROW_OK, "id");
    } else {
        index_test::append_vector(a, vector_of(row));
    }
}

}  // namespace

int main() {
    const auto path = index_test::scratch("nano_lance_vector_segment_test");
    const std::vector<Column> columns = {{"id", Column::Kind::Int64}, {"vec", Column::Kind::Vector, kDim}};
    index_test::write_fragment(path, false, columns, 0, 150, fill);
    index_test::write_fragment(path, true, columns, 150, 150, fill);  // PQ trains on 256 rows at least

    std::string error;
    nano_lance::pb::Manifest manifest;
    std::uint64_t version = 0;
    require(nano_lance::load_latest_manifest(path, manifest, version, error), "manifest: " + error);

    nano_lance::VectorIndexOptions options;
    options.type = "IVF_PQ";
    options.name = "vec_idx";
    options.num_partitions = 3;
    options.num_sub_vectors = 2;
    options.seed = 3;
    options.max_iters = 5;

    // Train only.
    ib::VectorModel trained;
    {
        ib::SegmentTarget target;
        target.manifest = &manifest;
        target.version = version;
        ib::VectorSegmentInputs inputs;
        inputs.trained = &trained;
        require(ib::build_vector_segment(path, "vec", options, target, inputs, error), "train: " + error);
        require(trained.partitions == 3U && trained.dim == static_cast<std::size_t>(kDim) &&
                    trained.centroids.size() == 3U * kDim,
                "3 trained centroids");
        require(trained.m == 2U && trained.codebook.size() == 2U * 256U * (kDim / 2),
                "a codebook of 2 sub-vectors of 256 codes");
        require(!std::filesystem::exists(path / "_indices"), "training writes no index files");
    }

    // A segment per fragment with the shared model.
    for (std::uint64_t fragment : {0U, 1U}) {
        const std::vector<std::uint64_t> fragments = {fragment};
        nano_lance::pb::IndexMetadata entry;
        ib::SegmentTarget target;
        target.manifest = &manifest;
        target.version = version;
        target.fragments = &fragments;
        target.out = &entry;
        ib::VectorSegmentInputs inputs;
        inputs.model = &trained;
        require(ib::build_vector_segment(path, "vec", options, target, inputs, error), "segment: " + error);
        require((entry.fragment_ids == std::vector<std::uint32_t>{static_cast<std::uint32_t>(fragment)}),
                "the segment covers its fragment");
        require(nano_lance::is_vector_index_url(entry.details_type_url), "vector details");

        ib::VectorModel loaded;
        require(ib::load_vector_model(path / "_indices" / nano_lance::pb::uuid_string(entry.uuid), loaded, error),
                "load: " + error);
        require(loaded.type == "IVF_PQ" && loaded.metric == nano_lance::VectorMetric::L2, "IVF_PQ, l2");
        require(loaded.centroids == trained.centroids, "the shared centroids, unchanged");
        require(loaded.codebook == trained.codebook && loaded.m == 2U && loaded.nbits == 8U, "the shared codebook");
        const auto rows = std::accumulate(loaded.lengths.begin(), loaded.lengths.end(), std::uint64_t{0});
        require(rows == 150U, "the partitions hold the fragment's rows");
    }

    {
        nano_lance::pb::Manifest after;
        std::uint64_t latest = 0;
        require(nano_lance::load_latest_manifest(path, after, latest, error), "manifest: " + error);
        require(latest == version && after.indices.empty(), "segment builds commit nothing");
    }
    ib::VectorModel none;
    require(!ib::load_vector_model(path / "_indices" / "missing", none, error), "no segment, no model");

    std::filesystem::remove_all(path);
    std::cout << "vector_segment: ok (train only, shared-model segments, model read back)\n";
    return 0;
}
