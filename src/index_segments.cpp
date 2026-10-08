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
#include "nanolance/vector_search.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <chrono>
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

}  // namespace nano_lance
