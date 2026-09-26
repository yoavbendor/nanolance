// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/// Decoded columns' large buffers, kept for reuse once Arrow releases them.
///
/// A read's output is fresh memory, and fresh memory costs a page fault per 4 KiB the first time it is
/// touched -- for a memory-bound column (a few ms to decode 16 MB of int64) that is most of the read.
/// Rust Lance and pyarrow allocate through pooling allocators (jemalloc, mimalloc) that hand a
/// released buffer to the next read already faulted in; glibc's malloc does that on the thread that
/// freed it but not across threads, so a parallel read (parallel.hpp) faulted its output in on every
/// run. This pool does it for nanolance's own buffers, whichever thread frees them.
///
/// Bounded: NANOLANCE_BUFFER_POOL_MB (default 128; 0 turns it off) of buffers of at least 1 MiB,
/// each dropped once it has sat unused for two seconds (checked on the next take or give).
namespace nano_lance::buffer_pool {

/// An empty vector with capacity for at least `bytes`, from the pool when one fits (not more than
/// four times larger), otherwise an empty vector with no capacity.
std::vector<std::uint8_t> take(std::size_t bytes);

/// Keep `buffer`'s capacity for a later take, or free it (too small, pool off or full).
void give(std::vector<std::uint8_t>&& buffer);

}  // namespace nano_lance::buffer_pool
