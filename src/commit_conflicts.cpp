// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "commit_conflicts.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Each check_* below is one arm of Lance 12's conflict_resolver.rs (check_txn and the check_*_txn it
// dispatches to), restricted to what nanolance's transactions record and to commits without affected
// rows: an overlap of fragments that a Delete or Update changes is retryable, as Lance has it.

namespace nano_lance {
namespace {

std::set<std::uint64_t> fragment_ids(const std::vector<pb::DataFragment>& fragments) {
    std::set<std::uint64_t> out;
    for (const auto& f : fragments) {
        out.insert(f.id);
    }
    return out;
}

/// The existing fragments a Delete or an Update changes or removes.
std::set<std::uint64_t> delete_update_ids(const Operation& op) {
    auto out = fragment_ids(op.updated_fragments);
    out.insert(op.removed_ids.begin(), op.removed_ids.end());
    return out;
}

/// The existing fragments a Rewrite replaces (its groups' old fragments).
std::set<std::uint64_t> rewrite_ids(const Operation& op) {
    std::set<std::uint64_t> out;
    for (const auto& g : op.groups) {
        for (const auto& f : g.old_fragments) {
            out.insert(f.id);
        }
    }
    return out;
}

std::set<std::uint64_t> replacement_ids(const Operation& op) {
    std::set<std::uint64_t> out;
    for (const auto& r : op.replacements) {
        out.insert(r.first);
    }
    return out;
}

std::set<std::uint64_t> overlay_ids(const Operation& op) {
    std::set<std::uint64_t> out;
    for (const auto& g : op.overlay_groups) {
        out.insert(g.first);
    }
    return out;
}

bool intersects(const std::set<std::uint64_t>& a, const std::set<std::uint64_t>& b) {
    return std::any_of(b.begin(), b.end(), [&](std::uint64_t id) { return a.count(id) != 0U; });
}

/// The fragments an index covers (its bitmap); false when the index records no coverage.
bool index_fragments(const std::vector<pb::IndexMetadata>& indices, std::set<std::uint64_t>& out) {
    for (const auto& index : indices) {
        if (!index.has_fragment_bitmap) {
            return false;
        }
        out.insert(index.fragment_ids.begin(), index.fragment_ids.end());
    }
    return true;
}

bool may_alter_nullability(const Operation& op) {
    return (op.kind == kProject || op.kind == kMerge) && !op.preserves_nullability;
}

bool supplies_values(const Operation& op) {
    return op.kind == kAppend || op.kind == kUpdate || op.kind == kDataReplacement || op.kind == kDataOverlay;
}

bool updates_schema_or_field_metadata(const Operation& op) {
    if (op.kind != kUpdateConfig) {
        return false;
    }
    const auto& schema = op.schema_metadata_updates;
    if (schema && (schema->replace || !schema->entries.empty())) {
        return true;
    }
    return std::any_of(op.field_metadata_updates.begin(), op.field_metadata_updates.end(),
                       [](const auto& kv) { return kv.second.replace || !kv.second.entries.empty(); });
}

/// Config keys an Overwrite or an UpdateConfig sets.
std::set<std::string> upserted_config_keys(const Operation& op) {
    std::set<std::string> out;
    if (op.kind == kOverwrite) {
        for (const auto& kv : op.config_upserts) {
            out.insert(kv.first);
        }
    }
    if (op.kind == kUpdateConfig && op.config_updates) {
        for (const auto& [key, value] : op.config_updates->entries) {
            if (value) {
                out.insert(key);
            }
        }
    }
    return out;
}

/// Config keys an UpdateConfig removes.
std::set<std::string> deleted_config_keys(const Operation& op) {
    std::set<std::string> out;
    if (op.kind == kUpdateConfig && op.config_updates) {
        for (const auto& [key, value] : op.config_updates->entries) {
            if (!value) {
                out.insert(key);
            }
        }
    }
    return out;
}

/// Lance's upsert_key_conflict: one side sets or removes a config key the other sets.
bool upsert_key_conflict(const Operation& a, const Operation& b) {
    const auto a_up = upserted_config_keys(a);
    const auto b_up = upserted_config_keys(b);
    const auto a_del = deleted_config_keys(a);
    const auto b_del = deleted_config_keys(b);
    return std::any_of(a_up.begin(), a_up.end(),
                       [&](const std::string& k) { return b_up.count(k) != 0U || b_del.count(k) != 0U; }) ||
           std::any_of(b_up.begin(), b_up.end(),
                       [&](const std::string& k) { return a_up.count(k) != 0U || a_del.count(k) != 0U; });
}

bool update_maps_conflict(const std::optional<UpdateMap>& a, const std::optional<UpdateMap>& b) {
    if (!a || !b) {
        return false;
    }
    if (a->replace || b->replace) {
        return true;
    }
    std::set<std::string> keys;
    for (const auto& e : a->entries) {
        keys.insert(e.first);
    }
    return std::any_of(b->entries.begin(), b->entries.end(),
                       [&](const auto& e) { return keys.count(e.first) != 0U; });
}

/// Lance's modifies_same_metadata: two UpdateConfigs that change the same table or schema metadata, or
/// the metadata of the same field.
bool modifies_same_metadata(const Operation& a, const Operation& b) {
    if (a.kind != kUpdateConfig || b.kind != kUpdateConfig) {
        return false;
    }
    if (update_maps_conflict(a.table_metadata_updates, b.table_metadata_updates)) {
        return true;
    }
    if (a.schema_metadata_updates && b.schema_metadata_updates) {
        return true;
    }
    return std::any_of(a.field_metadata_updates.begin(), a.field_metadata_updates.end(),
                       [&](const auto& kv) { return b.field_metadata_updates.count(kv.first) != 0U; });
}

/// Lance's check_create_index_txn, for two CreateIndex commits (frag-reuse and MemWAL index names are
/// not written by nanolance and are not special-cased here).
bool index_names_conflict(const Operation& ours, const Operation& theirs) {
    const auto named = [](const std::vector<pb::IndexMetadata>& list, const std::string& name) {
        return std::any_of(list.begin(), list.end(), [&](const pb::IndexMetadata& i) { return i.name == name; });
    };
    for (const auto& n : ours.new_indices) {
        if (named(theirs.new_indices, n.name)) {
            return true;  // the same name
        }
        if (!named(ours.removed_indices, n.name) && named(theirs.removed_indices, n.name)) {
            return true;  // a new index whose old one the other commit drops
        }
    }
    for (const auto& c : theirs.new_indices) {
        if (!named(theirs.removed_indices, c.name) && named(ours.removed_indices, c.name)) {
            return true;
        }
    }
    for (const auto& r : ours.removed_indices) {
        for (const auto& cr : theirs.removed_indices) {
            if (cr.uuid != r.uuid) {
                continue;
            }
            const auto touches = [&](const std::vector<pb::IndexMetadata>& list) {
                return std::any_of(list.begin(), list.end(), [&](const pb::IndexMetadata& i) {
                    return i.name == r.name || i.name == cr.name;
                });
            };
            if (touches(ours.new_indices) || touches(theirs.new_indices)) {
                return true;
            }
        }
    }
    return false;
}

// ── one arm per operation of ours ───────────────────────────────────────────────────────────────

Verdict delete_check(const Operation& ours, const Operation& theirs) {
    switch (theirs.kind) {
        case kAppend:
        case kReserveFragments:
        case kProject:
        case kUpdateConfig:
        case kCreateIndex:
        case kDataOverlay:  // deletions take precedence over overlays
        case kUpdateBases:
            return Verdict::Compatible;
        case kRewrite:
            return intersects(delete_update_ids(ours), rewrite_ids(theirs)) ? Verdict::Retryable : Verdict::Compatible;
        case kDataReplacement:
            return intersects(delete_update_ids(ours), replacement_ids(theirs)) ? Verdict::Retryable
                                                                                : Verdict::Compatible;
        case kUpdate:
        case kDelete:
            return intersects(delete_update_ids(ours), delete_update_ids(theirs)) ? Verdict::Retryable
                                                                                  : Verdict::Compatible;
        case kMerge:
            return Verdict::Retryable;
        case kOverwrite:
        case kRestore:
            return Verdict::Incompatible;
        default:
            return Verdict::Retryable;
    }
}

Verdict update_check(const Operation& ours, const Operation& theirs) {
    switch (theirs.kind) {
        case kCreateIndex:
        case kReserveFragments:
        case kProject:
        case kUpdateConfig:
        case kUpdateBases:
        case kAppend:  // (an inserted-rows filter, for a keyed merge, is not recorded: see the report)
            return Verdict::Compatible;
        case kDataOverlay: {
            // Lance's update_mode is not recorded: an update that adds fragments is read as RewriteRows.
            if (ours.fragments.empty()) {
                return Verdict::Compatible;
            }
            return intersects(delete_update_ids(ours), overlay_ids(theirs)) ? Verdict::Retryable : Verdict::Compatible;
        }
        case kRewrite:
            return intersects(delete_update_ids(ours), rewrite_ids(theirs)) ? Verdict::Retryable : Verdict::Compatible;
        case kDataReplacement:
            return intersects(delete_update_ids(ours), replacement_ids(theirs)) ? Verdict::Retryable
                                                                                : Verdict::Compatible;
        case kUpdate:
        case kDelete:
            return intersects(delete_update_ids(ours), delete_update_ids(theirs)) ? Verdict::Retryable
                                                                                  : Verdict::Compatible;
        case kMerge:
            return Verdict::Retryable;
        case kOverwrite:
        case kRestore:
            return Verdict::Incompatible;
        default:
            return Verdict::Retryable;
    }
}

Verdict create_index_check(const Operation& ours, const Operation& theirs) {
    switch (theirs.kind) {
        case kAppend:
        case kDataOverlay:
        case kUpdateBases:
        case kDelete:
        case kUpdate:  // a pruned index: a rebase, not a conflict
        case kMerge:   // only a MemWAL index conflicts with a merge
        case kReserveFragments:
        case kProject:
        case kUpdateConfig:
            return Verdict::Compatible;
        case kCreateIndex:
            return index_names_conflict(ours, theirs) ? Verdict::Retryable : Verdict::Compatible;
        case kRewrite: {
            std::set<std::uint64_t> covered;
            if (!index_fragments(ours.new_indices, covered)) {
                return Verdict::Retryable;
            }
            return intersects(covered, rewrite_ids(theirs)) ? Verdict::Retryable : Verdict::Compatible;
        }
        case kDataReplacement: {
            std::set<std::int32_t> depended;
            for (const auto& index : ours.new_indices) {
                depended.insert(index.fields.begin(), index.fields.end());
            }
            for (const auto& r : theirs.replacements) {
                for (const auto field : r.second.fields) {
                    if (depended.count(field) != 0U) {
                        return Verdict::Retryable;
                    }
                }
            }
            return Verdict::Compatible;
        }
        case kOverwrite:
        case kRestore:
            return Verdict::Incompatible;
        default:
            return Verdict::Retryable;
    }
}

Verdict rewrite_check(const Operation& ours, const Operation& theirs) {
    const auto replaced = rewrite_ids(ours);
    switch (theirs.kind) {
        case kAppend:
        case kReserveFragments:
        case kProject:
        case kUpdateConfig:
        case kUpdateBases:
            return Verdict::Compatible;
        case kDelete:
        case kUpdate:
            return intersects(replaced, delete_update_ids(theirs)) ? Verdict::Retryable : Verdict::Compatible;
        case kDataOverlay:
            return intersects(replaced, overlay_ids(theirs)) ? Verdict::Retryable : Verdict::Compatible;
        case kRewrite:
            return intersects(replaced, rewrite_ids(theirs)) ? Verdict::Retryable : Verdict::Compatible;
        case kDataReplacement:
            return intersects(replaced, replacement_ids(theirs)) ? Verdict::Retryable : Verdict::Compatible;
        case kMerge:
            return Verdict::Retryable;
        case kCreateIndex: {
            std::set<std::uint64_t> covered;
            if (!index_fragments(theirs.new_indices, covered)) {
                return Verdict::Retryable;
            }
            return intersects(replaced, covered) ? Verdict::Retryable : Verdict::Compatible;
        }
        case kOverwrite:
        case kRestore:
            return Verdict::Incompatible;
        default:
            return Verdict::Retryable;
    }
}

Verdict merge_check(const Operation& theirs) {
    switch (theirs.kind) {
        case kCreateIndex:
        case kReserveFragments:
        case kUpdateConfig:
        case kUpdateBases:
            return Verdict::Compatible;
        case kUpdate:
        case kAppend:
        case kDelete:
        case kRewrite:
        case kMerge:
        case kDataReplacement:
        case kDataOverlay:
            return Verdict::Retryable;
        case kOverwrite:
        case kRestore:
        case kProject:
            return Verdict::Incompatible;
        default:
            return Verdict::Retryable;
    }
}

Verdict project_check(const Operation& theirs) {
    switch (theirs.kind) {
        case kAppend:
        case kUpdate:
        case kDelete:
        case kUpdateConfig:
        case kCreateIndex:
        case kDataReplacement:
        case kDataOverlay:
        case kRewrite:
        case kReserveFragments:
        case kUpdateBases:
            return Verdict::Compatible;
        case kMerge:
        case kProject:
            return Verdict::Retryable;
        case kOverwrite:
        case kRestore:
            return Verdict::Incompatible;
        default:
            return Verdict::Retryable;
    }
}

Verdict reserve_check(const Operation& theirs) {
    switch (theirs.kind) {
        case kOverwrite:
        case kRestore:
            return Verdict::Incompatible;
        case kAppend:
        case kDelete:
        case kCreateIndex:
        case kRewrite:
        case kDataReplacement:
        case kDataOverlay:
        case kMerge:
        case kReserveFragments:
        case kUpdate:
        case kProject:
        case kUpdateConfig:
        case kUpdateBases:
            return Verdict::Compatible;
        default:
            return Verdict::Retryable;
    }
}

Verdict restore_check(const Operation& theirs) {
    switch (theirs.kind) {
        case kAppend:
        case kDelete:
        case kOverwrite:
        case kCreateIndex:
        case kRewrite:
        case kDataReplacement:
        case kDataOverlay:
        case kMerge:
        case kRestore:
        case kReserveFragments:
        case kUpdateBases:
        case kUpdate:
        case kProject:
        case kUpdateConfig:
            return Verdict::Compatible;
        default:
            return Verdict::Retryable;
    }
}

Verdict update_config_check(const Operation& ours, const Operation& theirs) {
    switch (theirs.kind) {
        case kOverwrite:
            return (ours.schema_metadata_updates || !ours.field_metadata_updates.empty() ||
                    upsert_key_conflict(ours, theirs))
                       ? Verdict::Incompatible
                       : Verdict::Compatible;
        case kUpdateConfig:
            return (upsert_key_conflict(ours, theirs) || modifies_same_metadata(ours, theirs)) ? Verdict::Incompatible
                                                                                              : Verdict::Compatible;
        case kAppend:
        case kDelete:
        case kCreateIndex:
        case kRewrite:
        case kDataReplacement:
        case kDataOverlay:
        case kMerge:
        case kRestore:
        case kReserveFragments:
        case kUpdate:
        case kProject:
        case kUpdateBases:
            return Verdict::Compatible;
        default:
            return Verdict::Retryable;
    }
}

Verdict overwrite_check(const Operation& ours, const Operation& theirs) {
    switch (theirs.kind) {
        case kOverwrite:
            return upsert_key_conflict(ours, theirs) ? Verdict::Incompatible : Verdict::Retryable;
        case kUpdateConfig:
            return upsert_key_conflict(ours, theirs) ? Verdict::Incompatible : Verdict::Compatible;
        case kAppend:
        case kDelete:
        case kCreateIndex:
        case kRewrite:
        case kDataReplacement:
        case kDataOverlay:
        case kMerge:
        case kRestore:
        case kReserveFragments:
        case kUpdate:
        case kProject:
        case kUpdateBases:
            return Verdict::Compatible;
        default:
            return Verdict::Retryable;
    }
}

Verdict append_check(const Operation& theirs) {
    switch (theirs.kind) {
        case kOverwrite:
        case kRestore:
            return Verdict::Incompatible;
        case kAppend:
        case kRewrite:
        case kCreateIndex:
        case kDelete:
        case kUpdate:
        case kReserveFragments:
        case kProject:
        case kUpdateBases:
        case kMerge:
        case kUpdateConfig:
        case kDataReplacement:
        case kDataOverlay:
            return Verdict::Compatible;
        default:
            return Verdict::Retryable;
    }
}

}  // namespace

namespace {

bool same_data_files(const pb::DataFragment& a, const pb::DataFragment& b) {
    if (a.files.size() != b.files.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.files.size(); ++i) {
        const auto& x = a.files[i];
        const auto& y = b.files[i];
        if (x.path != y.path || x.fields != y.fields || x.column_indices != y.column_indices ||
            x.file_major_version != y.file_major_version || x.file_minor_version != y.file_minor_version ||
            x.file_size_bytes != y.file_size_bytes) {
            return false;
        }
    }
    return true;
}

}  // namespace

Verdict check_txn_with_affected_rows(const Operation& ours, const Operation& theirs, const pb::Manifest& base) {
    const Verdict plain = check_txn(ours, theirs);
    const bool both = (ours.kind == kDelete || ours.kind == kUpdate) && (theirs.kind == kDelete || theirs.kind == kUpdate);
    if (plain != Verdict::Retryable || !both) {
        return plain;
    }
    const auto mine = delete_update_ids(ours);
    for (const auto id : theirs.removed_ids) {
        if (mine.count(id) != 0U) {
            return Verdict::Retryable;  // a fragment this change deletes from is removed by the other
        }
    }
    for (const auto& f : theirs.updated_fragments) {
        if (mine.count(f.id) == 0U) {
            continue;
        }
        const auto was = std::find_if(base.fragments.begin(), base.fragments.end(),
                                      [&](const pb::DataFragment& b) { return b.id == f.id; });
        if (was == base.fragments.end() || !same_data_files(*was, f)) {
            return Verdict::Retryable;  // the other change rewrote the data of a fragment this one deletes from
        }
    }
    return Verdict::Compatible;
}

Verdict check_txn(const Operation& ours, const Operation& theirs) {
    // Either order: the claim was checked without the write's data.
    if ((may_alter_nullability(ours) && supplies_values(theirs)) ||
        (supplies_values(ours) && may_alter_nullability(theirs))) {
        return Verdict::Retryable;
    }
    // Merge carries a complete schema from its read version: rebasing it over a metadata update can
    // discard one side's metadata changes, in either commit order.
    if ((ours.kind == kMerge && updates_schema_or_field_metadata(theirs)) ||
        (updates_schema_or_field_metadata(ours) && theirs.kind == kMerge)) {
        return Verdict::Retryable;
    }
    if (theirs.kind == kCloneField) {
        return Verdict::Compatible;  // Lance's checks pass a Clone in every case
    }
    switch (ours.kind) {
        case kDelete: return delete_check(ours, theirs);
        case kUpdate: return update_check(ours, theirs);
        case kCreateIndex: return create_index_check(ours, theirs);
        case kRewrite: return rewrite_check(ours, theirs);
        case kMerge: return merge_check(theirs);
        case kProject: return project_check(theirs);
        case kReserveFragments: return reserve_check(theirs);
        case kRestore: return restore_check(theirs);
        case kUpdateConfig: return update_config_check(ours, theirs);
        case kOverwrite: return overwrite_check(ours, theirs);
        case kAppend: return append_check(theirs);
        default: return Verdict::Retryable;
    }
}

}  // namespace nano_lance

namespace nano_lance {

bool commit_verdict(std::uint32_t ours_field, const std::vector<std::uint8_t>& ours_message,
                    std::uint32_t theirs_field, const std::vector<std::uint8_t>& theirs_message, int& verdict,
                    std::string& error) {
    Operation ours;
    Operation theirs;
    if (!decode_operation(ours_field, ours_message, ours, error) ||
        !decode_operation(theirs_field, theirs_message, theirs, error)) {
        return false;
    }
    switch (check_txn(ours, theirs)) {
        case Verdict::Compatible: verdict = 0; break;
        case Verdict::Retryable: verdict = 1; break;
        case Verdict::Incompatible: verdict = 2; break;
    }
    return true;
}

}  // namespace nano_lance

