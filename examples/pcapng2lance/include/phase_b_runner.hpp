// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace pcapng2lance {

class PhaseBRunner {
public:
    explicit PhaseBRunner(std::size_t threads);
    ~PhaseBRunner();
    PhaseBRunner(PhaseBRunner&&) noexcept;
    PhaseBRunner& operator=(PhaseBRunner&&) noexcept;
    PhaseBRunner(const PhaseBRunner&) = delete;
    PhaseBRunner& operator=(const PhaseBRunner&) = delete;

    void run(bool sequential, std::size_t num_tasks, std::size_t n,
             const std::function<void(std::size_t)>& kernel) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pcapng2lance
