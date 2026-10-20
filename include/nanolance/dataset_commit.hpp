// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <utility>

#include <optional>

namespace nano_lance {

/// The version a mutation was made from: the version its dataset handle was opened at (pylance commits
/// from there, so a concurrent commit is classified against what the change saw). Empty: the latest.
using ReadVersion = std::optional<std::uint64_t>;

/// The manifest a mutation reads: `read_version` when given, else the latest. `version` is its number.
bool load_read_version(const std::filesystem::path& dataset_path, ReadVersion read_version, pb::Manifest& manifest,
                       std::uint64_t& version, std::string& error);

/// Publish `manifest` (a version's manifest, changed) as the version after the one it was read at
/// (`manifest.version`), stamped with the time and nanolance as its writer. When another writer took
/// that version since, the change is rebased onto theirs if they only added fragments, as Lance
/// rebases compatible transactions; otherwise it fails with "commit conflict".
bool commit_next_version(const std::filesystem::path& dataset_path, pb::Manifest manifest, std::uint64_t& new_version,
                         std::string& error);

/// Commit conflicts since the last reset, for tests: commits rebased onto a concurrent version, operations
/// run again after a retryable conflict, and commits refused as incompatible (not run again).
struct CommitConflictStats {
    std::uint64_t rebased = 0;
    std::uint64_t retried = 0;
    std::uint64_t refused = 0;
};
CommitConflictStats commit_conflict_stats();
void reset_commit_conflict_stats();
/// Count one run of an operation again after a retryable conflict (retry_on_conflict does this).
void count_commit_retry();

/// Run `attempt` again (up to Lance's 20 tries, with a short jittered pause) while it fails with
/// "commit conflict": for operations that read the latest version afresh each time, as Lance re-runs
/// a delete, update, merge or compaction after a retryable conflict.
template <typename F>
bool retry_on_conflict(F&& attempt, std::string& error) {
    for (int tries = 0;; ++tries) {
        if (attempt()) {
            return true;
        }
        if (tries + 1 >= 20 || error.rfind("commit conflict", 0) != 0) {
            return false;
        }
        count_commit_retry();
        thread_local std::mt19937 rng{std::random_device{}()};
        const int ceiling = std::min(50, 1 << std::min(tries, 5));
        std::this_thread::sleep_for(std::chrono::milliseconds(std::uniform_int_distribution<int>(1, ceiling)(rng)));
    }
}

/// retry_on_conflict for an operation made from `read_version`: the first attempt reads that version (the
/// one the change was made from), a re-run after a retryable conflict reads the latest, as Lance's does.
template <typename F>
bool retry_from_read_version(ReadVersion read_version, F&& attempt, std::string& error) {
    ReadVersion from = read_version;
    return retry_on_conflict([&] { return attempt(std::exchange(from, std::nullopt)); }, error);
}

}  // namespace nano_lance
