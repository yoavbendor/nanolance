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

namespace nano_lance {

/// Publish `manifest` (a version's manifest, changed) as the version after the one it was read at
/// (`manifest.version`), stamped with the time and nanolance as its writer. When another writer took
/// that version since, the change is rebased onto theirs if they only added fragments, as Lance
/// rebases compatible transactions; otherwise it fails with "commit conflict".
bool commit_next_version(const std::filesystem::path& dataset_path, pb::Manifest manifest, std::uint64_t& new_version,
                         std::string& error);

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
        thread_local std::mt19937 rng{std::random_device{}()};
        const int ceiling = std::min(50, 1 << std::min(tries, 5));
        std::this_thread::sleep_for(std::chrono::milliseconds(std::uniform_int_distribution<int>(1, ceiling)(rng)));
    }
}

}  // namespace nano_lance
