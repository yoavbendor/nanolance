// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

/// A dataset's versions kept and let go, as Lance keeps them: tags (`_refs/tags/<name>.json`),
/// cleanup of old versions and the files only they use, the automatic cleanup a dataset's
/// `lance.auto_cleanup.*` config asks for after each commit, and dropping a whole dataset. The rules
/// follow Lance 12 (`dataset/cleanup.rs`, `dataset/refs.rs`, `validate_dataset_root_for_drop`), so a
/// dataset pylance and nanolance both write to is cleaned the same way by either.
namespace nano_lance {

// ── tags ───────────────────────────────────────────────────────────────────────────────────────────

struct TagInfo {
    std::string name;
    std::optional<std::string> branch;  // nanolance tags the main branch only
    std::uint64_t version = 0;
    std::string created_at;  // RFC 3339, UTC; empty when the tag file has none
    std::string updated_at;
    std::uint64_t manifest_size = 0;
    std::map<std::string, std::string> metadata;
};

/// Every tag, by name.
bool list_tags(const std::filesystem::path& dataset_path, std::vector<TagInfo>& out, std::string& error);
bool get_tag(const std::filesystem::path& dataset_path, const std::string& name, TagInfo& out, std::string& error);
/// Tag `version` (0: the latest) as `name`. Fails when the tag exists or the version does not, with
/// Lance's messages ("Ref conflict error: ...", "Version not found error: ...").
bool create_tag(const std::filesystem::path& dataset_path, const std::string& name, std::uint64_t version,
                std::string& error);
/// Point an existing tag at `version` (0: the latest), keeping its creation time and metadata.
bool update_tag(const std::filesystem::path& dataset_path, const std::string& name, std::uint64_t version,
                std::string& error);
bool delete_tag(const std::filesystem::path& dataset_path, const std::string& name, std::string& error);
/// Replace a tag's metadata (its version and times unchanged, as in Lance).
bool replace_tag_metadata(const std::filesystem::path& dataset_path, const std::string& name,
                          const std::map<std::string, std::string>& metadata, std::string& error);

// ── cleanup ────────────────────────────────────────────────────────────────────────────────────────

/// Which versions to remove: every condition set must hold (Lance's CleanupPolicy). The read
/// version and every newer one, and tagged versions, are always kept.
struct CleanupPolicy {
    std::optional<std::int64_t> before_timestamp_ns;  // committed before (since the epoch, UTC)
    std::optional<std::uint64_t> before_version;      // version below
    std::optional<std::set<std::uint64_t>> versions;  // one of these
    /// Remove files no manifest references even when they are newer than 7 days (they may belong to
    /// a write in progress).
    bool delete_unverified = false;
    /// Fail, removing nothing, when a tagged version matches the policy.
    bool error_if_tagged_old_versions = true;
    /// At most this many deletes a second.
    std::optional<std::uint64_t> delete_rate_limit;
};

/// Keep the newest `n` versions (> 0): sets `policy.before_version` as Lance's retain_n_versions.
bool cleanup_retain_versions(const std::filesystem::path& dataset_path, std::uint64_t n, CleanupPolicy& policy,
                             std::string& error);

struct CleanupStats {
    std::uint64_t bytes_removed = 0;
    std::uint64_t old_versions = 0;
    std::uint64_t data_files_removed = 0;
    std::uint64_t transaction_files_removed = 0;
    std::uint64_t index_files_removed = 0;
    std::uint64_t deletion_files_removed = 0;
};

struct CleanupCandidate {
    std::string path;  // relative to the dataset
    std::string kind;  // "manifest", "data", "transaction", "index", "deletion", "temporary_manifest"
    bool unverified = false;  // removable only by age (or delete_unverified), no old manifest names it
    std::uint64_t size_bytes = 0;
};

struct CleanupResult {
    std::uint64_t read_version = 0;
    CleanupStats stats;
    std::vector<CleanupCandidate> candidates;  // explain only, up to its limit
    bool candidates_truncated = false;
};

/// Remove the versions `policy` selects, as of `read_version` (0: the latest), and every file that
/// only they reference (`execute`), or report what would go (`!execute`, listing up to
/// `max_candidates` files).
bool cleanup_old_versions(const std::filesystem::path& dataset_path, std::uint64_t read_version,
                          const CleanupPolicy& policy, bool execute, std::size_t max_candidates,
                          CleanupResult& out, std::string& error);

/// The cleanup a just-committed version's `lance.auto_cleanup.*` config asks for (every `interval`
/// versions; `older_than`, `retain_versions`, `delete_rate_limit`), run as Lance runs it after a
/// commit: as of the version before, its errors ignored. Called by every commit.
void run_auto_cleanup(const std::filesystem::path& dataset_path, std::uint64_t committed_version,
                      const std::map<std::string, std::string>& config);

/// A duration as humantime writes and reads it ("14days", "1s", "1h 30m", "250ms").
bool parse_duration(const std::string& text, std::int64_t& nanoseconds);

// ── drop ───────────────────────────────────────────────────────────────────────────────────────────

/// Delete the dataset and everything under its root. Refused unless the root holds a manifest that
/// reads (or a namespace marker), as Lance refuses, so a mistyped parent directory survives; a
/// missing path is an error unless `ignore_not_found`.
bool drop_dataset(const std::filesystem::path& dataset_path, bool ignore_not_found, std::string& error);

}  // namespace nano_lance
