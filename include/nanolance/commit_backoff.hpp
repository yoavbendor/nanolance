// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// How often, and after how long a pause, a commit that lost a race to another writer is tried again.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>

namespace nano_lance {

/// Attempts at one commit: 20 on a local path (Lance's default conflict_retries), 200 on an object store, where
/// many separate machines finishing together collide again and again and one try (a list, a read, two writes) costs
/// tens of milliseconds. NANOLANCE_COMMIT_ATTEMPTS (1 to 100,000) overrides both.
inline int commit_attempts(bool object_store) {
    if (const char* env = std::getenv("NANOLANCE_COMMIT_ATTEMPTS"); env != nullptr && *env != '\0') {
        char* end = nullptr;
        const long n = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && n >= 1 && n <= 100000) {
            return static_cast<int>(n);
        }
    }
    return object_store ? 200 : 20;
}

/// The pause before attempt `attempt + 1`. Locally a jittered 1 ms up to a ceiling doubling to 50 ms, as before.
/// On an object store "full jitter": uniform in [1 ms, ceiling], the ceiling doubling from 25 ms to 2 s -- a
/// millisecond-scale pause only reschedules the collision there. The generator is seeded afresh from the entropy
/// source on every pause: a per-thread one is copied into every process a writer forks, and forked writers then
/// waited the same times and collided again on every try.
inline void commit_back_off(int attempt, bool object_store) {
    const int step = std::clamp(attempt, 0, 30);
    const int ceiling = object_store ? std::min(2000, 25 << std::min(step, 7)) : std::min(50, 1 << std::min(step, 5));
    std::random_device device;
    std::mt19937 rng(device());
    std::this_thread::sleep_for(std::chrono::milliseconds(std::uniform_int_distribution<int>(1, ceiling)(rng)));
}

}  // namespace nano_lance
