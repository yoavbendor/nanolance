#pragma once

#include "nanotins/pcap_blocks.hpp"
#include "nanotins/protocol_decode.hpp"  // protocols::WalkResult
#include "nanotins/dag_decode.hpp"       // nanotins::dag_tables
#include "nanotins/spec_dag.hpp"         // nanotins::L2L3Graph

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

// Decode one window into the per-node DAG tables (the spec/DAG path; byte-identical to the CPU
// dag_decode_window). out == nanotins::dag_tables<L2L3Graph>; trailers (size n) gets each packet's L4
// boundary for the remainder rows. CUDA impl lives behind NANOTINS_ENABLE_CUDA in the .cpp.
void decode_window(context& gpu_ctx, std::size_t num_tasks, std::uint64_t pid_base,
                   const std::uint16_t* link_type, const std::uint64_t* poff, const std::uint32_t* psize,
                   pcapblocks::Bytes window, std::size_t n,
                   nanotins::dag_tables<nanotins::L2L3Graph>& out, protocols::WalkResult* trailers);

}  // namespace pcapng2lance::gpu_bridge
