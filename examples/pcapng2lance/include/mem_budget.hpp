// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Pick a per-chunk row count from this host's memory budget, so an enrich stage running on a different
// machine than the L1 chunker sizes its bulk to its OWN RAM/VRAM. Budget is an explicit --mem-bytes or
// (default) a fraction of detected free RAM; rows-per-chunk = budget / estimated per-row cost.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

namespace membudget {

// Available physical memory in bytes, or 0 if it can't be determined.
inline std::uint64_t available_bytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX s{};
    s.dwLength = sizeof(s);
    if (GlobalMemoryStatusEx(&s)) {
        return static_cast<std::uint64_t>(s.ullAvailPhys);
    }
    return 0;
#elif defined(__linux__)
    struct sysinfo info{};
    if (sysinfo(&info) == 0) {
        return static_cast<std::uint64_t>(info.freeram) * info.mem_unit;
    }
    return 0;
#else
    return 0;
#endif
}

// Resolve the working-set budget: explicit value if > 0, else a fraction of free RAM, else a fallback.
inline std::uint64_t resolve_budget(std::uint64_t explicit_bytes, double fraction = 0.25,
                                    std::uint64_t fallback = std::uint64_t{1} << 30) {
    if (explicit_bytes > 0) {
        return explicit_bytes;
    }
    const std::uint64_t avail = available_bytes();
    if (avail == 0) {
        return fallback;
    }
    return static_cast<std::uint64_t>(static_cast<double>(avail) * fraction);
}

// Rows per bulk chunk from the budget and an estimated per-row working-set cost, clamped to a sane range.
inline std::size_t rows_per_chunk(std::uint64_t budget_bytes, std::size_t per_row_cost_bytes,
                                  std::size_t min_rows = 1, std::size_t max_rows = 4U << 20) {
    if (per_row_cost_bytes == 0) {
        per_row_cost_bytes = 1;
    }
    const std::uint64_t n = budget_bytes / per_row_cost_bytes;
    if (n < min_rows) {
        return min_rows;
    }
    if (n > max_rows) {
        return max_rows;
    }
    return static_cast<std::size_t>(n);
}

}  // namespace membudget
