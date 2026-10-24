// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The progress reporting of an index build (index/segment.hpp's stage): a stage starts with event 0 before
// its work runs and ends with event 2 only when the work succeeds, so a caller never sees a stage end that
// did not. With no progress callback the work runs and its result passes through unchanged.

#include "index/segment.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace ib = nano_lance::index_build;

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

struct Event {
    int event;
    std::string stage;
    std::uint64_t total;
    std::string unit;
    std::uint64_t completed;
};

}  // namespace

int main() {
    // Without a callback: the work runs, and its result (true or false) is what stage returns.
    {
        int runs = 0;
        require(ib::stage(nullptr, "scan", 10, "rows", [&] { ++runs; return true; }), "a successful stage is true");
        require(runs == 1, "the work runs once without a callback");
        require(!ib::stage(nullptr, "scan", 10, "rows", [&] { ++runs; return false; }), "a failed stage is false");
        require(runs == 2, "the failing work ran too");
    }

    // With a callback, success: start (0, the name, total, unit, 0) before the work, then end (2) after it.
    {
        std::vector<std::string> order;
        std::vector<Event> events;
        const ib::Progress progress = [&](int event, const char* stage, std::uint64_t total, const char* unit,
                                          std::uint64_t completed) {
            events.push_back({event, stage, total, unit, completed});
            order.push_back("event " + std::to_string(event));
        };
        const bool ok = ib::stage(&progress, "train", 2048, "vectors", [&] {
            order.push_back("work");
            return true;
        });
        require(ok, "the stage of a successful work is true");
        require((order == std::vector<std::string>{"event 0", "work", "event 2"}),
                "the start comes before the work, the end after it");
        require(events.size() == 2, "exactly two events");
        require(events[0].event == 0 && events[0].stage == "train" && events[0].total == 2048 &&
                    events[0].unit == "vectors" && events[0].completed == 0,
                "the start event names the stage, its total and unit, with nothing completed");
        require(events[1].event == 2 && events[1].stage == "train" && events[1].total == 0 &&
                    events[1].unit.empty() && events[1].completed == 0,
                "the end event names the stage, with nothing else");
    }

    // With a callback, failure: the start is reported, the end is not.
    {
        std::vector<int> seen;
        const ib::Progress progress = [&](int event, const char*, std::uint64_t, const char*, std::uint64_t) {
            seen.push_back(event);
        };
        const bool ok = ib::stage(&progress, "write", 0, "", [] { return false; });
        require(!ok, "the stage of a failed work is false");
        require((seen == std::vector<int>{0}), "a failed stage has a start and no end");
    }

    // Stages nest: an inner stage's events come inside the outer one's, each closed in turn.
    {
        std::vector<int> seen;
        const ib::Progress progress = [&](int event, const char*, std::uint64_t, const char*, std::uint64_t) {
            seen.push_back(event);
        };
        const bool ok = ib::stage(&progress, "outer", 0, "", [&] {
            return ib::stage(&progress, "inner", 0, "", [] { return true; });
        });
        require(ok, "nested stages succeed");
        require((seen == std::vector<int>{0, 0, 2, 2}), "nested stages report 0, 0, 2, 2");
    }

    std::cout << "index_segment: ok (stage events on success, failure, and without a callback)\n";
    return 0;
}
