// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/buffer_pool.hpp"

#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

namespace nano_lance::buffer_pool {
namespace {

constexpr std::size_t kMinBuffer = std::size_t{1} << 20U;
constexpr auto kIdle = std::chrono::seconds(2);

using Clock = std::chrono::steady_clock;

struct Pool {
    std::mutex mutex;
    std::multimap<std::size_t, std::pair<Clock::time_point, std::vector<std::uint8_t>>> buffers;  // by capacity
    std::size_t held = 0;
    std::size_t budget = 0;

    Pool() {
        const char* env = std::getenv("NANOLANCE_BUFFER_POOL_MB");
        const auto mb = env == nullptr || *env == '\0' ? 128ULL : std::strtoull(env, nullptr, 10);
        budget = static_cast<std::size_t>(mb) << 20U;
    }

    // Drop what has idled too long, then the oldest until `extra` more bytes fit the budget.
    void evict(std::size_t extra) {
        const auto now = Clock::now();
        for (auto it = buffers.begin(); it != buffers.end();) {
            if (now - it->second.first > kIdle) {
                held -= it->first;
                it = buffers.erase(it);
            } else {
                ++it;
            }
        }
        while (!buffers.empty() && held + extra > budget) {
            auto oldest = buffers.begin();
            for (auto it = buffers.begin(); it != buffers.end(); ++it) {
                if (it->second.first < oldest->second.first) {
                    oldest = it;
                }
            }
            held -= oldest->first;
            buffers.erase(oldest);
        }
    }
};

Pool& pool() {
    static auto* instance = new Pool();  // never destroyed: releases may come during static teardown
    return *instance;
}

}  // namespace

std::vector<std::uint8_t> take(std::size_t bytes) {
    auto& p = pool();
    if (p.budget == 0U || bytes < kMinBuffer) {
        return {};
    }
    std::vector<std::uint8_t> out;
    const std::lock_guard<std::mutex> lock(p.mutex);
    const auto it = p.buffers.lower_bound(bytes);
    if (it != p.buffers.end() && it->first / 4U <= bytes) {
        p.held -= it->first;
        out = std::move(it->second.second);
        p.buffers.erase(it);
        out.clear();
    }
    p.evict(0);
    return out;
}

void give(std::vector<std::uint8_t>&& buffer) {
    auto& p = pool();
    const auto capacity = buffer.capacity();
    if (p.budget == 0U || capacity < kMinBuffer || capacity > p.budget) {
        std::vector<std::uint8_t>().swap(buffer);
        return;
    }
    std::vector<std::uint8_t> kept = std::move(buffer);
    std::vector<std::uint8_t> dropped;  // freed outside the lock
    {
        const std::lock_guard<std::mutex> lock(p.mutex);
        p.evict(capacity);
        p.buffers.emplace(capacity, std::make_pair(Clock::now(), std::move(kept)));
        p.held += capacity;
    }
}

}  // namespace nano_lance::buffer_pool
