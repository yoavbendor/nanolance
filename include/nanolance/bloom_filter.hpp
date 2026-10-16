// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/// The split-block bloom filter (SBBF) of Parquet and Lance, bit for bit: the BLOOMFILTER scalar index's
/// filter (lance-core's utils/bloomfilter/sbbf.rs). Values hash with XXH64, seed 0, over their bytes.
namespace nano_lance::bloom_filter {

/// XXH64 of `n` bytes at `data`, seed 0.
std::uint64_t xxh64(const std::uint8_t* data, std::size_t n);

/// log2 of the filter's size in bytes for `ndv` items at false-positive probability `fpp`
/// (sbbf.rs's min_log2_bytes), clamped to Lance's 32 bytes .. 128 MiB.
std::uint8_t log2_bytes_for(std::uint64_t ndv, double fpp);

/// A filter of `num_bytes` (a power of two, at least 32) zeroed.
std::vector<std::uint8_t> empty_filter(std::size_t num_bytes);

/// Add the value with hash `hash` to `filter` (`num_bytes` long).
void insert(std::uint8_t* filter, std::size_t num_bytes, std::uint64_t hash);

/// False only when the value with hash `hash` was never inserted.
bool may_contain(const std::uint8_t* filter, std::size_t num_bytes, std::uint64_t hash);

}  // namespace nano_lance::bloom_filter
