// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The parallel for_each (include/nanolance/parallel.hpp) that every parallel read and write is built on:
// each index runs exactly once, a single thread runs on the calling thread, nested calls finish, an
// exception comes back to the caller (the first in index order, after the others ran), and a registered
// worker scope wraps the work pool threads do.

#include "nanolance/parallel.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::atomic<int> g_scope_calls{0};

// A worker scope as a layer above would register one: count the call, then run the work.
void counting_scope(const std::function<void()>& work) {
    g_scope_calls.fetch_add(1, std::memory_order_relaxed);
    work();
}

}  // namespace

int main() {
    int checks = 0;

    // threads() is at least one; set_threads() takes effect for later calls and 0 restores the default.
    {
        require(nano_lance::parallel::threads() >= 1, "threads() is at least 1");
        nano_lance::parallel::set_threads(4);
        require(nano_lance::parallel::threads() == 4, "set_threads(4) is reported by threads()");
        nano_lance::parallel::set_threads(0);
        require(nano_lance::parallel::threads() >= 1, "set_threads(0) restores a usable default");
        checks += 3;
    }

    // Every index runs exactly once: n = 0 runs nothing, n = 1 runs index 0, and many indices run each
    // once, with one thread and with four.
    for (const std::size_t width : {std::size_t{1}, std::size_t{4}}) {
        nano_lance::parallel::set_threads(width);

        int calls = 0;
        nano_lance::parallel::for_each(0, [&](std::size_t) { ++calls; });
        require(calls == 0, "n = 0 runs no task");

        std::vector<std::atomic<int>> hits(1);
        nano_lance::parallel::for_each(1, [&](std::size_t i) { hits[i].fetch_add(1); });
        require(hits[0].load() == 1, "n = 1 runs index 0 once");

        const std::size_t n = 5000;
        std::vector<std::atomic<int>> many(n);
        nano_lance::parallel::for_each(n, [&](std::size_t i) { many[i].fetch_add(1); });
        for (std::size_t i = 0; i < n; ++i) {
            require(many[i].load() == 1, "index " + std::to_string(i) + " ran exactly once");
        }
        checks += 3 + static_cast<int>(n);
    }

    // threads() == 1: no pool, every task runs on the calling thread, in index order.
    {
        nano_lance::parallel::set_threads(1);
        const auto caller = std::this_thread::get_id();
        std::vector<std::size_t> order;
        bool on_caller = true;
        nano_lance::parallel::for_each(50, [&](std::size_t i) {
            on_caller = on_caller && std::this_thread::get_id() == caller;
            order.push_back(i);
        });
        require(on_caller, "with one thread every task runs on the calling thread");
        require(order.size() == 50, "with one thread all 50 tasks run");
        for (std::size_t i = 0; i < order.size(); ++i) {
            require(order[i] == i, "with one thread tasks run in index order");
        }
        checks += 3;
    }

    // Nested for_each completes: each outer task starts an inner loop; all 64 cells are filled once.
    {
        nano_lance::parallel::set_threads(4);
        std::vector<std::atomic<int>> cells(64);
        nano_lance::parallel::for_each(8, [&](std::size_t i) {
            nano_lance::parallel::for_each(8, [&](std::size_t j) { cells[i * 8 + j].fetch_add(1); });
        });
        for (auto& c : cells) {
            require(c.load() == 1, "every nested cell ran exactly once");
        }
        checks += 1;
    }

    // An exception: every other task still runs, and the caller gets the exception of the lowest index
    // (7 here; 50 also throws).
    {
        nano_lance::parallel::set_threads(4);
        const std::size_t n = 100;
        std::vector<std::atomic<int>> ran(n);
        std::string caught;
        try {
            nano_lance::parallel::for_each(n, [&](std::size_t i) {
                ran[i].fetch_add(1);
                if (i == 7 || i == 50) {
                    throw std::runtime_error("task " + std::to_string(i));
                }
            });
        } catch (const std::runtime_error& e) {
            caught = e.what();
        }
        require(caught == "task 7", "the first exception in index order is rethrown (got '" + caught + "')");
        for (std::size_t i = 0; i < n; ++i) {
            require(ran[i].load() == 1, "every task ran even though some threw");
        }
        checks += 3;
    }

    // Exception with one thread: the same exception reaches the caller.
    // As with a pool, every task runs before the exception is rethrown (the plain loop used to stop at it).
    {
        nano_lance::parallel::set_threads(1);
        std::vector<std::atomic<int>> ran(10);
        std::string caught;
        try {
            nano_lance::parallel::for_each(10, [&](std::size_t i) {
                ran[i].fetch_add(1);
                if (i == 3) {
                    throw std::runtime_error("serial 3");
                }
            });
        } catch (const std::runtime_error& e) {
            caught = e.what();
        }
        require(caught == "serial 3", "a serial exception reaches the caller");
        int later = 0;
        for (std::size_t i = 4; i < 10; ++i) {
            later += ran[i].load();
        }
        require(later == 6, "with one thread, the tasks after a throwing one still run");
        checks += 2;
    }

    // A worker scope: once registered, pool threads run their share inside it, so it is called; with one
    // thread (no pool) it is never called; every task still runs.
    {
        nano_lance::parallel::set_threads(4);
        nano_lance::parallel::set_worker_scope(&counting_scope);
        g_scope_calls = 0;
        std::vector<std::atomic<int>> ran(400);
        nano_lance::parallel::for_each(400, [&](std::size_t i) {
            ran[i].fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        for (auto& r : ran) {
            require(r.load() == 1, "every task ran once inside the worker scope");
        }
        require(g_scope_calls.load() > 0, "pool threads ran their work inside the registered scope");

        nano_lance::parallel::set_worker_scope(nullptr);
        nano_lance::parallel::set_threads(1);
        g_scope_calls = 0;
        nano_lance::parallel::for_each(50, [](std::size_t) {});
        require(g_scope_calls.load() == 0, "with one thread no pool thread runs and the scope is not called");

        nano_lance::parallel::set_threads(4);
        nano_lance::parallel::for_each(400, [](std::size_t) {});
        require(g_scope_calls.load() == 0, "after set_worker_scope(nullptr) the scope is not called");
        checks += 4;
    }

    nano_lance::parallel::set_threads(0);
    std::cout << "parallel: " << checks << " checks passed\n";
    return 0;
}
