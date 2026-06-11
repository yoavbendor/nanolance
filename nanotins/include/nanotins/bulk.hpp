#pragma once

// Scheduler-agnostic partitioned bulk: run kernel `k(i)` for i in [0, n), split into `num_tasks`
// contiguous ranges, on the given stdexec scheduler. The CPU path passes an
// exec::static_thread_pool scheduler; a CUDA build later passes an nvexec::stream_context scheduler and
// the SAME call runs on the GPU. The kernel must therefore be device-safe: POD captures, no allocation,
// no STL/boost in the body, and (for the GPU build) `__host__ __device__` — which nanotins overlay/store
// already are. Mirrors stdexec_gpu_experiment's run_bulk_cpu/run_bulk_gpu, where the scheduler is the
// only difference.

#include "nanotins/portability.hpp"

#include <cstddef>

#include <stdexec/execution.hpp>

namespace nanotins {

template <class Scheduler, class Kernel>
void bulk_for_each(Scheduler sch, std::size_t num_tasks, std::size_t n, Kernel k) {
    if (num_tasks == 0) {
        num_tasks = 1;
    }
    namespace ex = stdexec;
    auto pipeline = ex::schedule(sch) | ex::bulk(stdexec::par, num_tasks, [=](std::size_t task) {
        const std::size_t start = (task * n) / num_tasks;
        const std::size_t end = ((task + 1) * n) / num_tasks;
        for (std::size_t i = start; i < end; ++i) {
            k(i);
        }
    });
    ex::sync_wait(std::move(pipeline));
}

// Sequential reference executor: same (num_tasks, n, kernel) contract as bulk_for_each, but runs the
// kernel in-thread with no scheduler. It is the readable/debuggable baseline and the correctness oracle
// for the bulk path — select it (e.g. behind a --sequential flag) to get identical output from a plain
// loop you can step through. `num_tasks` is ignored (one task).
template <class Kernel>
void serial_for_each(std::size_t /*num_tasks*/, std::size_t n, Kernel k) {
    for (std::size_t i = 0; i < n; ++i) {
        k(i);
    }
}

}  // namespace nanotins
