#pragma once

#include "nanotins/pcap_blocks.hpp"
#include "nanotins/protocol_decode.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pcapng2lance::gpu_bridge {

struct context;
void destroy_context(context* ctx);
using context_ptr = std::unique_ptr<context, void (*)(context*)>;
context_ptr create_context(int device);
std::uint64_t vram_budget(std::uint64_t bytes, unsigned pct);

std::vector<pcapblocks::EpbView> parse_packets(context& gpu_ctx, pcapblocks::Bytes wbytes,
                                               const std::vector<pcapblocks::BlockRef>& packets,
                                               std::size_t threads);

void decode_window(context& gpu_ctx, std::size_t num_tasks, std::uint64_t pid_base,
                   const std::uint16_t* link_type, const std::uint64_t* poff, const std::uint32_t* psize,
                   pcapblocks::Bytes window, std::size_t n, protocols::DecodedPdus& out,
                   protocols::WalkResult* trailers);

}  // namespace pcapng2lance::gpu_bridge
