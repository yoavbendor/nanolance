// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <atomic>
#include <cstdint>

/// Counters of the work nanolance did -- bytes read from data files, page windows decoded, row ranges
/// a read was cut into, bytes a write buffered -- for tests that guard HOW a result was produced, not
/// only what it is: a take of 64 rows from a 150 MB audio page must read a few MB, not the page.
/// Timings flake on shared machines; these do not. Process-wide, relaxed atomics: a count costs
/// one uncontended increment. Read and reset through nano_lance_work_stats / nano_lance_reset_work_stats
/// (Python: nanolance._work_stats / _reset_work_stats). Not a stable API.
namespace nano_lance::work_stats {

struct Counters {
    std::atomic<std::uint64_t> data_bytes_read{0};      // bytes read from Lance data files
    std::atomic<std::uint64_t> data_reads{0};           // reads issued
    std::atomic<std::uint64_t> largest_read{0};         // the biggest single read, in bytes
    std::atomic<std::uint64_t> page_windows{0};         // parts of pages decoded instead of the page
    std::atomic<std::uint64_t> read_morsels{0};         // row ranges fragment reads were cut into
    std::atomic<std::uint64_t> fragment_reads{0};       // fragment reads (each one or more morsels)
    std::atomic<std::uint64_t> parallel_column_writes{0};  // data files written column-parallel
    std::atomic<std::uint64_t> write_buffered_bytes{0};    // bytes those buffered before the file
    std::atomic<std::uint64_t> buffer_pool_hits{0};     // output buffers reused from the pool
    std::atomic<std::uint64_t> buffer_pool_misses{0};   // large ones it could not supply
    std::atomic<std::uint64_t> take_cache_hits{0};      // take() columns served from a decoded copy
};

inline Counters& counters() {
    static Counters c;
    return c;
}

inline void add(std::atomic<std::uint64_t>& counter, std::uint64_t n) {
    counter.fetch_add(n, std::memory_order_relaxed);
}

inline void raise_to(std::atomic<std::uint64_t>& counter, std::uint64_t n) {
    auto seen = counter.load(std::memory_order_relaxed);
    while (seen < n && !counter.compare_exchange_weak(seen, n, std::memory_order_relaxed)) {
    }
}

inline void reset() {
    auto& c = counters();
    for (auto* x : {&c.data_bytes_read, &c.data_reads, &c.largest_read, &c.page_windows, &c.read_morsels,
                    &c.fragment_reads, &c.parallel_column_writes, &c.write_buffered_bytes, &c.buffer_pool_hits,
                    &c.buffer_pool_misses, &c.take_cache_hits}) {
        x->store(0, std::memory_order_relaxed);
    }
}

}  // namespace nano_lance::work_stats
