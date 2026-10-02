// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <functional>

namespace nano_lance::parallel {

/// How many threads nanolance may use, the calling thread included. 1 runs everything on the calling
/// thread exactly as a single-threaded build would: no pool is started, no work is split.
///
/// Set by set_threads(); otherwise NANOLANCE_THREADS; otherwise the CPUs this process may run on
/// (its affinity mask, so `taskset` and container CPU limits are respected).
std::size_t threads();

/// 0 restores the default. Takes effect for work started afterwards.
void set_threads(std::size_t n);

/// Run `task(i)` for every i in [0, n), on the calling thread and any idle pool threads, and return
/// when all have finished. Tasks may themselves call for_each: the caller of each level works on its
/// own tasks while waiting, so nesting cannot deadlock. The calling thread's read limits
/// (read_safety.hpp) apply to every task, whichever thread runs it. An exception from a task is
/// rethrown here once all tasks are done (the first one, in index order).
///
/// With threads() == 1 or n <= 1 this is a plain loop.
void for_each(std::size_t n, const std::function<void(std::size_t)>& task);

}  // namespace nano_lance::parallel
