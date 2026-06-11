#pragma once

// GPU device infrastructure for the nvexec (CUDA) bulk path. EVERYTHING here is behind
// NANOTINS_ENABLE_CUDA, so a normal CPU build (g++/clang, no CUDA) skips it entirely and the rest of the
// library is unaffected. When NANOTINS_ENABLE_CUDA is defined (a clang-cuda / nvcc build that links
// nvexec + cudart — see nanotins/docs/GPU_BULK_INTEGRATION.md), this provides:
//
//   - gpu::context           : owns an nvexec::stream_context; .scheduler() feeds nanotins::bulk_for_each
//                              UNCHANGED (the only difference vs the CPU pool is which scheduler you pass).
//   - gpu::device_buffer<T>  : RAII over cudaMalloc/cudaFree + H2D/D2H copies.
//   - gpu::free_vram_bytes() / gpu::vram_budget(...)  : size a per-window VRAM budget (absolute or %).
//
// The bulk KERNELS are identical to the CPU ones: bulk_for_each(gpu.scheduler(), tasks, n, kernel) runs
// the same lambda on the GPU (nvexec compiles it for device), provided the pointers it captures are
// DEVICE pointers (use device_buffer) and the per-element functions are NANOTINS_HD (overlay/parse_epb
// already are).

#include "nanotins/bulk.hpp"

#include <cstddef>
#include <cstdint>

#ifdef NANOTINS_ENABLE_CUDA

#include <cuda_runtime.h>
#include <nvexec/stream_context.cuh>
#include <stdexec/execution.hpp>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace nanotins::gpu {

inline void check_cuda(cudaError_t err, const char* what) {
    if (err != cudaSuccess) {
        std::fprintf(stderr, "nanotins gpu: CUDA error (%s): %s - %s\n", what, cudaGetErrorName(err),
                     cudaGetErrorString(err));
        std::abort();
    }
}

// Owns the GPU scheduler. Pass scheduler() straight into nanotins::bulk_for_each — same call as the CPU
// pool, the kernel is unchanged.
class context {
public:
    explicit context(int device = 0) { check_cuda(cudaSetDevice(device), "cudaSetDevice"); }
    auto scheduler() { return ctx_.get_scheduler(); }

private:
    nvexec::stream_context ctx_;
};

// RAII device array of T. T must be trivially copyable (POD) — BlockRef, EpbView, the SoA column elems,
// and raw bytes all are.
template <class T>
class device_buffer {
public:
    explicit device_buffer(std::size_t count) : count_(count) {
        if (count_ > 0) {
            check_cuda(cudaMalloc(&ptr_, count_ * sizeof(T)), "cudaMalloc");
        }
    }
    ~device_buffer() {
        if (ptr_) cudaFree(ptr_);
    }
    device_buffer(const device_buffer&) = delete;
    device_buffer& operator=(const device_buffer&) = delete;
    device_buffer(device_buffer&& o) noexcept : ptr_(o.ptr_), count_(o.count_) { o.ptr_ = nullptr; o.count_ = 0; }

    T* get() const { return ptr_; }
    std::size_t count() const { return count_; }

    void to_device(const T* host, std::size_t n) {
        check_cuda(cudaMemcpy(ptr_, host, n * sizeof(T), cudaMemcpyHostToDevice), "cudaMemcpy H2D");
    }
    void to_host(T* host, std::size_t n) const {
        check_cuda(cudaMemcpy(host, ptr_, n * sizeof(T), cudaMemcpyDeviceToHost), "cudaMemcpy D2H");
    }
    void zero() {
        if (ptr_) check_cuda(cudaMemset(ptr_, 0, count_ * sizeof(T)), "cudaMemset");
    }

private:
    T* ptr_ = nullptr;
    std::size_t count_ = 0;
};

// Free VRAM on the current device, in bytes.
inline std::uint64_t free_vram_bytes() {
    std::size_t free_b = 0, total_b = 0;
    check_cuda(cudaMemGetInfo(&free_b, &total_b), "cudaMemGetInfo");
    return static_cast<std::uint64_t>(free_b);
}

// A per-window VRAM budget: an explicit byte count if `bytes` > 0, else `pct` percent of free VRAM. The
// driver caps its window size to this so each window's (bytes + refs + output) device allocation fits.
inline std::uint64_t vram_budget(std::uint64_t bytes, unsigned pct) {
    if (bytes > 0) return bytes;
    const std::uint64_t free_b = free_vram_bytes();
    const unsigned p = pct == 0 ? 80 : (pct > 100 ? 100 : pct);
    return (free_b / 100) * p;
}

}  // namespace nanotins::gpu

#endif  // NANOTINS_ENABLE_CUDA
