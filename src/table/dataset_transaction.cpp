// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Hand-built transactions (dataset_transaction.hpp): one lance.table.Transaction operation, decoded,
// checked and applied to a version's manifest as Lance applies it.

#include "nanolance/dataset_transaction.hpp"

#include "nanolance/base_paths.hpp"
#include "nanolance/dataset_commit.hpp"
#include "nanolance/deletion_vector.hpp"
#include "nanolance/index_maintenance.hpp"
#include "nanolance/mem_wal.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/overlays.hpp"
#include "nanolance/row_ids.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <random>
#include <set>
#include <unordered_map>

#include "table/dataset_operation.hpp"
#include "table/dataset_transaction_ops.hpp"

namespace nano_lance {
namespace {

using Bytes = OpBytes;
constexpr std::int32_t kTombstone = -2;

// ── Lance's checks (transaction/validate.rs) ────────────────────────────────────────────────────

std::string deletion_path(const pb::DataFragment& f) {
    return "_deletions/" + std::to_string(f.id) + "-" + std::to_string(f.deletion_file.read_version) + "-" +
           std::to_string(f.deletion_file.id) + (f.deletion_file.file_type == 1U ? ".bin" : ".arrow");
}

bool files_have_fields(const std::vector<pb::DataFragment>& fragments, std::string& error) {
    for (const auto& fragment : fragments) {
        for (const auto& file : fragment.files) {
            if (file.fields.empty()) {
                error = "Invalid user input: Datafile " + file.path + " does not contain any fields";
                return false;
            }
        }
    }
    return true;
}

bool overwrite_fragments_valid(const std::vector<pb::DataFragment>& fragments, std::string& error) {
    for (const auto& fragment : fragments) {
        if (fragment.deletion_file.present) {
            error = "Invalid user input: Overwrite fragments must be newly written, but fragment " +
                    std::to_string(fragment.id) + " carries deletion file " + deletion_path(fragment) +
                    ". Use Delete to commit deletions against existing fragments, or Merge to change their schema.";
            return false;
        }
    }
    return true;
}

std::string id_list(const std::vector<std::uint64_t>& ids) {
    std::string out = "[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        out += (i ? ", " : "") + std::to_string(ids[i]);
    }
    return out + "]";
}

/// "a.b": a field's path in `fields` by ids.
std::string field_path(const std::vector<pb::Field>& fields, std::int32_t id) {
    std::string out;
    for (std::int32_t at = id; at != -1;) {
        const auto it = std::find_if(fields.begin(), fields.end(), [&](const pb::Field& f) { return f.id == at; });
        if (it == fields.end()) {
            break;
        }
        out = out.empty() ? it->name : it->name + "." + out;
        at = it->parent_id;
    }
    return out;
}

const pb::Field* field_by_id(const std::vector<pb::Field>& fields, std::int32_t id) {
    const auto it = std::find_if(fields.begin(), fields.end(), [&](const pb::Field& f) { return f.id == id; });
    return it == fields.end() ? nullptr : &*it;
}

bool merge_valid(const pb::Manifest& manifest, const Operation& op, std::string& error) {
    const auto& original = manifest.fragments;
    if (op.fragments.size() < original.size()) {
        error = "Invalid user input: Merge operation reduced fragment count from " + std::to_string(original.size()) +
                " to " + std::to_string(op.fragments.size()) +
                ". Merge operations should only add columns, not reduce fragments.";
        return false;
    }
    std::unordered_map<std::uint64_t, const pb::DataFragment*> by_id;
    for (const auto& f : op.fragments) {
        by_id[f.id] = &f;
    }
    std::vector<std::uint64_t> missing;
    for (const auto& f : original) {
        const auto it = by_id.find(f.id);
        if (it == by_id.end()) {
            missing.push_back(f.id);
        } else if (it->second->physical_rows != f.physical_rows) {
            error = "Invalid user input: Merge operation changed row count for fragment " + std::to_string(f.id) +
                    ". Original: Some(" + std::to_string(f.physical_rows) + "), New: Some(" +
                    std::to_string(it->second->physical_rows) +
                    "). Merge operations should preserve fragment row counts and only add new columns.";
            return false;
        }
    }
    if (!missing.empty()) {
        std::vector<std::uint64_t> expected, got;
        for (const auto& f : original) expected.push_back(f.id);
        for (const auto& f : op.fragments) got.push_back(f.id);
        error = "Invalid user input: Merge operation is missing original fragments: " + id_list(missing) +
                ". Merge operations should preserve all original fragments and only add new columns. "
                "Expected fragments: " + id_list(expected) + ", but got: " + id_list(got);
        return false;
    }
    std::int32_t max_field_id = -1;
    for (const auto& f : manifest.fields) {
        max_field_id = std::max(max_field_id, f.id);
    }
    for (const auto& field : op.schema) {
        const auto* prior = field_by_id(manifest.fields, field.id);
        if (prior == nullptr) {
            if (field.id <= max_field_id) {
                error = "Invalid user input: Merge operation assigns id " + std::to_string(field.id) +
                        " to new field \"" + field_path(op.schema, field.id) + "\", but ids up to " +
                        std::to_string(max_field_id) +
                        " are already used by current or dropped fields. New fields must use ids of at least " +
                        std::to_string(max_field_id + 1) + ".";
                return false;
            }
            continue;
        }
        const auto before = field_path(manifest.fields, field.id);
        const auto after = field_path(op.schema, field.id);
        if (before != after) {
            error = "Invalid user input: Merge operation remaps field id " + std::to_string(field.id) + " from \"" +
                    before + "\" to \"" + after +
                    "\". Merge must preserve the dataset's field ids: derive the new schema from the dataset's "
                    "current schema instead of renumbering fields.";
            return false;
        }
    }
    return true;
}

// ── Lance's index bookkeeping for these operations (transaction/index_maintenance.rs) ────────────

void prune_updated_fields(std::vector<pb::IndexMetadata>& indices, const std::vector<std::uint64_t>& fragments,
                          const std::vector<std::uint64_t>& fields_modified) {
    if (fields_modified.empty() || fragments.empty()) {
        return;
    }
    const std::set<std::uint64_t> modified(fields_modified.begin(), fields_modified.end());
    for (auto& index : indices) {
        const bool touched = std::any_of(index.fields.begin(), index.fields.end(), [&](std::int32_t id) {
            return id >= 0 && modified.count(static_cast<std::uint64_t>(id)) != 0U;
        });
        if (!touched || !index.has_fragment_bitmap) {
            continue;
        }
        const auto before = index.fragment_ids.size();
        index.fragment_ids.erase(std::remove_if(index.fragment_ids.begin(), index.fragment_ids.end(),
                                                [&](std::uint32_t id) {
                                                    return std::find(fragments.begin(), fragments.end(), id) !=
                                                           fragments.end();
                                                }),
                                 index.fragment_ids.end());
        index.fragment_bitmap_changed = index.fragment_bitmap_changed || index.fragment_ids.size() != before;
    }
}

/// Each field's data file in a fragment (tombstones left out).
std::map<std::int32_t, std::string> field_files(const pb::DataFragment& f) {
    std::map<std::int32_t, std::string> out;
    for (const auto& file : f.files) {
        for (const auto id : file.fields) {
            if (id >= 0) {
                out[id] = file.path;
            }
        }
    }
    return out;
}

/// A Merge that rewrote a field's data in place: the fragment leaves the coverage of indices on it.
void prune_merge_rewritten(std::vector<pb::IndexMetadata>& indices, const std::vector<pb::DataFragment>& before,
                           const std::vector<pb::DataFragment>& after) {
    std::unordered_map<std::uint64_t, const pb::DataFragment*> prev;
    for (const auto& f : before) {
        prev[f.id] = &f;
    }
    for (const auto& f : after) {
        const auto it = prev.find(f.id);
        if (it == prev.end()) {
            continue;
        }
        const auto old_paths = field_files(*it->second);
        const auto new_paths = field_files(f);
        std::vector<std::uint64_t> changed;
        for (const auto& [id, path] : old_paths) {
            const auto now = new_paths.find(id);
            if (now != new_paths.end() && now->second != path) {
                changed.push_back(static_cast<std::uint64_t>(id));
            }
        }
        prune_updated_fields(indices, {f.id}, changed);
    }
}

bool recalculate_bitmap(std::vector<std::uint32_t>& ids, const std::vector<RewriteGroup>& groups, std::string& error) {
    std::set<std::uint32_t> out(ids.begin(), ids.end());
    const std::set<std::uint32_t> old(ids.begin(), ids.end());
    for (const auto& group : groups) {
        std::size_t covered = 0;
        for (const auto& f : group.old_fragments) {
            covered += old.count(static_cast<std::uint32_t>(f.id));
        }
        if (covered == 0U) {
            continue;
        }
        if (covered != group.old_fragments.size()) {
            error = "Invalid user input: The compaction plan included a rewrite group that was a split of indexed "
                    "and non-indexed data";
            return false;
        }
        for (const auto& f : group.old_fragments) out.erase(static_cast<std::uint32_t>(f.id));
        for (const auto& f : group.new_fragments) out.insert(static_cast<std::uint32_t>(f.id));
    }
    ids.assign(out.begin(), out.end());
    return true;
}

// ── building the next manifest (transaction/manifest_build.rs) ──────────────────────────────────

std::uint64_t next_fragment_id(const pb::Manifest& m) {
    std::uint64_t next = m.has_max_fragment_id ? static_cast<std::uint64_t>(m.max_fragment_id) + 1U : 0U;
    for (const auto& f : m.fragments) {
        next = std::max(next, f.id + 1U);
    }
    return std::max(next, first_fragment_id_after_indices(m.indices));
}

/// Lance's fragments_with_ids: a fragment without an id (0) takes the next one.
void assign_ids(std::vector<pb::DataFragment>& fragments, std::uint64_t& next) {
    for (auto& f : fragments) {
        if (f.id == 0U) {
            f.id = next++;
        }
    }
}

/// The storage version a new dataset's data files imply ("2.2" when there are none).
std::string storage_version_of(const std::vector<pb::DataFragment>& fragments) {
    for (const auto& f : fragments) {
        for (const auto& file : f.files) {
            if (file.file_major_version == 0U && file.file_minor_version == 0U) {
                return "0.1";
            }
            if (file.file_major_version == 0U && file.file_minor_version == 3U) {
                return "2.0";  // 2.0 files carry the footer version 0.3
            }
            return std::to_string(file.file_major_version) + "." + std::to_string(file.file_minor_version);
        }
    }
    return "2.2";
}

bool data_replacement(pb::Manifest& next, const Operation& op, std::string& error) {
    std::set<std::vector<std::int32_t>> distinct;
    for (const auto& [id, file] : op.replacements) {
        distinct.insert(file.fields);
    }
    if (distinct.size() > 1U) {
        std::string info;
        for (std::size_t i = 0; i < op.replacements.size(); ++i) {
            info += "File " + std::to_string(i) + ": [";
            const auto& fields = op.replacements[i].second.fields;
            for (std::size_t k = 0; k < fields.size(); ++k) {
                info += (k ? ", " : "") + std::to_string(fields[k]);
            }
            info += "]\n";
        }
        error = "Invalid user input: All new data files must have the same fields, but found different fields:\n" + info;
        return false;
    }
    std::vector<std::uint64_t> changed;
    std::vector<std::uint64_t> replaced_fields;
    if (!op.replacements.empty()) {
        for (const auto id : op.replacements.front().second.fields) {
            if (id >= 0) {
                replaced_fields.push_back(static_cast<std::uint64_t>(id));
            }
        }
        // A parent field's children are replaced with it.
        for (std::size_t k = 0; k < replaced_fields.size(); ++k) {
            for (const auto& f : next.fields) {
                if (f.parent_id >= 0 && static_cast<std::uint64_t>(f.parent_id) == replaced_fields[k] &&
                    std::find(replaced_fields.begin(), replaced_fields.end(), static_cast<std::uint64_t>(f.id)) ==
                        replaced_fields.end()) {
                    replaced_fields.push_back(static_cast<std::uint64_t>(f.id));
                }
            }
        }
    }
    const std::set<std::int32_t> replacement_ids(replaced_fields.begin(), replaced_fields.end());
    std::set<std::int32_t> live;
    for (const auto& f : next.fields) {
        live.insert(f.id);
    }
    for (const auto& [frag_id, new_file] : op.replacements) {
        auto it = std::find_if(next.fragments.begin(), next.fragments.end(),
                               [&](const pb::DataFragment& f) { return f.id == frag_id; });
        if (it == next.fragments.end()) {
            error = "Invalid user input: Fragment being replaced not found in existing fragments";
            return false;
        }
        const Bytes before = pb::encode_data_fragment(*it);
        std::set<std::int32_t> covered;
        bool in_place = false;
        for (auto& file : it->files) {
            if (file.fields == new_file.fields && file.file_major_version == new_file.file_major_version &&
                file.file_minor_version == new_file.file_minor_version) {
                file.path = new_file.path;
                file.file_size_bytes = new_file.file_size_bytes;
                in_place = true;
            }
            covered.insert(file.fields.begin(), file.fields.end());
        }
        const bool disjoint = std::none_of(replacement_ids.begin(), replacement_ids.end(),
                                           [&](std::int32_t id) { return covered.count(id) != 0U; });
        const bool subset = std::all_of(replacement_ids.begin(), replacement_ids.end(),
                                        [&](std::int32_t id) { return covered.count(id) != 0U; });
        if (disjoint) {
            it->files.push_back(new_file);  // an all-null column given its data
        } else if (!in_place && subset) {
            // Tombstone the replaced fields where they live and add the file that answers for them.
            for (auto& file : it->files) {
                for (auto& id : file.fields) {
                    if (replacement_ids.count(id) != 0U) {
                        id = kTombstone;
                    }
                }
            }
            it->files.erase(std::remove_if(it->files.begin(), it->files.end(),
                                           [&](const pb::DataFile& file) {
                                               return std::none_of(file.fields.begin(), file.fields.end(),
                                                                   [&](std::int32_t id) { return live.count(id); });
                                           }),
                            it->files.end());
            it->files.push_back(new_file);
        }
        if (pb::encode_data_fragment(*it) == before) {
            error = "Invalid user input: Expected to modify the fragment but no changes were made. This means the "
                    "new data files does not align with any exiting datafiles. Please check if the schema of the new "
                    "data files matches the schema of the old data files including the file major and minor versions";
            return false;
        }
        changed.push_back(frag_id);
    }
    prune_updated_fields(next.indices, changed, replaced_fields);
    return true;
}

/// Stable row ids: a fragment that arrives without row ids takes the next ones from the manifest's
/// high-water mark, and one without version metadata is created and last updated at `version`
/// (Lance's assign_row_ids / build_version_meta). Fragments that carry their own are kept as given.
bool stamp_missing_row_meta(std::vector<pb::DataFragment>& fragments, std::uint64_t& next_row_id,
                            std::uint64_t version, std::string& error) {
    for (auto& fragment : fragments) {
        FragmentRowMeta meta;
        if (!read_fragment_row_meta(fragment, meta, error)) {
            return false;
        }
        bool changed = false;
        if (!meta.has_row_ids && !meta.external) {
            meta.has_row_ids = true;
            meta.row_ids = RowIdSequence::range(next_row_id, fragment.physical_rows).encode();
            next_row_id += fragment.physical_rows;
            changed = true;
        } else if (meta.has_row_ids) {
            RowIdSequence ids;
            std::uint64_t top = 0;
            if (!RowIdSequence::decode(meta.row_ids.data(), meta.row_ids.size(), ids, error)) {
                return false;
            }
            if (ids.max_id(top)) {
                next_row_id = std::max(next_row_id, top + 1U);
            }
        }
        if (fragment.physical_rows != 0U && !meta.external) {
            if (!meta.has_created) {
                meta.has_created = true;
                meta.created = RowVersionSequence::uniform(fragment.physical_rows, version).encode();
                changed = true;
            }
            if (!meta.has_last_updated) {
                meta.has_last_updated = true;
                meta.last_updated = RowVersionSequence::uniform(fragment.physical_rows, version).encode();
                changed = true;
            }
        }
        if (changed) {
            write_fragment_row_meta(fragment, meta);
        }
    }
    return true;
}

/// Stable row ids, an Update in RewriteColumns mode: the rows its updated_fragment_offsets name were last
/// updated at `version` (Lance's refresh of the row version metadata for the matched rows only).
bool stamp_updated_rows(std::vector<pb::DataFragment>& fragments, const Operation& op, std::uint64_t version,
                        std::string& error) {
    for (const auto& [fragment_id, bitmap] : op.updated_fragment_offsets) {
        auto fragment = std::find_if(fragments.begin(), fragments.end(),
                                     [&](const pb::DataFragment& f) { return f.id == fragment_id; });
        if (fragment == fragments.end()) {
            continue;
        }
        std::vector<std::uint32_t> offsets;
        FragmentRowMeta meta;
        if (!parse_roaring_bitmap(bitmap, offsets, error, fragment->physical_rows) ||
            !read_fragment_row_meta(*fragment, meta, error)) {
            return false;
        }
        if (meta.external || offsets.empty()) {
            continue;
        }
        std::vector<std::uint64_t> versions(fragment->physical_rows, 1U);
        if (meta.has_last_updated) {
            RowVersionSequence seq;
            if (!RowVersionSequence::decode(meta.last_updated.data(), meta.last_updated.size(), seq, error)) {
                return false;
            }
            versions = seq.to_vector();
            versions.resize(fragment->physical_rows, 1U);
        }
        for (const auto offset : offsets) {
            if (offset < versions.size()) {
                versions[offset] = version;
            }
        }
        meta.has_last_updated = true;
        meta.last_updated = RowVersionSequence::from_values(versions).encode();
        write_fragment_row_meta(*fragment, meta);
    }
    return true;
}

bool apply_operation(const pb::Manifest* base, const Operation& op, pb::Manifest& next, std::string& error,
                     bool enable_stable_row_ids = false, std::uint64_t new_version = 0) {
    if (base == nullptr && op.kind != kOverwrite) {
        error = std::string("Invalid user input: Cannot apply operation ") + operation_name(op.kind) +
                " to non-existent dataset";
        return false;
    }
    if (op.kind == kOverwrite) {
        if (!overwrite_fragments_valid(op.fragments, error) || !files_have_fields(op.fragments, error)) {
            return false;
        }
    }
    if ((op.kind == kAppend || op.kind == kMerge) && !files_have_fields(op.fragments, error)) {
        return false;
    }
    if (op.kind == kUpdate && (!files_have_fields(op.updated_fragments, error) || !files_have_fields(op.fragments, error))) {
        return false;
    }
    if (op.kind == kMerge && !merge_valid(*base, op, error)) {
        return false;
    }

    if (base != nullptr) {
        next = *base;
    } else {
        next = pb::Manifest{};
        next.data_format.file_format = "lance";
        next.data_format.version = storage_version_of(op.fragments);
    }
    if (op.has_schema) {
        next.fields = op.schema;
    }
    if (op.has_schema_metadata || op.kind == kOverwrite) {
        next.schema_metadata = op.schema_metadata;
    }
    std::uint64_t next_id = base != nullptr ? next_fragment_id(*base) : 0U;
    const bool stable = (next.reader_feature_flags & pb::kFlagStableRowIds) != 0U ||
                        (enable_stable_row_ids && (base == nullptr || op.kind == kOverwrite));

    switch (op.kind) {
        case kAppend: {
            auto added = op.fragments;
            assign_ids(added, next_id);
            if (stable && !stamp_missing_row_meta(added, next.next_row_id, new_version, error)) {
                return false;
            }
            next.fragments.insert(next.fragments.end(), added.begin(), added.end());
            break;
        }
        case kDelete: {
            const std::set<std::uint64_t> deleted(op.removed_ids.begin(), op.removed_ids.end());
            std::unordered_map<std::uint64_t, const pb::DataFragment*> updated;
            for (const auto& f : op.updated_fragments) {
                updated[f.id] = &f;
            }
            std::vector<pb::DataFragment> kept;
            for (auto& f : next.fragments) {
                if (deleted.count(f.id) != 0U) {
                    continue;
                }
                const auto it = updated.find(f.id);
                kept.push_back(it != updated.end() ? *it->second : f);
            }
            next.fragments = std::move(kept);
            retain_relevant_indices(next.indices, next.fields, next.fragments);
            break;
        }
        case kUpdate: {
            const std::set<std::uint64_t> removed(op.removed_ids.begin(), op.removed_ids.end());
            std::unordered_map<std::uint64_t, const pb::DataFragment*> updated;
            std::vector<std::uint64_t> updated_ids;
            for (const auto& f : op.updated_fragments) {
                updated.emplace(f.id, &f);
                updated_ids.push_back(f.id);
            }
            std::vector<pb::DataFragment> kept;
            for (auto& f : next.fragments) {
                if (removed.count(f.id) != 0U) {
                    continue;
                }
                const auto it = updated.find(f.id);
                kept.push_back(it != updated.end() ? *it->second : f);
            }
            next.fragments = std::move(kept);
            if (stable && op.rewrite_columns && !stamp_updated_rows(next.fragments, op, new_version, error)) {
                return false;
            }
            prune_updated_fields(next.indices, updated_ids, op.fields_modified);
            auto added = op.fragments;
            assign_ids(added, next_id);
            if (stable && !stamp_missing_row_meta(added, next.next_row_id, new_version, error)) {
                return false;
            }
            next.fragments.insert(next.fragments.end(), added.begin(), added.end());
            retain_relevant_indices(next.indices, next.fields, next.fragments);
            break;
        }
        case kOverwrite: {
            auto added = op.fragments;
            for (auto& f : added) {
                f.id = next_id++;  // every fragment of an overwrite is new, whatever id it came with
            }
            if (stable && !stamp_missing_row_meta(added, next.next_row_id, new_version, error)) {
                return false;
            }
            next.fragments = std::move(added);
            next.indices.clear();
            next.index_section_error.clear();
            for (const auto& [k, v] : op.config_upserts) {
                next.config[k] = v;
            }
            if (!op.initial_bases.empty()) {
                // The bases the dataset registers, ids from 1 in the order given (as Lance numbers them).
                std::vector<BasePath> bases;
                for (const auto& message : op.initial_bases) {
                    BasePath base;
                    if (!decode_base_path_message(message.data(), message.size(), base, error)) {
                        return false;
                    }
                    base.id = static_cast<std::uint32_t>(bases.size() + 1);
                    bases.push_back(std::move(base));
                }
                write_base_paths(next, bases);
            }
            break;
        }
        case kRewrite: {
            // New fragments numbered once, in group order (the order fragments end in is by id).
            std::vector<RewriteGroup> groups = op.groups;
            for (auto& g : groups) {
                assign_ids(g.new_fragments, next_id);
            }
            for (const auto& group : groups) {
                std::set<std::uint64_t> old_ids;
                for (const auto& f : group.old_fragments) {
                    const bool present = std::any_of(next.fragments.begin(), next.fragments.end(),
                                                     [&](const pb::DataFragment& x) { return x.id == f.id; });
                    if (!present) {
                        error = "commit conflict: dataset does not contain a fragment a rewrite operation wants to "
                                "replace: id=" + std::to_string(f.id);
                        return false;
                    }
                    old_ids.insert(f.id);
                }
                next.fragments.erase(std::remove_if(next.fragments.begin(), next.fragments.end(),
                                                    [&](const pb::DataFragment& f) { return old_ids.count(f.id); }),
                                     next.fragments.end());
                next.fragments.insert(next.fragments.end(), group.new_fragments.begin(), group.new_fragments.end());
            }
            std::set<std::array<std::uint8_t, 16>> seen;
            for (const auto& ri : op.rewritten_indices) {
                if (!seen.insert(ri.old_id).second) {
                    error = "Invalid user input: An invalid compaction plan must have been generated because multiple "
                            "tasks modified the same index: " + pb::uuid_string(ri.old_id);
                    return false;
                }
                auto it = std::find_if(next.indices.begin(), next.indices.end(),
                                       [&](const pb::IndexMetadata& i) { return i.uuid == ri.old_id; });
                if (it == next.indices.end()) {
                    continue;
                }
                auto ids = it->fragment_ids;
                if (!recalculate_bitmap(ids, groups, error)) {
                    return false;
                }
                *it = pb::make_index_metadata(ri.new_id, it->fields, it->name, it->dataset_version, ids,
                                              ri.details_type_url.empty() ? it->details_type_url : ri.details_type_url,
                                              ri.index_version, it->created_at, {},
                                              ri.details_type_url.empty() ? it->details_value : ri.details_value);
            }
            break;
        }
        case kMerge: {
            prune_merge_rewritten(next.indices, base->fragments, op.fragments);
            next.fragments = op.fragments;
            retain_relevant_indices(next.indices, next.fields, next.fragments);
            break;
        }
        case kProject: {
            std::set<std::int32_t> remaining;
            for (const auto& f : next.fields) {
                remaining.insert(f.id);
            }
            for (auto& fragment : next.fragments) {
                fragment.files.erase(
                    std::remove_if(fragment.files.begin(), fragment.files.end(),
                                   [&](const pb::DataFile& file) {
                                       return std::none_of(file.fields.begin(), file.fields.end(),
                                                           [&](std::int32_t id) { return remaining.count(id); });
                                   }),
                    fragment.files.end());
            }
            retain_relevant_indices(next.indices, next.fields, next.fragments);
            break;
        }
        case kReserveFragments: {
            next.has_max_fragment_id = true;
            next.max_fragment_id = static_cast<std::uint32_t>(
                (base->has_max_fragment_id ? base->max_fragment_id : 0U) + op.reserve);
            break;
        }
        case kUpdateConfig: {
            auto same = [](const std::string& s) { return s; };
            if (op.config_updates) apply_update_map(next.config, *op.config_updates, same);
            if (op.table_metadata_updates) apply_update_map(next.table_metadata, *op.table_metadata_updates, same);
            auto as_bytes = [](const std::string& s) { return Bytes(s.begin(), s.end()); };
            if (op.schema_metadata_updates) {
                apply_update_map(next.schema_metadata, *op.schema_metadata_updates, as_bytes);
            }
            for (const auto& [id, update] : op.field_metadata_updates) {
                auto it = std::find_if(next.fields.begin(), next.fields.end(),
                                       [&](const pb::Field& f) { return f.id == id; });
                if (it == next.fields.end()) {
                    error = "Invalid user input: Field with id " + std::to_string(id) + " does not exist";
                    return false;
                }
                apply_update_map(it->metadata, update, as_bytes);
            }
            break;
        }
        case kCreateIndex: {
            // Lance's CreateIndex: the indices it removes go, and the ones it adds replace any of the same
            // uuid. An index covers only the fragments still present (finish_create_index): the ones the
            // change read and that another writer removed drop out of its bitmap.
            for (const auto& gone : op.removed_indices) {
                next.indices.erase(std::remove_if(next.indices.begin(), next.indices.end(),
                                                  [&](const pb::IndexMetadata& i) { return i.uuid == gone.uuid; }),
                                   next.indices.end());
            }
            std::set<std::uint64_t> present;
            for (const auto& f : next.fragments) {
                present.insert(f.id);
            }
            for (const auto& created : op.new_indices) {
                pb::IndexMetadata index = created;
                auto ids = index.fragment_ids;
                ids.erase(std::remove_if(ids.begin(), ids.end(),
                                         [&](std::uint64_t id) { return present.count(id) == 0U; }),
                          ids.end());
                if (ids.size() != index.fragment_ids.size()) {
                    index.fragment_ids = std::move(ids);
                    index.fragment_bitmap_changed = true;
                }
                auto it = std::find_if(next.indices.begin(), next.indices.end(),
                                       [&](const pb::IndexMetadata& i) { return i.uuid == index.uuid; });
                if (index.name == kMemWalIndexName && it == next.indices.end() &&
                    std::any_of(next.indices.begin(), next.indices.end(),
                                [](const pb::IndexMetadata& i) { return i.name == kMemWalIndexName; })) {
                    error = "Invalid user input: MemWAL is already initialized on this dataset.";  // one per dataset
                    return false;
                }
                if (it != next.indices.end()) {
                    *it = std::move(index);
                } else {
                    next.indices.push_back(std::move(index));
                }
            }
            break;
        }
        case kDataReplacement:
            if (!data_replacement(next, op, error)) {
                return false;
            }
            break;
        case kUpdateBases: {
            // Lance's UpdateBases: each new base takes the next id after the largest one registered; a
            // name or a path already registered is a conflict.
            std::vector<BasePath> bases;
            if (!read_base_paths(next, bases, error)) {
                return false;
            }
            for (const auto& message : op.new_bases) {
                BasePath base;
                if (!decode_base_path_message(message.data(), message.size(), base, error)) {
                    return false;
                }
                for (const auto& known : bases) {
                    if ((base.has_name && known.has_name && base.name == known.name) || base.path == known.path) {
                        error = "Invalid user input: Conflict detected: Base path with name '" +
                                (base.has_name ? base.name : std::string("None")) + "' or path '" + base.path +
                                "' already exists";
                        return false;
                    }
                }
                std::uint32_t next_id = 1;
                for (const auto& known : bases) {
                    next_id = std::max(next_id, known.id + 1U);
                }
                base.id = next_id;
                bases.push_back(std::move(base));
            }
            write_base_paths(next, bases);
            break;
        }
        case kDataOverlay:
            // Lance's DataOverlay: each group's overlays go after the fragment's own, stamped with
            // this commit's version.
            for (const auto& [fragment_id, overlays] : op.overlay_groups) {
                auto it = std::find_if(next.fragments.begin(), next.fragments.end(),
                                       [&](const pb::DataFragment& f) { return f.id == fragment_id; });
                if (it == next.fragments.end()) {
                    error = "Invalid user input: data overlay for fragment " + std::to_string(fragment_id) +
                            ", which the dataset does not have";
                    return false;
                }
                if (!append_fragment_overlays(*it, overlays, new_version, error)) {
                    return false;
                }
            }
            break;
        default:
            error = std::string("LanceDataset.commit of ") + operation_name(op.kind) + " is not supported";
            return false;
    }

    std::stable_sort(next.fragments.begin(), next.fragments.end(),
                     [](const pb::DataFragment& a, const pb::DataFragment& b) { return a.id < b.id; });
    // A data file left holding nothing but tombstones is gone.
    for (auto& fragment : next.fragments) {
        fragment.files.erase(std::remove_if(fragment.files.begin(), fragment.files.end(),
                                            [](const pb::DataFile& file) {
                                                return !file.fields.empty() &&
                                                       std::all_of(file.fields.begin(), file.fields.end(),
                                                                   [](std::int32_t id) { return id == kTombstone; });
                                            }),
                             fragment.files.end());
    }
    // max_fragment_id is a high-water mark over the dataset's history.
    std::uint64_t max_id = next.has_max_fragment_id ? next.max_fragment_id : 0U;
    bool any = next.has_max_fragment_id;
    for (const auto& f : next.fragments) {
        max_id = std::max(max_id, f.id);
        any = true;
    }
    if (any) {
        next.has_max_fragment_id = true;
        next.max_fragment_id = static_cast<std::uint32_t>(max_id);
    }
    if (stable) {
        next.reader_feature_flags |= pb::kFlagStableRowIds;
        next.writer_feature_flags |= pb::kFlagStableRowIds;
    }
    // Overlays present: the feature flag Lance sets (pylance 12 reads such a dataset only with
    // LANCE_ENABLE_UNSTABLE_DATA_OVERLAY_FILES set).
    if (std::any_of(next.fragments.begin(), next.fragments.end(),
                    [](const pb::DataFragment& f) { return fragment_has_overlays(f); })) {
        next.reader_feature_flags |= kFlagDataOverlays;
        next.writer_feature_flags |= kFlagDataOverlays;
    } else {
        next.reader_feature_flags &= ~kFlagDataOverlays;
        next.writer_feature_flags &= ~kFlagDataOverlays;
    }
    const bool deletions = std::any_of(next.fragments.begin(), next.fragments.end(),
                                       [](const pb::DataFragment& f) { return f.deletion_file.present; });
    if (deletions) {
        next.reader_feature_flags |= pb::kFlagDeletionFiles;
        next.writer_feature_flags |= pb::kFlagDeletionFiles;
    } else {
        next.reader_feature_flags &= ~pb::kFlagDeletionFiles;
        next.writer_feature_flags &= ~pb::kFlagDeletionFiles;
    }
    return true;
}

void stamp(pb::Manifest& m) {
    m.has_timestamp = true;
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    m.timestamp_seconds = static_cast<std::int64_t>(nanos / 1000000000LL);
    m.timestamp_nanos = static_cast<std::int32_t>(nanos % 1000000000LL);
    m.writer_library = "nanolance";
    m.writer_version = nanolance_writer_version();
    m.transaction_file.clear();
    m.tag.clear();
}

bool load(const std::filesystem::path& path, bool has_version, std::uint64_t version, pb::Manifest& m,
          std::string& error) {
    if (has_version) {
        return load_manifest_version(path, version, m, error);
    }
    std::uint64_t latest = 0;
    return load_latest_manifest(path, m, latest, error);
}

}  // namespace

bool rebase_operation(const pb::Manifest& latest, const pb::Manifest& ours, const Operation& op,
                      const std::vector<Operation>& theirs, std::uint64_t new_version, pb::Manifest& next,
                      std::string& error) {
    if (op.kind == kRestore) {
        // A restore publishes the manifest it restores (Lance's Restore): the other writers' changes give way
        // to it. Only the high-water marks of the latest manifest carry over.
        next = ours;
        const std::uint64_t high = std::max(latest.has_max_fragment_id ? latest.max_fragment_id : 0U,
                                            ours.has_max_fragment_id ? ours.max_fragment_id : 0U);
        next.has_max_fragment_id = true;
        next.max_fragment_id = static_cast<std::uint32_t>(high);
        next.next_row_id = std::max(ours.next_row_id, latest.next_row_id);
        return true;
    }
    const bool stable = (latest.reader_feature_flags & pb::kFlagStableRowIds) != 0U;
    if (stable && (op.kind == kAppend || op.kind == kUpdate || op.kind == kMerge || op.kind == kOverwrite)) {
        // Rows added by this change carry row ids from the base's counter; re-stamping them over the latest
        // one is not done here, so the caller re-runs the operation instead (a retryable conflict).
        error = "commit conflict: stable row ids: a rebase of " + std::string(operation_name(op.kind)) +
                " is not supported";
        return false;
    }
    // The fragments this change adds were numbered after the version it was made from; another writer may have
    // taken those ids since, so they are numbered after the latest version's (as apply_operation numbers a new
    // fragment whose id is 0).
    Operation renumbered = op;
    if (op.kind == kAppend || op.kind == kUpdate) {
        for (auto& f : renumbered.fragments) {
            f.id = 0;
        }
    }
    if (!apply_operation(&latest, renumbered, next, error, false, new_version)) {
        return false;
    }
    if (op.kind == kCreateIndex) {
        // Lance's finish_create_index: a concurrent Update that rewrote a field this index covers prunes the
        // fragments it rewrote from the index (it covers them again only after a rebuild).
        std::set<std::array<std::uint8_t, 16>> ours_uuids;
        for (const auto& index : op.new_indices) {
            ours_uuids.insert(index.uuid);
        }
        std::vector<pb::IndexMetadata> mine;
        for (const auto& index : next.indices) {
            if (ours_uuids.count(index.uuid) != 0U) {
                mine.push_back(index);
            }
        }
        for (const auto& t : theirs) {
            if (t.kind != kUpdate) {
                continue;
            }
            std::vector<std::uint64_t> updated;
            for (const auto& f : t.updated_fragments) {
                updated.push_back(f.id);
            }
            prune_updated_fields(mine, updated, t.fields_modified);
        }
        for (const auto& index : mine) {
            auto it = std::find_if(next.indices.begin(), next.indices.end(),
                                   [&](const pb::IndexMetadata& i) { return i.uuid == index.uuid; });
            if (it != next.indices.end()) {
                *it = index;
            }
        }
    }
    return true;
}

bool dataset_commit_hand_built(const std::filesystem::path& dataset_path, const HandBuiltCommit& commit,
                               std::uint64_t& new_version, std::string& error) {
    error.clear();
    Operation op;
    if (!decode_operation(commit.operation_field, commit.operation, op, error)) {
        return false;
    }
    pb::Manifest base;
    const bool exists = commit.base_version != 0U;
    if (exists && !load_manifest_version(dataset_path, commit.base_version, base, error)) {
        return false;
    }
    if (exists && !check_manifest_writable(base, WriteKind::Commit, error)) {
        return false;
    }
    pb::Manifest next;
    if (op.kind == kRestore) {
        if (!exists) {
            error = "Invalid user input: Cannot apply operation Restore to non-existent dataset";
            return false;
        }
        if (!load_manifest_version(dataset_path, op.restore_version, next, error)) {
            return false;
        }
        if (base.has_max_fragment_id) {
            next.has_max_fragment_id = true;
            next.max_fragment_id = std::max(next.max_fragment_id, base.max_fragment_id);
        }
        next.next_row_id = std::max(next.next_row_id, base.next_row_id);
    } else if (!apply_operation(exists ? &base : nullptr, op, next, error, commit.enable_stable_row_ids,
                                commit.base_version + 1U)) {
        return false;
    }
    next.version = commit.base_version + 1U;
    if (commit.detached) {
        std::random_device device;
        const auto random = (static_cast<std::uint64_t>(device()) << 32U) | device();
        next.version = random | (1ULL << 63U);
    }
    stamp(next);
    next.operation = pb::Manifest::Operation::Derive;
    next.explicit_operation_field = commit.operation_field;
    next.explicit_operation = commit.operation;
    next.explicit_read_version = commit.read_version;
    next.transaction_properties = commit.transaction_properties;
    if (!publish_manifest(dataset_path, next, error)) {
        return false;
    }
    new_version = next.version;
    return true;
}

bool dataset_fragment_messages(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                               std::vector<Bytes>& out, std::string& error) {
    pb::Manifest m;
    if (!load(dataset_path, has_version, version, m, error)) {
        return false;
    }
    out.clear();
    for (const auto& f : m.fragments) {
        out.push_back(pb::encode_data_fragment(f));
    }
    return true;
}

bool dataset_field_messages(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                            std::vector<Bytes>& fields, std::map<std::string, Bytes>& schema_metadata,
                            std::string& error) {
    pb::Manifest m;
    if (!load(dataset_path, has_version, version, m, error)) {
        return false;
    }
    fields.clear();
    for (const auto& f : m.fields) {
        fields.push_back(pb::encode_field(f));
    }
    schema_metadata = m.schema_metadata;
    return true;
}

}  // namespace nano_lance
