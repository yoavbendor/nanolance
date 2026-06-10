// CPU bulk smoke: drive nanotins::bulk_for_each with an exec::static_thread_pool scheduler over a
// partitioned sum kernel. Proves stdexec is wired into the build and the scheduler-agnostic bulk works
// on this host; on a CUDA host the same call takes an nvexec scheduler.

#include "nanotins/bulk.hpp"

#include <exec/static_thread_pool.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main() {
    const std::size_t n = 100000;
    std::vector<std::uint32_t> data(n);
    for (std::size_t i = 0; i < n; ++i) {
        data[i] = static_cast<std::uint32_t>(i % 7);
    }
    const std::size_t num_tasks = 16;
    std::vector<std::uint64_t> partial(num_tasks, 0);

    exec::static_thread_pool pool(8);
    // Kernel captures POD pointers only (device-safe shape): accumulate per task into partial[task].
    const std::uint32_t* d = data.data();
    std::uint64_t* p = partial.data();
    nanotins::bulk_for_each(pool.get_scheduler(), num_tasks, num_tasks, [=](std::size_t task) {
        const std::size_t start = (task * n) / num_tasks;
        const std::size_t end = ((task + 1) * n) / num_tasks;
        std::uint64_t acc = 0;
        for (std::size_t i = start; i < end; ++i) {
            acc += d[i];
        }
        p[task] = acc;
    });

    std::uint64_t total = 0;
    for (auto v : partial) {
        total += v;
    }
    std::uint64_t expect = 0;
    for (std::size_t i = 0; i < n; ++i) {
        expect += i % 7;
    }
    if (total != expect) {
        std::fprintf(stderr, "bulk smoke failed: total=%llu expect=%llu\n",
                     static_cast<unsigned long long>(total), static_cast<unsigned long long>(expect));
        return 1;
    }
    std::puts("nanotins bulk smoke ok");
    return 0;
}
