// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// optimize_indices: one new segment per index, over the fragments its replaced segments still cover
// and those no segment covers, with the replaced segments' parameters (index_optimize.hpp). The
// selection follows lance/src/index/append.rs; the new segment is built the way Lance rebuilds a
// scalar segment (rebuild_scalar_segment) and, for a vector index, with the existing model, as its
// default (non-retraining) merge assigns new rows to the existing partitions.

#include "nanolance/index_optimize.hpp"

#include "index_build.hpp"
#include "nanolance/dataset_commit.hpp"
#include "nanolance/index_maintenance.hpp"
#include "nanolance/manifest_reader.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <system_error>

namespace nano_lance {
namespace {

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// A field's dotted path, from the root down.
bool field_path(const pb::Manifest& manifest, std::int32_t id, std::string& out) {
    std::map<std::int32_t, const pb::Field*> by_id;
    for (const auto& f : manifest.fields) {
        by_id[f.id] = &f;
    }
    out.clear();
    for (std::size_t depth = 0; depth < by_id.size() + 1U; ++depth) {
        const auto it = by_id.find(id);
        if (it == by_id.end()) {
            return false;
        }
        out = out.empty() ? it->second->name : it->second->name + "." + out;
        id = it->second->parent_id;
        if (id < 0) {
            return true;
        }
    }
    return false;
}

std::filesystem::path segment_dir(const std::filesystem::path& dataset_path, const pb::IndexMetadata& index) {
    return dataset_path / "_indices" / pb::uuid_string(index.uuid);
}

enum class Kind { BTree, Bitmap, LabelList, Inverted, Vector, Other };

Kind kind_of(const std::filesystem::path& dataset_path, const pb::IndexMetadata& index) {
    const auto& url = index.details_type_url;
    if (ends_with(url, "BTreeIndexDetails")) {
        return Kind::BTree;
    }
    if (ends_with(url, "BitmapIndexDetails")) {
        return Kind::Bitmap;
    }
    if (ends_with(url, "LabelListIndexDetails")) {
        return Kind::LabelList;
    }
    if (ends_with(url, "InvertedIndexDetails")) {
        return Kind::Inverted;
    }
    std::error_code ec;
    if (ends_with(url, "VectorIndexDetails") ||
        (url.empty() && std::filesystem::exists(segment_dir(dataset_path, index) / "index.idx", ec))) {
        return Kind::Vector;
    }
    return Kind::Other;
}

/// A VectorIndexDetails' target_partition_size (field 2), or Lance's default for the index type
/// (lance-index-core IndexType::target_partition_size).
std::size_t target_partition_size(const pb::IndexMetadata& index, const std::string& type) {
    const auto& b = index.details_value;
    std::size_t i = 0;
    const auto varint = [&](std::uint64_t& v) {
        v = 0;
        for (unsigned shift = 0; i < b.size() && shift < 64U; shift += 7U) {
            const std::uint8_t byte = b[i++];
            v |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0U) {
                return true;
            }
        }
        return false;
    };
    while (i < b.size()) {
        std::uint64_t key = 0;
        std::uint64_t v = 0;
        if (!varint(key)) {
            break;
        }
        const auto wire = key & 7U;
        if (wire == 0U) {
            if (!varint(v)) {
                break;
            }
            if ((key >> 3U) == 2U && v > 0U) {
                return static_cast<std::size_t>(v);
            }
        } else if (wire == 2U) {
            if (!varint(v) || v > b.size() - i) {
                break;
            }
            i += static_cast<std::size_t>(v);
        } else if (wire == 5U) {
            i += 4;
        } else if (wire == 1U) {
            i += 8;
        } else {
            break;
        }
    }
    return type == "IVF_FLAT" ? 4096U : (type.rfind("IVF_HNSW", 0) == 0 ? std::size_t{1} << 20U : 8192U);
}

/// Whether Lance's optimize would rebalance these partitions with nothing new to fold in: one over
/// the split threshold, or (several partitions) one under the join threshold.
bool needs_rebalance(const std::vector<std::uint64_t>& lengths, std::size_t target) {
    const std::size_t split = 4U * target;
    const std::size_t join = 25U * target / 100U;
    bool small = false;
    for (const auto n : lengths) {
        if (n > split) {
            return true;
        }
        small = small || n < join;
    }
    return lengths.size() > 1U && small;
}

/// One index's new segment, built on `manifest` (version `version`); `replaced` the segments it
/// supersedes. False with an empty `error` when there is nothing to do.
bool optimize_one(const std::filesystem::path& dataset_path, const pb::Manifest& manifest, std::uint64_t version,
                  const std::vector<const pb::IndexMetadata*>& segments, const OptimizeIndicesOptions& options,
                  pb::IndexMetadata& out, std::vector<const pb::IndexMetadata*>& replaced, std::string& error) {
    const auto& last = *segments.back();
    const Kind kind = kind_of(dataset_path, last);
    if (kind == Kind::Other) {
        error = "index '" + last.name + "' (" + last.details_type_url +
                ") is of a kind nanolance cannot build, so it cannot be optimized";
        return false;
    }
    for (const auto* s : segments) {
        if (kind_of(dataset_path, *s) != kind || s->fields != last.fields) {
            error = "index '" + last.name + "' has segments of different kinds";
            return false;
        }
        if (!s->has_fragment_bitmap) {
            error = "index '" + last.name + "' has a segment without fragment coverage";
            return false;
        }
    }
    if (last.fields.size() != 1U) {
        error = "index '" + last.name + "' covers " + std::to_string(last.fields.size()) +
                " fields; nanolance optimizes single-field indexes";
        return false;
    }
    std::string column;
    if (!field_path(manifest, last.fields.front(), column)) {
        error = "index '" + last.name + "': its field is no longer in the schema";
        return false;
    }

    std::set<std::uint64_t> live;
    for (const auto& f : manifest.fragments) {
        live.insert(f.id);
    }
    std::set<std::uint64_t> covered;
    for (const auto* s : segments) {
        covered.insert(s->fragment_ids.begin(), s->fragment_ids.end());
    }
    std::vector<std::uint64_t> unindexed;
    for (const auto id : live) {
        if (covered.count(id) == 0U) {
            unindexed.push_back(id);
        }
    }

    const bool retrain = options.retrain && kind == Kind::Vector;
    std::size_t merge = std::min<std::size_t>(options.num_indices_to_merge.value_or(1U), segments.size());
    if (retrain) {
        merge = segments.size();
    }
    replaced.assign(segments.end() - static_cast<std::ptrdiff_t>(merge), segments.end());
    index_build::VectorModel model;
    std::size_t target_size = 0;
    if (kind == Kind::Vector) {
        if (!index_build::load_vector_model(segment_dir(dataset_path, last), model, error)) {
            error = "index '" + last.name + "': " + error;
            return false;
        }
        target_size = target_partition_size(last, model.type);
    }
    if (!retrain && unindexed.empty() && replaced.size() <= 1U) {
        // Nothing to fold in. A vector index is still rebalanced when Lance's default would be.
        if (kind != Kind::Vector || options.num_indices_to_merge.has_value()) {
            return false;
        }
        std::vector<std::uint64_t> sizes(model.partitions, 0);
        for (const auto* s : segments) {
            index_build::VectorModel m;
            if (!index_build::load_vector_model(segment_dir(dataset_path, *s), m, error)) {
                error = "index '" + last.name + "': " + error;
                return false;
            }
            for (std::size_t p = 0; p < std::min(sizes.size(), m.lengths.size()); ++p) {
                sizes[p] += m.lengths[p];
            }
        }
        if (!needs_rebalance(sizes, target_size)) {
            return false;
        }
        replaced = segments;
    }
    std::set<std::uint64_t> target(unindexed.begin(), unindexed.end());
    for (const auto* s : replaced) {
        for (const auto id : s->fragment_ids) {
            if (live.count(id) != 0U) {
                target.insert(id);
            }
        }
    }
    if (retrain) {
        target = live;
    }
    const std::vector<std::uint64_t> fragments(target.begin(), target.end());
    // The parameters of the segment the new one follows.
    const pb::IndexMetadata& reference = replaced.empty() ? last : *replaced.back();

    std::vector<std::uint64_t> old_rows;
    index_build::SegmentTarget where;
    where.manifest = &manifest;
    where.version = version;
    where.fragments = &fragments;
    where.out = &out;
    switch (kind) {
    case Kind::BTree:
    case Kind::Bitmap:
    case Kind::LabelList: {
        ScalarIndexOptions o;
        o.name = last.name;
        const ScalarIndexType type = kind == Kind::BTree    ? ScalarIndexType::BTree
                                     : kind == Kind::Bitmap ? ScalarIndexType::Bitmap
                                                            : ScalarIndexType::LabelList;
        if (!index_build::build_scalar_segment(dataset_path, column, type, o, where, error)) {
            return false;
        }
        break;
    }
    case Kind::Inverted: {
        InvertedIndexOptions o;
        o.name = last.name;
        // As Lance merges new rows into a segment: its documents stay (deleted rows too -- they count
        // in its statistics until a rebuild), the uncovered fragments' live rows are added.
        if (!retrain) {
            for (const auto* s : replaced) {
                std::vector<std::uint64_t> rows;
                if (!index_build::load_inverted_rows(segment_dir(dataset_path, *s), rows, error)) {
                    error = "index '" + last.name + "': " + error;
                    return false;
                }
                for (const auto r : rows) {
                    if (live.count(r >> 32U) != 0U) {
                        old_rows.push_back(r);
                    }
                }
            }
            std::sort(old_rows.begin(), old_rows.end());
            old_rows.erase(std::unique(old_rows.begin(), old_rows.end()), old_rows.end());
            where.rows = &old_rows;
            where.fragments = &unindexed;
            where.coverage = &fragments;
        }
        if (!index_build::load_inverted_params(segment_dir(dataset_path, reference), o.params, error) ||
            !index_build::build_inverted_segment(dataset_path, column, o, where, error)) {
            error = "index '" + last.name + "': " + error;
            return false;
        }
        break;
    }
    case Kind::Vector: {
        if (&reference != &last && !index_build::load_vector_model(segment_dir(dataset_path, reference), model, error)) {
            error = "index '" + last.name + "': " + error;
            return false;
        }
        VectorIndexOptions o;
        o.name = last.name;
        o.type = model.type;
        o.metric = model.metric;
        o.num_partitions = static_cast<std::uint32_t>(model.partitions);
        o.num_sub_vectors = static_cast<std::uint32_t>(model.m);
        o.num_bits = model.nbits;
        o.hnsw_m = model.hnsw_m;
        o.hnsw_ef_construction = model.hnsw_ef_construction;
        o.hnsw_max_level = model.hnsw_max_level;
        if (!retrain) {
            where.model = &model;
            where.rebalance_target = target_size;
        }
        where.details = &reference.details_value;
        if (!index_build::build_vector_segment(dataset_path, column, o, where, error)) {
            error = "index '" + last.name + "': " + error;
            return false;
        }
        break;
    }
    case Kind::Other:
        break;
    }
    // The replaced segments' index version, so the new segment reads as theirs did.
    if (out.index_version != reference.index_version && kind != Kind::Vector) {
        out.index_version = reference.index_version;
        out = pb::make_index_metadata(out.uuid, out.fields, out.name, out.dataset_version, out.fragment_ids,
                                      out.details_type_url, out.index_version, out.created_at, out.files,
                                      out.details_value);
    }
    return true;
}

}  // namespace

bool optimize_indices_once(const std::filesystem::path& dataset_path, const OptimizeIndicesOptions& options,
                           OptimizeIndicesResult& result, std::string& error);

bool dataset_optimize_indices(const std::filesystem::path& dataset_path, const OptimizeIndicesOptions& options,
                              OptimizeIndicesResult& result, std::string& error) {
    return retry_on_conflict([&] { return optimize_indices_once(dataset_path, options, result, error); }, error);
}

bool optimize_indices_once(const std::filesystem::path& dataset_path, const OptimizeIndicesOptions& options,
                           OptimizeIndicesResult& result, std::string& error) {
    error.clear();
    result = OptimizeIndicesResult{};
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        return false;
    }
    result.version = version;
    if ((manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U && !manifest.indices.empty()) {
        error = "optimizing indexes of a dataset with stable row ids is not supported";
        return false;
    }
    // The indexes, by name, in the manifest's order; each one's segments in order.
    std::vector<std::string> names;
    std::map<std::string, std::vector<const pb::IndexMetadata*>> by_name;
    for (const auto& index : manifest.indices) {
        if (is_system_index(index)) {
            continue;
        }
        if (by_name.count(index.name) == 0U) {
            names.push_back(index.name);
        }
        by_name[index.name].push_back(&index);
    }
    if (!options.index_names.empty()) {
        for (const auto& n : options.index_names) {
            if (by_name.count(n) == 0U) {
                error = "Index not found: " + n;
                return false;
            }
        }
        names = options.index_names;
    }

    std::vector<pb::IndexMetadata> built;
    std::set<const pb::IndexMetadata*> replaced_all;
    const auto cleanup = [&] {
        std::error_code ec;
        for (const auto& b : built) {
            std::filesystem::remove_all(segment_dir(dataset_path, b), ec);
        }
    };
    for (const auto& name : names) {
        pb::IndexMetadata segment;
        std::vector<const pb::IndexMetadata*> replaced;
        if (!optimize_one(dataset_path, manifest, version, by_name[name], options, segment, replaced, error)) {
            if (!error.empty()) {
                cleanup();
                return false;
            }
            continue;
        }
        replaced_all.insert(replaced.begin(), replaced.end());
        built.push_back(std::move(segment));
        result.optimized.push_back(name);
    }
    if (built.empty()) {
        return true;
    }
    pb::Manifest next = manifest;
    next.indices.clear();
    for (const auto& index : manifest.indices) {
        if (replaced_all.count(&index) == 0U) {
            next.indices.push_back(index);
        }
    }
    for (auto& b : built) {
        next.indices.push_back(b);
    }
    if (!commit_next_version(dataset_path, std::move(next), result.version, error)) {
        cleanup();
        return false;
    }
    result.committed = true;
    return true;
}

}  // namespace nano_lance
