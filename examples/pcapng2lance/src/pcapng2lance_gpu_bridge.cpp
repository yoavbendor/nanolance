#include "pcapng2lance_gpu_bridge.hpp"

#include <algorithm>
#include <stdexcept>

#ifdef NANOTINS_ENABLE_CUDA

#include "nanotins/bulk.hpp"
#include "nanotins/gpu.hpp"
#include "nanotins/protocol_decode_gpu.hpp"

namespace pcapng2lance::gpu_bridge {

struct context {
    explicit context(int device) : gpu(device) {}
    nanotins::gpu::context gpu;
};

void destroy_context(context* ctx) {
    delete ctx;
}

context_ptr create_context(int device) {
    return context_ptr(new context(device), &destroy_context);
}

std::uint64_t vram_budget(std::uint64_t bytes, unsigned pct) {
    return nanotins::gpu::vram_budget(bytes, pct);
}

std::vector<pcapblocks::EpbView> parse_packets(context& gpu_ctx, pcapblocks::Bytes wbytes,
                                               const std::vector<pcapblocks::BlockRef>& packets,
                                               std::size_t threads) {
    const std::size_t n = packets.size();
    nanotins::gpu::device_buffer<std::uint8_t> d_win(wbytes.size());
    d_win.to_device(wbytes.data(), wbytes.size());
    nanotins::gpu::device_buffer<pcapblocks::BlockRef> d_refs(n);
    d_refs.to_device(packets.data(), n);
    nanotins::gpu::device_buffer<pcapblocks::EpbView> d_out(n);
    d_out.zero();

    const std::uint8_t* win = d_win.get();
    const std::size_t wsize = wbytes.size();
    const pcapblocks::BlockRef* pk = d_refs.get();
    pcapblocks::EpbView* out = d_out.get();
    const std::size_t num_tasks = std::min<std::size_t>(n, threads);
    nanotins::bulk_for_each(gpu_ctx.gpu.scheduler(), num_tasks, n, [=](std::size_t i) {
        pcapblocks::Bytes wb(win, wsize);  // span over DEVICE memory
        pcapblocks::EpbView v{};
        if (pcapblocks::parse_epb(wb, pk[i], v)) out[i] = v;
    });

    std::vector<pcapblocks::EpbView> parsed(n);
    d_out.to_host(parsed.data(), n);
    return parsed;
}

void decode_window(context& gpu_ctx, std::size_t num_tasks, std::uint64_t pid_base,
                   const std::uint16_t* link_type, const std::uint64_t* poff, const std::uint32_t* psize,
                   pcapblocks::Bytes window, std::size_t n, protocols::DecodedPdus& out,
                   protocols::WalkResult* trailers) {
    const protocols::Bytes win(window.data(), window.size());
    protocols::gpu::decode_window_gpu(gpu_ctx.gpu.scheduler(), num_tasks, pid_base, link_type, poff, psize,
                                      win, n, out, trailers);
}

}  // namespace pcapng2lance::gpu_bridge

#else

namespace pcapng2lance::gpu_bridge {

struct context {};

void destroy_context(context* ctx) {
    delete ctx;
}

context_ptr create_context(int /*device*/) {
    throw std::runtime_error(
        "pcapng2lance: --gpu requires a CUDA build (configure with -DNANOTINS_ENABLE_CUDA=ON)");
}

std::uint64_t vram_budget(std::uint64_t /*bytes*/, unsigned /*pct*/) {
    throw std::runtime_error(
        "pcapng2lance: --gpu requires a CUDA build (configure with -DNANOTINS_ENABLE_CUDA=ON)");
}

std::vector<pcapblocks::EpbView> parse_packets(context& /*gpu_ctx*/, pcapblocks::Bytes /*wbytes*/,
                                               const std::vector<pcapblocks::BlockRef>& /*packets*/,
                                               std::size_t /*threads*/) {
    throw std::runtime_error(
        "pcapng2lance: --gpu requires a CUDA build (configure with -DNANOTINS_ENABLE_CUDA=ON)");
}

void decode_window(context& /*gpu_ctx*/, std::size_t /*num_tasks*/, std::uint64_t /*pid_base*/,
                   const std::uint16_t* /*link_type*/, const std::uint64_t* /*poff*/,
                   const std::uint32_t* /*psize*/, pcapblocks::Bytes /*window*/, std::size_t /*n*/,
                   protocols::DecodedPdus& /*out*/, protocols::WalkResult* /*trailers*/) {
    throw std::runtime_error(
        "pcapng2lance: --gpu requires a CUDA build (configure with -DNANOTINS_ENABLE_CUDA=ON)");
}

}  // namespace pcapng2lance::gpu_bridge

#endif
