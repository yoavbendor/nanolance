#pragma once

// GPU parse of a wire_spec into its SoA columns — the device twin of the CPU spec_soa fill. Same spec,
// same NANOTINS_HD read_field/scatter_spec; only the scheduler (nvexec), the pointers (device), and the
// H2D/D2H bracketing differ. Everything is behind NANOTINS_ENABLE_CUDA so the CPU build is untouched.
//
// This exists to VALIDATE, early and on the simplest protocols, that the wire_spec device primitives
// actually compile under clang-cuda/nvcc and produce GPU==CPU results. The key risk it exercises:
// scatter_spec captures a std::tuple of device pointers (soatins::soa_ptrs) BY VALUE into the nvexec bulk
// lambda and does std::get<I> inside the kernel — exactly the case GPU_BULK_INTEGRATION.md gotcha #2 warns
// about. If this compiles + matches CPU, the tuple-pack representation is good for everything built on
// spec_soa; if it chokes, the fix is a generated struct-of-named-pointers (and we learn it on a UDP header
// rather than a sensor).

#include "nanotins/wire_spec.hpp"
#include "nanotins/wire_spec_soa.hpp"

#ifdef NANOTINS_ENABLE_CUDA

#include "gputins/gpu.hpp"
#include "nanotins/bulk.hpp"

#include "soatins/reflect.hpp"  // soatins::soa_ptrs_h

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

namespace nanotins::gpu {

// Parse `n` fixed-stride PDUs from a host buffer on the GPU and D2H each column into `host_cols` (a
// soa_ptrs pack of host elem* per column, e.g. spec_soa<Spec,N>::raw()). `num_tasks` is the bulk grain.
template <class Spec, class HostPtrs>
void parse_spec_gpu(context& ctx, std::size_t num_tasks, const std::uint8_t* host_headers,
                    std::size_t stride, std::size_t n, HostPtrs host_cols) {
    using cols = nanotins::columns_of_spec<Spec>;
    constexpr std::size_t ncols = std::tuple_size_v<cols>;

    // H2D the headers.
    device_buffer<std::uint8_t> d_hdr(n * stride);
    d_hdr.to_device(host_headers, n * stride);

    // One device column buffer per spec column (heterogeneous element types).
    auto dev_cols = [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::tuple<device_buffer<typename std::tuple_element_t<I, cols>::elem>...>{
            device_buffer<typename std::tuple_element_t<I, cols>::elem>(n)...};
    }(std::make_index_sequence<ncols>{});

    // A TRIVIALLY-COPYABLE pack of the DEVICE column pointers. nvexec memcpys the kernel to the device, so
    // the captured pack must be trivially copyable — std::tuple (soa_ptrs) is not, a void*[N] POD is.
    nanotins::dev_ptr_pack<ncols> dptrs;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((dptrs.p[I] = static_cast<void*>(std::get<I>(dev_cols).get())), ...);
    }(std::make_index_sequence<ncols>{});

    // The kernel: read every field from the device header and scatter into the device columns. dptrs (a
    // POD void*[ncols]) is captured by value — the lambda is now trivially copyable for the GPU bulk.
    const std::uint8_t* dh = d_hdr.get();
    nanotins::bulk_for_each(ctx.scheduler(), num_tasks, n, [=](std::size_t i) {
        nanotins::scatter_spec_pod<Spec>(dptrs, i, dh + i * stride);
    });

    // D2H each column into the caller's host buffers.
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((std::get<I>(dev_cols).to_host(std::get<I>(host_cols), n)), ...);
    }(std::make_index_sequence<ncols>{});
}

}  // namespace nanotins::gpu

#endif  // NANOTINS_ENABLE_CUDA
