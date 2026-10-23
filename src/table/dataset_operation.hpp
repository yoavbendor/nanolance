// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Internal to nanolance: a transaction's operation (lance.table.Transaction) decoded as Lance reads it.
// The hand-built commit path (dataset_transaction.cpp) and the conflict checks (commit_conflicts.cpp)
// share this decoder, so every transaction file is read the same way.
namespace nano_lance {

using OpBytes = std::vector<std::uint8_t>;

/// The operation's field number in lance.table.Transaction.
inline constexpr std::uint32_t kAppend = 100, kDelete = 101, kOverwrite = 102, kCreateIndex = 103, kRewrite = 104,
                               kMerge = 105, kRestore = 106, kReserveFragments = 107, kUpdate = 108, kProject = 109,
                               kUpdateConfig = 110, kDataReplacement = 111, kUpdateBases = 114, kDataOverlay = 115;
/// Clone (113) has no decoder: Lance's checks treat it as compatible with every operation.
inline constexpr std::uint32_t kCloneField = 113;
inline constexpr std::uint32_t kFirstOperationField = 100, kLastOperationField = 115;

/// Lance's name for an operation's field number ("an operation" for one it does not know).
const char* operation_name(std::uint32_t field);

/// Transaction.UpdateMap: entries (key, optional value: absent removes the key), and `replace`.
struct UpdateMap {
    std::vector<std::pair<std::string, std::optional<std::string>>> entries;
    bool replace = false;
};

/// Replace (or merge into) `target` by `update`, entry by entry.
template <typename Map, typename Convert>
void apply_update_map(Map& target, const UpdateMap& update, Convert convert) {
    if (update.replace) {
        target.clear();
    }
    for (const auto& [key, value] : update.entries) {
        if (value.has_value()) {
            target[key] = convert(*value);
        } else {
            target.erase(key);
        }
    }
}

struct RewriteGroup {
    std::vector<pb::DataFragment> old_fragments;
    std::vector<pb::DataFragment> new_fragments;
};

struct RewrittenIndex {
    std::array<std::uint8_t, 16> old_id{};
    std::array<std::uint8_t, 16> new_id{};
    std::string details_type_url;
    OpBytes details_value;
    std::uint32_t index_version = 0;
};

/// Every part of a decoded operation; which are set depends on its kind.
struct Operation {
    std::uint32_t kind = 0;
    std::vector<pb::DataFragment> fragments;          // Append, Overwrite, Merge; Update's new
    std::vector<pb::DataFragment> updated_fragments;  // Delete, Update
    std::vector<std::uint64_t> removed_ids;           // Delete's deleted, Update's removed
    bool has_schema = false;
    std::vector<pb::Field> schema;                    // Overwrite, Merge, Project
    bool has_schema_metadata = false;
    std::map<std::string, OpBytes> schema_metadata;   // Overwrite, Merge
    std::map<std::string, std::string> config_upserts;  // Overwrite
    std::vector<OpBytes> initial_bases;               // Overwrite: each a lance.table.BasePath
    std::vector<std::uint64_t> fields_modified;       // Update
    bool rewrite_columns = false;                     // Update: update_mode RewriteColumns
    /// Update: per updated fragment, the offsets of the rows it changed (a portable Roaring bitmap).
    std::vector<std::pair<std::uint64_t, OpBytes>> updated_fragment_offsets;
    std::vector<RewriteGroup> groups;                 // Rewrite
    std::vector<RewrittenIndex> rewritten_indices;    // Rewrite
    std::uint64_t restore_version = 0;                // Restore
    std::uint64_t reserve = 0;                        // ReserveFragments
    std::optional<UpdateMap> config_updates, table_metadata_updates, schema_metadata_updates;  // UpdateConfig
    std::map<std::int32_t, UpdateMap> field_metadata_updates;
    std::vector<std::pair<std::uint64_t, pb::DataFile>> replacements;  // DataReplacement
    std::vector<OpBytes> new_bases;                   // UpdateBases: each a lance.table.BasePath
    /// DataOverlay: per group, the fragment and its new overlays (each a lance.table.DataOverlayFile).
    std::vector<std::pair<std::uint64_t, std::vector<OpBytes>>> overlay_groups;
    /// CreateIndex: the indices it adds and the ones it removes (Lance's new_indices / removed_indices).
    std::vector<pb::IndexMetadata> new_indices;
    std::vector<pb::IndexMetadata> removed_indices;
    /// Project, Merge: Lance's preserves_nullability (false when the field is absent).
    bool preserves_nullability = false;
};

/// Decode `message`, the operation of field number `kind`. False with `error` when it is malformed or
/// an operation this decoder does not read.
bool decode_operation(std::uint32_t kind, const OpBytes& message, Operation& op, std::string& error);

/// The operation of a transaction file (`_transactions/<name>`, a lance.table.Transaction): its field
/// number and message. False with `error` when the file has none.
bool transaction_file_operation(const OpBytes& file, std::uint32_t& field, OpBytes& message, std::string& error);

}  // namespace nano_lance
