// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/parallel.hpp"

#include "nanolance/data_file_reader.hpp"
#include "nanolance/read_safety.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace nano_lance::parallel {
namespace {

/// One for_each call. Lives on its caller's stack: the caller removes it from the queue and waits for
/// every helper to leave it before returning.
struct Job {
    const std::function<void(std::size_t)>* task = nullptr;
    std::size_t n = 0;
    std::size_t max_helpers = 0;
    ReadLimits limits;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> helpers{0};  // pool threads currently inside this job
    std::mutex left_mutex;                // helpers leaving, for the caller waiting on them
    std::condition_variable left;
    std::vector<std::exception_ptr> errors;  // one slot per task; each written by its runner only

    void work() {
        for (;;) {
            const auto i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) {
                return;
            }
            try {
                (*task)(i);
            } catch (...) {
                errors[i] = std::current_exception();
            }
        }
    }
};

/// A fixed set of threads that help whichever jobs are queued. Created on first parallel use, sized
/// once to the most threads asked for so far, grown if asked for more; never destroyed (process exit
/// takes the threads with it, so no static-destruction order to get wrong at interpreter shutdown).
class Pool {
   public:
    void ensure(std::size_t workers) {
        const std::lock_guard<std::mutex> lock(mutex_);
        while (threads_ < workers) {
            std::thread([this] { loop(); }).detach();
            ++threads_;
        }
    }

    void run(Job& job) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(&job);
        }
        wake_.notify_all();
        job.work();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const auto it = std::find(queue_.begin(), queue_.end(), &job);
            if (it != queue_.end()) {
                queue_.erase(it);
            }
        }
        // Tasks still running belong to helpers; each leaves the job only after its last task.
        std::unique_lock<std::mutex> lock(job.left_mutex);
        job.left.wait(lock, [&] { return job.helpers.load(std::memory_order_acquire) == 0U; });
    }

   private:
    void loop() {
        for (;;) {
            Job* job = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] {
                    for (auto* j : queue_) {
                        if (j->next.load(std::memory_order_relaxed) < j->n &&
                            j->helpers.load(std::memory_order_relaxed) < j->max_helpers) {
                            job = j;
                            return true;
                        }
                    }
                    return false;
                });
                // Joined under the lock: its caller cannot have removed it and returned yet.
                job->helpers.fetch_add(1, std::memory_order_acq_rel);
            }
            {
                const ScopedReadLimits limits(job->limits);
                const DataFileReadScope scope;
                job->work();
            }
            {
                // Under the job's lock, so the caller cannot see 0, return, and free the job between
                // this decrement and the notify.
                const std::lock_guard<std::mutex> lock(job->left_mutex);
                job->helpers.fetch_sub(1, std::memory_order_acq_rel);
                job->left.notify_all();
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job*> queue_;
    std::size_t threads_ = 0;
};

std::size_t available_cpus() {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int n = CPU_COUNT(&set);
        if (n > 0) {
            return static_cast<std::size_t>(n);
        }
    }
#endif
    return std::max<std::size_t>(1U, std::thread::hardware_concurrency());
}

std::size_t default_threads() {
    const char* env = std::getenv("NANOLANCE_THREADS");
    if (env != nullptr && *env != '\0') {
        const auto n = std::strtoull(env, nullptr, 10);
        if (n > 0U) {
            return static_cast<std::size_t>(n);
        }
    }
    return available_cpus();
}

std::atomic<std::size_t> g_threads{0};  // 0: not decided yet

/// The pool for this process. A child forked from a process that had started one (a PyTorch
/// DataLoader worker, say) inherits the object but not its threads: it gets a pool of its own.
Pool& pool() {
    static std::mutex mutex;
    static Pool* instance = nullptr;
#if defined(__unix__) || defined(__APPLE__)
    static pid_t owner = 0;
    const std::lock_guard<std::mutex> lock(mutex);
    if (instance == nullptr || owner != getpid()) {
        instance = new Pool();  // the forked-over one is left as is: its threads do not exist here
        owner = getpid();
    }
#else
    const std::lock_guard<std::mutex> lock(mutex);
    if (instance == nullptr) {
        instance = new Pool();
    }
#endif
    return *instance;
}

}  // namespace

std::size_t threads() {
    auto n = g_threads.load(std::memory_order_relaxed);
    if (n == 0U) {
        n = default_threads();
        g_threads.store(n, std::memory_order_relaxed);
    }
    return n;
}

void set_threads(std::size_t n) { g_threads.store(n == 0U ? default_threads() : n, std::memory_order_relaxed); }

void for_each(std::size_t n, const std::function<void(std::size_t)>& task) {
    const auto width = std::min(threads(), n);
    if (width <= 1U) {
        for (std::size_t i = 0; i < n; ++i) {
            task(i);
        }
        return;
    }
    Job job;
    job.task = &task;
    job.n = n;
    job.max_helpers = width - 1U;
    job.limits = active_read_limits();
    job.errors.resize(n);
    auto& p = pool();
    p.ensure(threads() - 1U);
    p.run(job);
    for (auto& e : job.errors) {
        if (e) {
            std::rethrow_exception(e);
        }
    }
}

}  // namespace nano_lance::parallel
