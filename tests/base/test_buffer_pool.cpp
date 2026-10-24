// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The buffer pool for decoded columns (include/nanolance/buffer_pool.hpp): a released large buffer is handed
// to the next read that fits it (not more than four times larger), small buffers are never pooled, and the
// pool stays within its budget (128 MB by default) by dropping the oldest buffers. The hit and miss
// counters record each decision. Runs with the default budget, set explicitly so the test does not depend
// on the environment.

#include "nanolance/buffer_pool.hpp"
#include "nanolance/work_stats.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

constexpr std::size_t kMiB = std::size_t{1} << 20U;
constexpr std::size_t kKiB = std::size_t{1} << 10U;

std::vector<std::uint8_t> buffer_of(std::size_t capacity) {
    std::vector<std::uint8_t> b;
    b.reserve(capacity);
    return b;
}

// Drains the pool of the buffers the checks below give it (up to 2 MiB each): a take of 1 MiB gets
// the ones of 1 to 4 MiB, a take of 256 KiB the smaller ones.
void drain() {
    for (int i = 0; i < 1000; ++i) {
        const bool large = nano_lance::buffer_pool::take(kMiB).capacity() != 0U;
        const bool small = nano_lance::buffer_pool::take(256U * kKiB).capacity() != 0U;
        if (!large && !small) {
            return;
        }
    }
}

}  // namespace

int main() {
    setenv("NANOLANCE_BUFFER_POOL_MB", "128", 1);
    int checks = 0;
    auto& counters = nano_lance::work_stats::counters();

    // Small requests (under 256 KiB) get an empty vector and are not counted as misses.
    {
        nano_lance::work_stats::reset();
        require(nano_lance::buffer_pool::take(0).capacity() == 0U, "take(0) gives an empty buffer");
        require(nano_lance::buffer_pool::take(4U * kKiB).capacity() == 0U, "a small take gives an empty buffer");
        require(counters.buffer_pool_misses.load() == 0U && counters.buffer_pool_hits.load() == 0U,
                "small takes are not counted");
        checks += 3;
    }

    // A large take from an empty pool misses; the counter says so.
    {
        drain();
        nano_lance::work_stats::reset();
        require(nano_lance::buffer_pool::take(kMiB).capacity() == 0U, "an empty pool gives no buffer");
        require(counters.buffer_pool_misses.load() == 1U, "the miss is counted");
        checks += 2;
    }

    // A released buffer comes back, empty, with at least its capacity, to a take of the same size.
    {
        drain();
        nano_lance::work_stats::reset();
        nano_lance::buffer_pool::give(buffer_of(kMiB));
        auto got = nano_lance::buffer_pool::take(kMiB);
        require(got.capacity() >= kMiB, "a released 1 MiB buffer is reused for a 1 MiB take");
        require(got.empty(), "a reused buffer comes back empty");
        require(counters.buffer_pool_hits.load() == 1U, "the reuse is counted as a hit");
        checks += 3;
    }

    // Four times larger: a 2 MiB buffer is not handed to a 256 KiB take (more than four times the request is
    // left in the pool), but it is handed to a 1 MiB take (2 MiB <= 4 * 1 MiB).
    {
        drain();
        nano_lance::work_stats::reset();
        nano_lance::buffer_pool::give(buffer_of(2U * kMiB));
        require(nano_lance::buffer_pool::take(256U * kKiB).capacity() == 0U,
                "a 2 MiB buffer is not given to a 256 KiB request");
        require(counters.buffer_pool_misses.load() == 1U, "that refused take is a miss");
        const auto got = nano_lance::buffer_pool::take(kMiB);
        require(got.capacity() >= 2U * kMiB, "a 2 MiB buffer is given to a 1 MiB request");
        require(counters.buffer_pool_hits.load() == 1U, "that take is a hit");
        checks += 4;
    }

    // Buffers below 256 KiB are never kept: giving one away frees it, so a later take cannot find it.
    {
        drain();
        nano_lance::work_stats::reset();
        nano_lance::buffer_pool::give(buffer_of(128U * kKiB));
        require(nano_lance::buffer_pool::take(256U * kKiB).capacity() == 0U,
                "a small buffer given back is not pooled");
        checks += 1;
    }

    // The budget: 200 buffers of 1 MiB given back leave at most 128 MiB pooled, the newest ones, so exactly
    // 128 of them can be taken again.
    {
        drain();
        for (int i = 0; i < 200; ++i) {
            nano_lance::buffer_pool::give(buffer_of(kMiB));
        }
        int taken = 0;
        for (;;) {
            if (nano_lance::buffer_pool::take(kMiB).capacity() == 0U) {
                break;
            }
            ++taken;
        }
        require(taken == 128, "the pool keeps exactly its 128 MiB budget (got " + std::to_string(taken) + ")");
        checks += 1;
    }

    std::cout << "buffer_pool: " << checks << " checks passed\n";
    return 0;
}
