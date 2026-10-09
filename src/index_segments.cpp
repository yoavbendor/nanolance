// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Committing index segments built apart (lance/src/index.rs, commit_existing_index_segments), and
// listing a logical index's segments.

#include "nanolance/index_segments.hpp"

#include "index_build.hpp"
#include "index_files.hpp"
#include "nanolance/dataset_commit.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/scalar_index.hpp"
#include "nanolance/vector_search.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <system_error>

namespace nano_lance {
namespace {

bool fail(std::string& error, SegmentErrorKind& kind, SegmentErrorKind k, std::string message) {
    error = std::move(message);
    kind = k;
    return false;
}

std::string uuid_text(const std::array<std::uint8_t, 16>& uuid) { return pb::uuid_string(uuid); }

std::string fragment_list(const std::set<std::uint32_t>& ids) {
    std::string out = "[";
    for (const auto id : ids) {
        out += (out.size() > 1 ? ", " : "") + std::to_string(id);
    }
    return out + "]";
}

/// System indexes (fragment reuse, MemWAL) are not user indexes.
bool is_system_index(const pb::IndexMetadata& index) { return index.name.rfind("__lance_", 0) == 0; }

/// The field and everything under it.
void subtree(const pb::Manifest& manifest, std::int32_t id, std::set<std::int32_t>& out) {
    out.insert(id);
    for (const auto& f : manifest.fields) {
        if (f.parent_id == id && out.count(f.id) == 0U) {
            subtree(manifest, f.id, out);
        }
    }
}

/// A fragment's data files that hold any of `fields`, by path.
std::vector<std::string> field_files(const pb::DataFragment& fragment, const std::set<std::int32_t>& fields) {
    std::vector<std::string> out;
    for (const auto& file : fragment.files) {
        if (std::any_of(file.fields.begin(), file.fields.end(), [&](std::int32_t f) { return fields.count(f) != 0U; })) {
            out.push_back(file.path);
        }
    }
    return out;
}

/// Every file under `dir`, relative to it, with its size; Lance's committed segment files
/// (staging/ left out for an INVERTED index).
bool list_files(const std::filesystem::path& dir, bool inverted, std::vector<pb::IndexMetadata::File>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const auto rel = std::filesystem::relative(it->path(), dir, ec).generic_string();
        if (inverted && rel.rfind("staging/", 0) == 0) {
            continue;
        }
        out.push_back({rel, static_cast<std::uint64_t>(it->file_size(ec))});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return !ec;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// A vector segment's shape for the compatibility check: metric, dimension, sub-index and quantizer.
struct VectorShape {
    std::uint64_t metric = 0;  // VectorIndexDetails.metric_type
    std::size_t dim = 0;
    std::string kind;  // IVF_PQ, IVF_HNSW_SQ, ...: the sub-index and the quantizer
};

std::uint64_t details_metric(const std::vector<std::uint8_t>& b) {
    std::size_t i = 0;
    const auto varint = [&](std::uint64_t& v) {
        v = 0;
        for (int shift = 0; i < b.size() && shift < 64; shift += 7) {
            const auto c = b[i++];
            v |= static_cast<std::uint64_t>(c & 0x7FU) << shift;
            if ((c & 0x80U) == 0U) {
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
            if ((key >> 3U) == 1U) {
                return v;
            }
        } else if (wire == 2U) {
            if (!varint(v) || v > b.size() - i) {
                break;
            }
            i += static_cast<std::size_t>(v);
        } else if (wire == 1U) {
            i += 8;
        } else if (wire == 5U) {
            i += 4;
        } else {
            break;
        }
    }
    return 0;
}

std::optional<VectorShape> vector_shape(const std::filesystem::path& dataset_path, const pb::IndexMetadata& index) {
    if (!ends_with(index.details_type_url, "VectorIndexDetails")) {
        return std::nullopt;
    }
    VectorShape shape;
    shape.metric = details_metric(index.details_value);
    shape.kind = vector_index_type(index.details_value);
    index_build::VectorModel model;
    std::string ignored;
    if (index_build::load_vector_model(dataset_path / "_indices" / pb::uuid_string(index.uuid), model, ignored)) {
        shape.dim = model.dim;
    }
    return shape;
}

}  // namespace

bool dataset_commit_index_segments(const std::filesystem::path& dataset_path, const std::string& name,
                                   const std::string& column, const std::vector<std::vector<std::uint8_t>>& segments,
                                   std::uint64_t& new_version, std::string& error, SegmentErrorKind& kind) {
    error.clear();
    kind = SegmentErrorKind::None;
    const auto invalid = [&](std::string message) {
        return fail(error, kind, SegmentErrorKind::InvalidArgument, "CreateIndex: " + std::move(message));
    };
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        kind = SegmentErrorKind::Other;
        return false;
    }
    std::vector<std::string> parts;
    const auto* field = index_files::find_field(manifest, column, parts);
    if (field == nullptr) {
        return fail(error, kind, SegmentErrorKind::Index, "CreateIndex: column '" + column + "' does not exist");
    }
    if (segments.empty()) {
        return invalid("at least one index segment is required");
    }

    // The incoming set (build_index_metadata_from_segments, validate_segment_metadata).
    std::vector<pb::IndexMetadata> incoming(segments.size());
    for (std::size_t i = 0; i < segments.size(); ++i) {
        std::string why;
        if (segments[i].empty() || !pb::decode_index_message(segments[i].data(), segments[i].size(), incoming[i], why)) {
            return invalid("segment " + std::to_string(i) + " is not a valid IndexMetadata message" +
                           (why.empty() ? "" : ": " + why));
        }
    }
    std::set<std::array<std::uint8_t, 16>> seen;
    std::set<std::uint32_t> covered;
    for (const auto& s : incoming) {
        const auto id = uuid_text(s.uuid);
        if (s.dataset_version > version) {
            return invalid("segment " + id + " was built at future dataset version " +
                           std::to_string(s.dataset_version) + " (current version " + std::to_string(version) + ")");
        }
        if (s.fields.empty() || s.fields.front() != field->id) {
            return invalid("segment " + id + " was built for other fields, expected keyed field [" +
                           std::to_string(field->id) + "]");
        }
        if (s.fields != incoming.front().fields) {
            return invalid("segment " + id + " declares other fields than index '" + name +
                           "'; every segment of one index must declare the same columns");
        }
        if (!seen.insert(s.uuid).second) {
            return invalid("duplicate segment uuid " + id + " for index '" + name + "'");
        }
        if (!s.has_fragment_bitmap) {
            return invalid("segment " + id + " is missing fragment coverage");
        }
        for (const auto f : s.fragment_ids) {
            if (!covered.insert(f).second) {
                return invalid("overlapping fragment coverage in segment set for index '" + name + "'");
            }
        }
    }

    // Coverage the dataset no longer backs (prune_stale_segment_coverage): a fragment that is gone,
    // or whose files for the indexed column changed since the segment was built.
    std::set<std::int32_t> indexed;
    subtree(manifest, field->id, indexed);
    std::map<std::uint64_t, const pb::DataFragment*> current;
    for (const auto& f : manifest.fragments) {
        current[f.id] = &f;
    }
    std::map<std::uint64_t, pb::Manifest> historical;
    for (auto& s : incoming) {
        if (s.dataset_version >= version) {
            continue;
        }
        auto it = historical.find(s.dataset_version);
        if (it == historical.end()) {
            pb::Manifest old;
            std::string why;
            if (!load_manifest_version(dataset_path, s.dataset_version, old, why)) {
                return invalid("cannot validate segment coverage built at dataset version " +
                               std::to_string(s.dataset_version) + ": " + why);
            }
            it = historical.emplace(s.dataset_version, std::move(old)).first;
        }
        std::map<std::uint64_t, const pb::DataFragment*> then;
        for (const auto& f : it->second.fragments) {
            then[f.id] = &f;
        }
        std::vector<std::uint32_t> kept;
        for (const auto f : s.fragment_ids) {
            const auto old = then.find(f);
            const auto now = current.find(f);
            const bool stale = old != then.end() &&
                               (now == current.end() || field_files(*old->second, indexed) != field_files(*now->second, indexed));
            if (!stale) {
                kept.push_back(f);
            }
        }
        if (kept.size() != s.fragment_ids.size()) {
            s.fragment_ids = std::move(kept);
        }
    }

    // The new entries: named `name`, their files as they are on disk, created now.
    const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::system_clock::now().time_since_epoch())
                                                       .count());
    std::vector<pb::IndexMetadata> fresh;
    std::set<std::uint32_t> incoming_fragments;
    for (const auto& s : incoming) {
        if (s.details_type_url.empty()) {
            return invalid("segment " + uuid_text(s.uuid) + " is missing index details");
        }
        if (s.details_type_url != incoming.front().details_type_url) {
            return invalid("segment set for index '" + name + "' mixes incompatible index detail types");
        }
        std::vector<pb::IndexMetadata::File> files;
        if (!list_files(dataset_path / "_indices" / pb::uuid_string(s.uuid),
                        ends_with(s.details_type_url, "InvertedIndexDetails"), files) ||
            files.empty()) {
            return invalid("segment " + uuid_text(s.uuid) + " has no files under _indices/");
        }
        incoming_fragments.insert(s.fragment_ids.begin(), s.fragment_ids.end());
        fresh.push_back(pb::make_index_metadata(s.uuid, s.fields, name, s.dataset_version, s.fragment_ids,
                                                s.details_type_url, s.index_version, now_ms, files, s.details_value));
    }

    // The index already there under this name: what the set replaces and what it keeps.
    std::vector<const pb::IndexMetadata*> existing;
    for (const auto& index : manifest.indices) {
        if (index.name == name && !is_system_index(index)) {
            existing.push_back(&index);
        }
    }
    for (const auto* index : existing) {
        if (index->fields.empty() || index->fields.front() != field->id) {
            return fail(error, kind, SegmentErrorKind::Index,
                        "Index name '" + name + "' already exists with different fields, please specify a different name");
        }
    }
    std::set<std::uint32_t> dataset_fragments;
    for (const auto& f : manifest.fragments) {
        dataset_fragments.insert(static_cast<std::uint32_t>(f.id));
    }
    std::set<std::uint32_t> missing;
    std::set_difference(dataset_fragments.begin(), dataset_fragments.end(), incoming_fragments.begin(),
                        incoming_fragments.end(), std::inserter(missing, missing.begin()));
    std::optional<std::string> other_type;
    for (const auto* index : existing) {
        if (index->details_type_url.empty()) {
            other_type = "<unknown>";
        } else if (index->details_type_url != incoming.front().details_type_url) {
            other_type = index->details_type_url;
        }
    }
    if (other_type && !missing.empty()) {
        return invalid("cannot change index '" + name + "' from type '" + *other_type + "' to type '" +
                       incoming.front().details_type_url +
                       "' with partial fragment coverage; incoming segments are missing current fragments " +
                       fragment_list(missing));
    }
    std::set<std::array<std::uint8_t, 16>> removed;
    std::vector<const pb::IndexMetadata*> retained;
    for (const auto* index : existing) {
        if (other_type) {
            removed.insert(index->uuid);
            continue;
        }
        if (!index->has_fragment_bitmap) {
            if (!missing.empty() || incoming_fragments.size() != dataset_fragments.size()) {
                return invalid("cannot replace legacy index segment " + uuid_text(index->uuid) + " for '" + name +
                               "' with partial fragment coverage; rebuild all fragments in one commit");
            }
            removed.insert(index->uuid);
            continue;
        }
        std::set<std::uint32_t> effective;  // the fragments of this version it covers
        for (const auto f : index->fragment_ids) {
            if (dataset_fragments.count(f) != 0U) {
                effective.insert(f);
            }
        }
        if (effective.empty()) {
            removed.insert(index->uuid);
            continue;
        }
        std::set<std::uint32_t> uncovered;
        std::set_difference(effective.begin(), effective.end(), incoming_fragments.begin(), incoming_fragments.end(),
                            std::inserter(uncovered, uncovered.begin()));
        if (uncovered.size() == effective.size()) {
            if (index->fields != incoming.front().fields) {
                return invalid("incoming segments for '" + name + "' declare other fields than retained segment " +
                               uuid_text(index->uuid) +
                               "; a logical index cannot mix declarations - rebuild every segment in one commit");
            }
            retained.push_back(index);
            continue;
        }
        if (!uncovered.empty()) {
            return invalid("incoming segments for '" + name + "' would orphan fragments " + fragment_list(uncovered) +
                           " from existing segment " + uuid_text(index->uuid));
        }
        removed.insert(index->uuid);
    }

    // Vector segments that will coexist agree on metric, dimension, sub-index and quantizer.
    std::optional<VectorShape> first;
    std::vector<const pb::IndexMetadata*> coexisting = retained;
    for (const auto& s : incoming) {
        coexisting.push_back(&s);
    }
    for (const auto* index : coexisting) {
        const auto shape = vector_shape(dataset_path, *index);
        if (!shape) {
            continue;
        }
        if (!first) {
            first = shape;
            continue;
        }
        if (shape->metric != first->metric || shape->kind != first->kind ||
            (shape->dim != 0U && first->dim != 0U && shape->dim != first->dim)) {
            return invalid("vector segment " + uuid_text(index->uuid) + " of index '" + name +
                           "' is incompatible with the others: segments must share one distance metric, dimension, "
                           "sub-index type and quantizer kind");
        }
    }

    manifest.indices.erase(std::remove_if(manifest.indices.begin(), manifest.indices.end(),
                                          [&](const pb::IndexMetadata& i) { return removed.count(i.uuid) != 0U; }),
                           manifest.indices.end());
    for (auto& entry : fresh) {
        manifest.indices.push_back(std::move(entry));
    }
    if (!commit_next_version(dataset_path, std::move(manifest), new_version, error)) {
        kind = SegmentErrorKind::Other;
        return false;
    }
    return true;
}

bool dataset_index_segments(const std::filesystem::path& dataset_path, std::uint64_t version, const std::string& name,
                            std::vector<std::array<std::uint8_t, 16>>& out, std::string& error,
                            SegmentErrorKind& kind) {
    out.clear();
    error.clear();
    kind = SegmentErrorKind::None;
    pb::Manifest manifest;
    std::uint64_t latest = 0;
    if (version != 0U ? !load_manifest_version(dataset_path, version, manifest, error)
                      : !load_latest_manifest(dataset_path, manifest, latest, error)) {
        kind = SegmentErrorKind::Other;
        return false;
    }
    for (const auto& index : manifest.indices) {
        if (index.name == name && !is_system_index(index)) {
            out.push_back(index.uuid);
        }
    }
    if (out.empty()) {
        return fail(error, kind, SegmentErrorKind::NotFound, "Index not found: name='" + name + "'");
    }
    return true;
}

namespace {

/// The column path ("a.b") of field `id`.
std::string field_path(const pb::Manifest& manifest, std::int32_t id) {
    std::string path;
    for (std::int32_t at = id; at >= 0;) {
        const auto it = std::find_if(manifest.fields.begin(), manifest.fields.end(),
                                     [&](const pb::Field& f) { return f.id == at; });
        if (it == manifest.fields.end()) {
            return "";
        }
        path = path.empty() ? it->name : it->name + "." + path;
        at = it->parent_id;
    }
    return path;
}

/// The last part of a details type URL: "BTreeIndexDetails", "VectorIndexDetails", ...
std::string details_kind(const std::string& url) {
    const auto dot = url.rfind('.');
    return dot == std::string::npos ? url : url.substr(dot + 1U);
}

std::string type_details_kind(const std::string& type) {
    if (type == "BTREE") return "BTreeIndexDetails";
    if (type == "BITMAP") return "BitmapIndexDetails";
    if (type == "LABEL_LIST") return "LabelListIndexDetails";
    if (type == "INVERTED") return "InvertedIndexDetails";
    return "VectorIndexDetails";
}

/// The name a build gets (create.rs): the given one, else <column>_idx, then _2, _3, ... past
/// indexes of that name on another column or of another kind; refused when the version holds it.
bool resolve_name(const pb::Manifest& manifest, const pb::Field& field, const std::string& column,
                  const std::string& kind, std::string& name, std::string& error, SegmentErrorKind& k) {
    const auto clashes = [&](const std::string& n) {
        return std::any_of(manifest.indices.begin(), manifest.indices.end(), [&](const auto& i) {
            return i.name == n &&
                   (i.fields.empty() || i.fields.front() != field.id || details_kind(i.details_type_url) != kind);
        });
    };
    if (name.empty()) {
        const std::string base = column + "_idx";
        name = base;
        for (int n = 2; clashes(name); ++n) {
            name = base + "_" + std::to_string(n);
        }
    }
    for (const auto& i : manifest.indices) {
        if (i.name != name) {
            continue;
        }
        if (i.fields.empty() || i.fields.front() != field.id) {
            return fail(error, k, SegmentErrorKind::Index,
                        "Index name '" + name + "' already exists with different fields, please specify a different name");
        }
        return fail(error, k, SegmentErrorKind::Index,
                    "Index name '" + name + "' already exists, please specify a different name or use replace=True");
    }
    return true;
}

bool load_version(const std::filesystem::path& dataset_path, std::uint64_t version, pb::Manifest& manifest,
                  std::uint64_t& at, std::string& error) {
    if (version == 0U) {
        return load_latest_manifest(dataset_path, manifest, at, error);
    }
    at = version;
    return load_manifest_version(dataset_path, version, manifest, error);
}

/// Build over `fragments` of `manifest` (version `version`); the entry in `entry`.
bool build_segment(const std::filesystem::path& dataset_path, const std::string& column, const std::string& type,
                   const std::string& name, const pb::Manifest& manifest, std::uint64_t version,
                   const std::vector<std::uint64_t>* fragments, const std::array<std::uint8_t, 16>* uuid,
                   const InvertedIndexOptions& inverted, const VectorIndexOptions& vector,
                   const index_build::VectorModel* model, const std::vector<std::uint8_t>* details,
                   pb::IndexMetadata& entry, std::string& error) {
    index_build::SegmentTarget target;
    target.manifest = &manifest;
    target.version = version;
    target.fragments = fragments;
    target.out = &entry;
    target.uuid = uuid;
    target.model = model;
    target.details = details;
    if (type == "INVERTED") {
        InvertedIndexOptions o = inverted;
        o.name = name;
        return index_build::build_inverted_segment(dataset_path, column, o, target, error);
    }
    ScalarIndexType scalar{};
    if (parse_scalar_index_type(type, scalar)) {
        ScalarIndexOptions o;
        o.name = name;
        return index_build::build_scalar_segment(dataset_path, column, scalar, o, target, error);
    }
    VectorIndexOptions o = vector;
    o.type = type;
    o.name = name;
    return index_build::build_vector_segment(dataset_path, column, o, target, error);
}

}  // namespace

bool decode_index_segment(const std::vector<std::uint8_t>& message, IndexSegmentInfo& out, std::string& error) {
    pb::IndexMetadata m;
    if (message.empty() || !pb::decode_index_message(message.data(), message.size(), m, error)) {
        error = "invalid IndexMetadata protobuf" + (error.empty() ? "" : ": " + error);
        return false;
    }
    out = IndexSegmentInfo{};
    out.uuid = m.uuid;
    out.name = m.name;
    out.fields = m.fields;
    out.dataset_version = m.dataset_version;
    out.has_fragment_ids = m.has_fragment_bitmap;
    out.fragment_ids = m.fragment_ids;
    out.index_version = m.index_version;
    out.created_at = m.created_at;
    out.details_type_url = m.details_type_url;
    out.details_value = m.details_value;
    for (const auto& f : m.files) {
        out.files.emplace_back(f.path, f.size);
    }
    return true;
}

std::vector<std::uint8_t> encode_index_segment(const IndexSegmentInfo& s) {
    std::vector<pb::IndexMetadata::File> files;
    for (const auto& [path, size] : s.files) {
        files.push_back({path, size});
    }
    auto m = pb::make_index_metadata(s.uuid, s.fields, s.name, s.dataset_version, s.fragment_ids, s.details_type_url,
                                     s.index_version, s.created_at, files, s.details_value);
    return pb::encode_index_message(m);
}

bool dataset_build_index_segment(const std::filesystem::path& dataset_path, const std::string& column,
                                 const IndexSegmentBuild& build, std::vector<std::uint8_t>& segment,
                                 std::string& error, SegmentErrorKind& kind) {
    error.clear();
    kind = SegmentErrorKind::None;
    const auto invalid = [&](std::string message) {
        return fail(error, kind, SegmentErrorKind::InvalidArgument, std::move(message));
    };
    static const std::set<std::string> types = {"BTREE",    "BITMAP", "LABEL_LIST",  "INVERTED",
                                                "IVF_FLAT", "IVF_PQ", "IVF_HNSW_SQ"};
    if (types.count(build.type) == 0U) {
        return fail(error, kind, SegmentErrorKind::Other,
                    "index type '" + build.type + "' is not supported for an index segment");
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_version(dataset_path, build.version, manifest, version, error)) {
        kind = SegmentErrorKind::Other;
        return false;
    }
    std::vector<std::string> parts;
    const auto* field = index_files::find_field(manifest, column, parts);
    if (field == nullptr) {
        return fail(error, kind, SegmentErrorKind::Index, "column '" + column + "' does not exist");
    }
    if (build.fragments) {
        std::set<std::uint64_t> existing;
        for (const auto& f : manifest.fragments) {
            existing.insert(f.id);
        }
        std::set<std::uint64_t> seen;
        for (std::size_t i = 0; i < build.fragments->size(); ++i) {
            const auto id = (*build.fragments)[i];
            if (!seen.insert(id).second) {
                return invalid("fragment_ids[" + std::to_string(i) + "] is duplicate fragment id " + std::to_string(id));
            }
            if (existing.count(id) == 0U) {
                return invalid("fragment_ids[" + std::to_string(i) + "]=" + std::to_string(id) +
                               " does not exist in dataset version " + std::to_string(version));
            }
        }
    }
    std::string name = build.name;
    if (!resolve_name(manifest, *field, column, type_details_kind(build.type), name, error, kind)) {
        return false;
    }
    if (build.uuid && build.fragments && !build.fragments->empty() &&
        (build.type == "BTREE" || build.type == "LABEL_LIST")) {
        return invalid(std::string("index_uuid is no longer accepted for ") +
                       (build.type == "BTREE" ? "BTree" : "LabelList") +
                       " distributed index builds; segment UUIDs are generated by Lance and returned in the index "
                       "metadata.");
    }
    std::optional<index_build::VectorModel> model;
    if (!build.ivf_centroids.empty()) {
        const auto& v = build.vector;
        const std::size_t partitions = v.num_partitions.value_or(0U);
        if (partitions == 0U || build.ivf_centroids.size() % partitions != 0U) {
            return invalid("ivf_centroids length does not match num_partitions " + std::to_string(partitions));
        }
        auto& m = model.emplace();
        m.type = build.type;
        m.metric = v.metric;
        m.dim = build.ivf_centroids.size() / partitions;
        m.partitions = partitions;
        m.centroids = build.ivf_centroids;
        m.nbits = v.num_bits;
        m.m = build.type == "IVF_PQ" ? v.num_sub_vectors : 0U;
        m.has_codebook = !build.pq_codebook.empty();
        m.codebook = build.pq_codebook;
        m.has_sq = false;
        m.hnsw_m = v.hnsw_m;
        m.hnsw_ef_construction = v.hnsw_ef_construction;
        m.hnsw_max_level = v.hnsw_max_level;
        if (m.has_codebook && (m.m == 0U || m.codebook.size() != (std::size_t{1} << m.nbits) * m.dim)) {
            return invalid("pq_codebook must hold 2^num_bits (" + std::to_string(std::size_t{1} << m.nbits) +
                           ") centroids of dimension " + std::to_string(m.dim) + ", got " +
                           std::to_string(m.codebook.size()) + " values");
        }
    } else if (!build.pq_codebook.empty()) {
        return invalid("pq_codebook requires ivf_centroids");
    }
    pb::IndexMetadata entry;
    if (!build_segment(dataset_path, column, build.type, name, manifest, version,
                       build.fragments ? &*build.fragments : nullptr, build.uuid ? &*build.uuid : nullptr,
                       build.inverted, build.vector, model ? &*model : nullptr, nullptr, entry, error)) {
        kind = SegmentErrorKind::Other;
        return false;
    }
    segment = pb::encode_index_message(entry);
    return true;
}

bool dataset_merge_index_segments(const std::filesystem::path& dataset_path,
                                  const std::vector<std::vector<std::uint8_t>>& segments,
                                  std::vector<std::uint8_t>& merged, std::string& error, SegmentErrorKind& kind) {
    error.clear();
    kind = SegmentErrorKind::None;
    const auto invalid = [&](std::string message) {
        return fail(error, kind, SegmentErrorKind::InvalidArgument, std::move(message));
    };
    if (segments.empty()) {
        return invalid("CreateIndex: at least one index segment is required");
    }
    pb::Manifest latest;
    std::uint64_t current = 0;
    if (!load_latest_manifest(dataset_path, latest, current, error)) {
        kind = SegmentErrorKind::Other;
        return false;
    }
    // validate_segment_metadata("uncommitted", ...), then merge_existing_index_segments' own checks.
    std::vector<pb::IndexMetadata> in(segments.size());
    std::set<std::array<std::uint8_t, 16>> seen;
    std::set<std::uint32_t> covered;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        std::string why;
        if (segments[i].empty() || !pb::decode_index_message(segments[i].data(), segments[i].size(), in[i], why)) {
            return invalid("segment " + std::to_string(i) + " is not a valid IndexMetadata message" +
                           (why.empty() ? "" : ": " + why));
        }
        const auto& s = in[i];
        if (!seen.insert(s.uuid).second) {
            return invalid("CreateIndex: duplicate segment uuid " + uuid_text(s.uuid) + " for index 'uncommitted'");
        }
        if (!s.has_fragment_bitmap) {
            return invalid("CreateIndex: segment " + uuid_text(s.uuid) + " is missing fragment coverage");
        }
        for (const auto f : s.fragment_ids) {
            if (!covered.insert(f).second) {
                return invalid("CreateIndex: overlapping fragment coverage in segment set for index 'uncommitted'");
            }
        }
    }
    std::uint64_t source_version = UINT64_MAX;
    for (const auto& s : in) {
        if (s.dataset_version > current) {
            return invalid("merge_existing_index_segments: segment " + uuid_text(s.uuid) +
                           " was built at future dataset version " + std::to_string(s.dataset_version) +
                           " (current version " + std::to_string(current) + ")");
        }
        source_version = std::min(source_version, s.dataset_version);
    }
    if (in.front().fields.empty()) {
        return invalid("CreateIndex: segment " + uuid_text(in.front().uuid) + " is missing field ids");
    }
    const auto field_id = in.front().fields.front();
    for (const auto& s : in) {
        if (s.fields.empty() || s.fields.front() != field_id) {
            return invalid("merge_existing_index_segments: segment " + uuid_text(s.uuid) +
                           " was built for other fields, expected keyed field [" + std::to_string(field_id) + "]");
        }
        if (s.fields != in.front().fields) {
            return invalid("merge_existing_index_segments requires segments with identical fields");
        }
        if (s.details_type_url.empty() || s.details_type_url != in.front().details_type_url) {
            return invalid(
                "merge_existing_index_segments requires all segments to have the same supported index type");
        }
    }
    const std::string kind_name = details_kind(in.front().details_type_url);
    const bool is_vector = kind_name == "VectorIndexDetails";
    if (is_vector && in.size() == 1U) {
        merged = segments.front();
        return true;
    }
    pb::Manifest manifest;
    if (!load_manifest_version(dataset_path, source_version, manifest, error)) {
        kind = SegmentErrorKind::Other;
        return false;
    }
    const std::string column = field_path(manifest, field_id);
    if (column.empty()) {
        return fail(error, kind, SegmentErrorKind::Index,
                    "merge_existing_index_segments: field " + std::to_string(field_id) + " does not exist");
    }
    std::vector<std::uint64_t> fragments(covered.begin(), covered.end());
    const auto dir_of = [&](const pb::IndexMetadata& s) { return dataset_path / "_indices" / pb::uuid_string(s.uuid); };

    std::string type;
    InvertedIndexOptions inverted;
    VectorIndexOptions vector;
    std::optional<index_build::VectorModel> model;
    if (kind_name == "BTreeIndexDetails") {
        type = "BTREE";
    } else if (kind_name == "BitmapIndexDetails") {
        type = "BITMAP";
    } else if (kind_name == "LabelListIndexDetails") {
        type = "LABEL_LIST";
    } else if (kind_name == "InvertedIndexDetails") {
        type = "INVERTED";
        if (!index_build::load_inverted_params(dir_of(in.front()), inverted.params, error)) {
            kind = SegmentErrorKind::Other;
            return false;
        }
    } else if (is_vector) {
        // The segments must share one model (lance-index distributed/index_merger.rs): the metric,
        // the IVF centroids and the PQ codebook (equal within 1e-5); an SQ segment takes the first's
        // bounds.
        std::vector<index_build::VectorModel> models(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (!index_build::load_vector_model(dir_of(in[i]), models[i], error)) {
                kind = SegmentErrorKind::Other;
                return false;
            }
        }
        const auto close = [](const std::vector<float>& a, const std::vector<float>& b) {
            if (a.size() != b.size()) {
                return false;
            }
            for (std::size_t j = 0; j < a.size(); ++j) {
                if (!(std::abs(a[j] - b[j]) <= 1e-5F)) {
                    return false;
                }
            }
            return true;
        };
        const auto& first = models.front();
        for (const auto& m : models) {
            const auto mismatch = [&](const std::string& what) {
                return fail(error, kind, SegmentErrorKind::Index, what);
            };
            if (m.metric != first.metric) {
                return mismatch("Distance type mismatch across shards");
            }
            if (m.type != first.type) {
                return mismatch("merge_existing_index_segments: vector index segments do not share a storage format");
            }
            if (m.partitions != first.partitions) {
                return mismatch("IVF partition count mismatch across shards");
            }
            if (m.dim != first.dim) {
                return mismatch("Dimension mismatch across shards");
            }
            if (!close(m.centroids, first.centroids)) {
                return mismatch("IVF centroids mismatch across shards");
            }
            if (m.type == "IVF_PQ") {
                if (m.m != first.m || m.nbits != first.nbits) {
                    return mismatch("Distributed PQ merge: structural mismatch across shards; first(dim=" +
                                    std::to_string(first.dim) + ", m=" + std::to_string(first.m) +
                                    ", nbits=" + std::to_string(first.nbits) + "), current(dim=" +
                                    std::to_string(m.dim) + ", m=" + std::to_string(m.m) +
                                    ", nbits=" + std::to_string(m.nbits) + ")");
                }
                if (!close(m.codebook, first.codebook)) {
                    return mismatch("PQ codebook content mismatch across shards");
                }
            }
        }
        model = std::move(models.front());
        type = model->type;
    } else {
        return invalid("merge_existing_index_segments requires all segments to have the same supported index type");
    }
    pb::IndexMetadata entry;
    if (!build_segment(dataset_path, column, type, in.front().name, manifest, source_version, &fragments, nullptr,
                       inverted, vector, model ? &*model : nullptr, is_vector ? &in.front().details_value : nullptr,
                       entry, error)) {
        kind = SegmentErrorKind::Other;
        return false;
    }
    merged = pb::encode_index_message(entry);
    return true;
}

bool dataset_train_vector_model(const std::filesystem::path& dataset_path, std::uint64_t version,
                                const std::string& column, const VectorIndexOptions& options,
                                const std::vector<std::uint64_t>* fragments, const std::vector<float>* ivf_centroids,
                                TrainedVectorModel& out, std::string& error) {
    pb::Manifest manifest;
    std::uint64_t at = 0;
    if (!load_version(dataset_path, version, manifest, at, error)) {
        return false;
    }
    std::optional<index_build::VectorModel> model;
    if (ivf_centroids != nullptr) {
        const std::size_t partitions = options.num_partitions.value_or(0U);
        if (partitions == 0U || ivf_centroids->empty() || ivf_centroids->size() % partitions != 0U) {
            error = "ivf_centroids length does not match num_partitions " + std::to_string(partitions);
            return false;
        }
        auto& m = model.emplace();
        m.type = options.type;
        m.metric = options.metric;
        m.dim = ivf_centroids->size() / partitions;
        m.partitions = partitions;
        m.centroids = *ivf_centroids;
        m.nbits = options.num_bits;
        m.m = options.num_sub_vectors;
        m.has_codebook = false;
        m.has_sq = false;
    }
    index_build::VectorModel trained;
    index_build::SegmentTarget target;
    target.manifest = &manifest;
    target.version = at;
    target.fragments = fragments;
    target.model = model ? &*model : nullptr;
    target.trained = &trained;
    if (!index_build::build_vector_segment(dataset_path, column, options, target, error)) {
        return false;
    }
    out.dim = trained.dim;
    out.partitions = trained.partitions;
    out.centroids = std::move(trained.centroids);
    out.codebook = std::move(trained.codebook);
    return true;
}

bool infer_index_segment_details(const std::filesystem::path& dataset_path, const std::string& column,
                                 const std::array<std::uint8_t, 16>& uuid, std::string& type_url,
                                 std::vector<std::uint8_t>& value, std::string& error) {
    const auto dir = dataset_path / "_indices" / pb::uuid_string(uuid);
    const auto has = [&](const char* name) {
        std::error_code ec;
        return std::filesystem::is_regular_file(dir / name, ec);
    };
    value.clear();
    if (has("bitmap_page_lookup.lance")) {
        pb::Manifest manifest;
        std::uint64_t version = 0;
        if (!load_latest_manifest(dataset_path, manifest, version, error)) {
            return false;
        }
        std::vector<std::string> parts;
        const auto* field = index_files::find_field(manifest, column, parts);
        const bool list = field != nullptr && (field->logical_type.rfind("list", 0) == 0 ||
                                               field->logical_type.rfind("large_list", 0) == 0);
        type_url = list ? "/lance.table.LabelListIndexDetails" : "/lance.table.BitmapIndexDetails";
        return true;
    }
    if (has("page_lookup.lance")) {
        type_url = "/lance.table.BTreeIndexDetails";
        return true;
    }
    if (has("metadata.lance")) {
        fts::AnalyzerParams params;
        if (!index_build::load_inverted_params(dir, params, error)) {
            return false;
        }
        type_url = "/lance.table.InvertedIndexDetails";
        value = index_build::inverted_details(params);
        return true;
    }
    if (has("index.idx")) {
        type_url = "/lance.table.VectorIndexDetails";
        return true;
    }
    error = "segment " + pb::uuid_string(uuid) + " has no index files under _indices/ to infer its type from";
    return false;
}

}  // namespace nano_lance
