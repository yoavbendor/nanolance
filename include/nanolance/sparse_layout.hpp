// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/page_layout.hpp"
#include "nanolance/repdef.hpp"

#include <cstdint>
#include <string>
#include <vector>

/// Lance's sparse structural page layout (PageLayout field 5, file version 2.3).
///
/// A sparse page keeps the Arrow structure in layer-local slot domains instead of as repetition and
/// definition levels. Its buffers are: the chunk metadata (8 bytes per value chunk), the value buffer
/// (mini-block chunks with no levels), then the explicit position, count and validity sets of each
/// layer in the order the layers list them. This header turns those into the same per-layer offsets
/// and validity `repdef::unravel` produces for a level-encoded page, so the nested reader takes both.
///
/// Every function here is pure computation over untrusted bytes: each count and domain is checked
/// before it sizes anything, and a shape nanolance does not read is refused by name.
namespace nano_lance::sparse {

/// The structural buffers a layout needs after its two value buffers: one per explicit position set,
/// count set or validity set. Refuses a layout whose sets are malformed.
bool structural_buffer_count(const page_layout::SparseLayout& layout, std::size_t& count, std::string& error);

/// Decodes `count` u64 values stored with `encoding`, from `bytes`. Covers Flat(64), InlineBitpacking(64),
/// Bitpacked{64, Flat(w)} and General over one of those; anything else is refused.
bool decode_u64(const page_layout::Compressive& encoding, const std::vector<std::uint8_t>& bytes,
                std::uint64_t count, std::vector<std::uint64_t>& out, std::string& error);

/// The value chunks' item counts, from the metadata buffer: one 8-byte entry per chunk, a u32 chunk size
/// in 8-byte words minus one and a u32 value count. The counts must sum to `num_visible_items` and the
/// chunk sizes to `value_bytes`.
bool chunk_items(const std::vector<std::uint8_t>& meta, std::uint64_t value_bytes, std::uint64_t num_visible_items,
                 std::vector<std::uint64_t>& items, std::string& error);

/// Unravels one page. `buffers` holds the structural buffers in page order (the two value buffers are
/// not among them). `rows` is the page's row count and `leaf_nullable` whether the column's leaf field
/// is nullable: the innermost validity layer is then the leaf's own, otherwise it is a struct's.
///
/// On success `out` holds one entry per structural level in the order repdef::unravel uses: out[0] is
/// the leaf items (length num_visible_items, with the leaf's validity) and the last entry's length is
/// `rows`. Fixed-size-list layers are refused for now.
bool unravel(const page_layout::SparseLayout& layout, std::uint64_t rows, bool leaf_nullable,
             const std::vector<std::vector<std::uint8_t>>& buffers, std::vector<repdef::UnraveledLayer>& out,
             std::string& error);

/// A MiniBlock descriptor for the page's value chunks, as a PageLayout wrapper (the form
/// decode_page_layout reads). The value chunks are mini-block chunks with no levels, so the mini-block
/// reader and its value decoders read them as they are; their metadata is read by chunk_items.
std::vector<std::uint8_t> value_descriptor(const page_layout::SparseLayout& layout, std::uint64_t num_items);

}  // namespace nano_lance::sparse
