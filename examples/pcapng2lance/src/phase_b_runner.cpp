#include "phase_b_runner.hpp"

#include "nanotins/bulk.hpp"

#include <exec/static_thread_pool.hpp>

namespace pcapng2lance {

struct PhaseBRunner::Impl {
    explicit Impl(std::size_t threads) : pool(threads) {}
    exec::static_thread_pool pool;
};

PhaseBRunner::PhaseBRunner(std::size_t threads) : impl_(std::make_unique<Impl>(threads)) {}
PhaseBRunner::~PhaseBRunner() = default;
PhaseBRunner::PhaseBRunner(PhaseBRunner&&) noexcept = default;
PhaseBRunner& PhaseBRunner::operator=(PhaseBRunner&&) noexcept = default;

void PhaseBRunner::run(bool sequential, std::size_t num_tasks, std::size_t n,
                       const std::function<void(std::size_t)>& kernel) const {
    if (sequential) {
        nanotins::serial_for_each(num_tasks, n, kernel);
    } else {
        nanotins::bulk_for_each(impl_->pool.get_scheduler(), num_tasks, n, kernel);
    }
}

}  // namespace pcapng2lance
