// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// index_optimize.hpp: keeping an index up to date as Lance's optimize_indices does. A BTREE index is built on a
// one-fragment dataset, a fragment is appended (not covered), and optimizing with Lance's default merge (1)
// replaces the segment by one covering both; with num_indices_to_merge = 0 a new segment over the uncovered
// fragment alone is added beside the old one. An index with nothing to fold in commits nothing, and an
// unknown index name is an error.

#include "nanolance/index_optimize.hpp"

#include "nanolance/dataset.hpp"
#include "nanolance/scalar_index.hpp"

#include "support.hpp"

#include <set>
#include <string>
#include <vector>

using dataset_test::require;
namespace nl = nano_lance;

namespace {

/// The fragments each segment of `name` covers, in manifest order.
std::vector<std::set<std::uint32_t>> coverage(const std::filesystem::path& path, const std::string& name) {
    std::string error;
    std::vector<nl::IndexInfo> indices;
    require(nl::dataset_list_indices(path, false, 0, indices, error), "list indices: " + error);
    std::vector<std::set<std::uint32_t>> out;
    for (const auto& index : indices) {
        if (index.name == name) {
            out.emplace_back(index.fragment_ids.begin(), index.fragment_ids.end());
        }
    }
    return out;
}

void append(const std::filesystem::path& path, const std::vector<std::int64_t>& ids) {
    NanoLanceWriter w{};
    require(nano_lance_writer_init_append(&w, path.string().c_str(), 0) == NANO_LANCE_OK, "init_append");
    dataset_test::write_ids(&w, ids);
    require(nano_lance_writer_commit(&w, true) == NANO_LANCE_OK, "append commit");
    require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
}

}  // namespace

int main() {
    const auto path = dataset_test::scratch("nano_lance_index_optimize_test");
    dataset_test::write_dataset(path, {{1, 2, 3}});
    std::string error;
    std::uint64_t v = 0;
    nl::ScalarIndexOptions sopts;
    require(nl::dataset_create_scalar_index(path, "id", nl::ScalarIndexType::BTree, sopts, v, error),
            "create index: " + error);
    require(coverage(path, "id_idx") == std::vector<std::set<std::uint32_t>>{{0}}, "one segment over fragment 0");

    // Nothing appended: nothing to fold in.
    nl::OptimizeIndicesOptions options;
    nl::OptimizeIndicesResult result;
    require(nl::dataset_optimize_indices(path, options, result, error), "optimize: " + error);
    require(!result.committed && result.optimized.empty() && result.version == v,
            "an index covering every fragment is left as it is");

    // An append is not covered; the default merge replaces the segment by one over both fragments.
    append(path, {4, 5});
    require(nl::dataset_optimize_indices(path, options, result, error), "optimize: " + error);
    require(result.committed && result.optimized == std::vector<std::string>{"id_idx"}, "id_idx optimized");
    require(coverage(path, "id_idx") == std::vector<std::set<std::uint32_t>>{{0, 1}},
            "still one segment, now covering both fragments");
    std::uint64_t latest = 0;
    require(nl::dataset_latest_version(path, latest, error) && latest == result.version, "the version committed");
    require(nl::dataset_optimize_indices(path, options, result, error) && !result.committed,
            "optimizing again finds nothing to do");

    // num_indices_to_merge = 0: a new segment over the new fragment alone, beside the old.
    append(path, {6});
    options.num_indices_to_merge = 0;
    require(nl::dataset_optimize_indices(path, options, result, error) && result.committed, "optimize 0: " + error);
    const auto two = coverage(path, "id_idx");
    require(two.size() == 2 && std::set<std::set<std::uint32_t>>(two.begin(), two.end()) ==
                                   std::set<std::set<std::uint32_t>>{{0, 1}, {2}},
            "a second segment over the new fragment alone, beside the old");

    // The rows an indexed filter returns are the filter's, over all three fragments.
    const std::string filter = "id >= 3";
    require(dataset_test::read_column(path, "id", &filter) == std::vector<std::int64_t>{3, 4, 5, 6},
            "the optimized index answers like a scan");

    // An unknown index name is an error and commits nothing.
    nl::OptimizeIndicesOptions unknown;
    unknown.index_names = {"nope"};
    std::uint64_t before = 0;
    require(nl::dataset_latest_version(path, before, error), "latest");
    require(!nl::dataset_optimize_indices(path, unknown, result, error), "an unknown index is an error");
    require(nl::dataset_latest_version(path, latest, error) && latest == before, "and commits nothing");

    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::cout << "index_optimize: OK\n";
    return 0;
}
