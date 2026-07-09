// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Memory-safety substrate for the nanolance READ path.
//
// nanolance's writer produces its own bytes (trusted), but the reader parses UNTRUSTED on-disk Lance
// files (manifest protobuf, data-file footer, encoded column buffers) and can fetch external blobs by
// (uri, position, size). This header centralizes the primitives the decode path uses to stay safe
// against hostile inputs WITHOUT slowing the hot loops: overflow-checked integer math, size/limit
// budgets, and a byte-assembly integer load (never reinterpret_cast disk bytes to a typed pointer).
//
// The checks here are meant to run once per page/chunk/header, not once per value — validate a
// declared size against the real buffer up front, then let the inner per-value loop run check-free.

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace nano_lance {

// ── Read limits (compile-time-tunable budget on untrusted on-disk sizes) ──────────────────────────
// Override any of these at build time with -DNANOLANCE_MAX_*=... , or per-read via ReadLimits.
// They bound what a *hostile* file can make the reader allocate; legitimate files are far below them.
#ifndef NANOLANCE_MAX_UNCOMPRESSED_BYTES
#define NANOLANCE_MAX_UNCOMPRESSED_BYTES (std::uint64_t{8} << 30)  // 8 GiB per decoded buffer
#endif
#ifndef NANOLANCE_MAX_ROWS_PER_COLUMN
#define NANOLANCE_MAX_ROWS_PER_COLUMN (std::uint64_t{1} << 34)  // ~17.2 billion rows
#endif
#ifndef NANOLANCE_MAX_COLUMNS
#define NANOLANCE_MAX_COLUMNS (std::uint32_t{1} << 20)  // 1,048,576 columns
#endif
#ifndef NANOLANCE_MAX_MANIFEST_ELEMENTS
#define NANOLANCE_MAX_MANIFEST_ELEMENTS (std::size_t{1} << 24)  // 16.7M fields/fragments/files/pages
#endif

struct ReadLimits {
    std::uint64_t max_uncompressed_bytes = NANOLANCE_MAX_UNCOMPRESSED_BYTES;
    std::uint64_t max_rows_per_column = NANOLANCE_MAX_ROWS_PER_COLUMN;
    std::uint32_t max_columns = NANOLANCE_MAX_COLUMNS;
    std::size_t max_manifest_elements = NANOLANCE_MAX_MANIFEST_ELEMENTS;
};

// The limits every decode-path check (`default_read_limits()`) currently consults. thread_local so
// concurrent reads on different threads (e.g. one trusted, one not) never interfere with each other.
// Only the four DoS-budget comparisons above read this; every overflow-safe bounds check
// (checked_add/checked_mul/range_in_bounds, and the offset/size comparisons built on them) is
// unconditional code, not data-driven by this struct, so it's never affected by trusted mode.
inline ReadLimits& active_read_limits() {
    thread_local ReadLimits limits{};
    return limits;
}

inline const ReadLimits& default_read_limits() { return active_read_limits(); }

// A limits set with every budget maxed out — used by trusted-input mode (see ScopedTrustedRead) to skip
// the DoS/OOM budget comparisons for a self-produced pipeline that doesn't need them. This does NOT
// disable any bounds check: `checked_add`/`checked_mul`/`range_in_bounds` and every offset/size compare
// built on them keep running exactly as in the default (untrusted) path.
inline ReadLimits trusted_read_limits() {
    ReadLimits limits;
    limits.max_uncompressed_bytes = std::numeric_limits<std::uint64_t>::max();
    limits.max_rows_per_column = std::numeric_limits<std::uint64_t>::max();
    limits.max_columns = std::numeric_limits<std::uint32_t>::max();
    limits.max_manifest_elements = std::numeric_limits<std::size_t>::max();
    return limits;
}

// RAII scope that swaps this thread's active read limits for the duration of a read, then restores the
// previous value (so nested/sequential reads on the same thread can't leak a trusted scope into an
// unrelated untrusted one). Construct with `trusted_read_limits()` to enable trusted-input mode, or with
// any other ReadLimits to run a read under a custom budget.
class ScopedReadLimits {
   public:
    explicit ScopedReadLimits(const ReadLimits& limits) : previous_(active_read_limits()) {
        active_read_limits() = limits;
    }
    ~ScopedReadLimits() { active_read_limits() = previous_; }
    ScopedReadLimits(const ScopedReadLimits&) = delete;
    ScopedReadLimits& operator=(const ScopedReadLimits&) = delete;

   private:
    ReadLimits previous_;
};

// ── Overflow-checked unsigned arithmetic ──────────────────────────────────────────────────────────
// Return false (leaving `out` untouched) when the true result would wrap. Use before any resize /
// reserve / bounds compare whose operands come from disk.
[[nodiscard]] inline bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) {
        return false;
    }
    out = a + b;
    return true;
}

[[nodiscard]] inline bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a != 0U && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return false;
    }
    out = a * b;
    return true;
}

// True when `v` fits in std::size_t (i.e. can be a container size / index on this platform).
[[nodiscard]] inline bool fits_size_t(std::uint64_t v) noexcept {
    return v <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
}

// True when the half-open range [offset, offset+len) lies fully within a buffer of `total` bytes,
// computed without wrapping. The canonical "is this slice in bounds?" check for disk offsets.
[[nodiscard]] inline bool range_in_bounds(std::uint64_t offset, std::uint64_t len,
                                          std::uint64_t total) noexcept {
    std::uint64_t end = 0;
    return checked_add(offset, len, end) && end <= total;
}

// ── Byte-assembly integer load (no reinterpret_cast of disk bytes to a typed pointer) ─────────────
// Reads sizeof(T) little-endian bytes from `p` into a T via memcpy + std::bit_cast, avoiding
// alignment/aliasing UB. The caller is responsible for having proven `p` has sizeof(T) readable bytes.
template <typename T>
[[nodiscard]] inline T load_le(const std::uint8_t* p) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "load_le requires a trivially copyable type");
    std::array<std::uint8_t, sizeof(T)> tmp{};
    std::memcpy(tmp.data(), p, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) {
        for (std::size_t i = 0; i < sizeof(T) / 2U; ++i) {
            std::swap(tmp[i], tmp[sizeof(T) - 1U - i]);
        }
    }
    return std::bit_cast<T>(tmp);
}

}  // namespace nano_lance
