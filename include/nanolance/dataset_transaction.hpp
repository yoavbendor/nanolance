// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

/// Transactions built by hand, as Lance's commit API takes them (pylance's LanceDataset.commit, what
/// Ray / Daft / Spark writers use): data files written apart from any commit (write_fragments), then
/// one operation -- Append, Overwrite, Delete, Update, Merge, Project, Rewrite, Restore,
/// ReserveFragments, UpdateConfig, DataReplacement -- that says what to do with them.
///
/// The operation travels as Lance itself encodes it, a lance.table.Transaction operation message, and
/// is applied as Lance's build_manifest applies it (lance-table's transaction/manifest_build.rs),
/// after Lance's checks (transaction/validate.rs). It goes into the commit's transaction file as given.
namespace nano_lance {

struct HandBuiltCommit {
    /// The operation's field number in lance.table.Transaction (100 Append, 101 Delete, 102 Overwrite,
    /// 104 Rewrite, 105 Merge, 106 Restore, 107 ReserveFragments, 108 Update, 109 Project,
    /// 110 UpdateConfig, 111 DataReplacement) and its encoded message.
    std::uint32_t operation_field = 0;
    std::vector<std::uint8_t> operation;
    /// The version the change is applied to and published after (as base_version + 1, exactly: when
    /// another writer took that version, the commit fails with "commit conflict" and changes nothing).
    /// The caller checks what was committed since read_version first, as Lance does. 0: there is no
    /// dataset yet, and the operation (an Overwrite) creates it.
    std::uint64_t base_version = 0;
    /// The version the change was made from, recorded in the transaction file.
    std::uint64_t read_version = 0;
    std::map<std::string, std::string> transaction_properties;
    /// Commit a detached version (Lance's detached commits): built on `base_version`, numbered at
    /// random with the high bit set, outside the lineage -- never the latest, reachable by its number.
    bool detached = false;
};

bool dataset_commit_hand_built(const std::filesystem::path& dataset_path, const HandBuiltCommit& commit,
                               std::uint64_t& new_version, std::string& error);

/// A version's fragments and schema fields, each as Lance encodes it (lance.table.DataFragment,
/// lance.file.Field), and its schema metadata: what a hand-built transaction carries back.
bool dataset_fragment_messages(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                               std::vector<std::vector<std::uint8_t>>& out, std::string& error);
bool dataset_field_messages(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                            std::vector<std::vector<std::uint8_t>>& fields,
                            std::map<std::string, std::vector<std::uint8_t>>& schema_metadata, std::string& error);

}  // namespace nano_lance
