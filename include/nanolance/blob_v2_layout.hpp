// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/column_values.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nano_lance {

// On-disk packed layout of one external (kind=3) blob v2 descriptor row. All integers little-endian.
//
//   offset  size  field
//   ------  ----  -----------------------------------------------------------
//      0      4   prefix      = kBlobV2FixedDescriptorBytes + uri_len (record length minus this u32)
//      4      1   kind        = 3 (external reference)
//      5      8   position    byte offset of the blob inside the external object
//     13      8   size        blob length in bytes
//     21      4   blob_id     reserved; currently always 0
//     25      4   uri_len     length of the uri bytes that follow
//     29  uri_len uri         external object URI (e.g. "s3://bucket/key", "file:///path")
//
// Total record bytes = kBlobV2RecordHeaderBytes + uri_len.
// `prefix` deliberately excludes its own 4 bytes so it equals everything Lance counts as the record body.
constexpr std::uint32_t kBlobV2FixedDescriptorBytes = 25U;             // kind..uri_len, excludes leading prefix u32
constexpr std::size_t kBlobV2RecordHeaderBytes = 4U + kBlobV2FixedDescriptorBytes;  // prefix u32 + fixed fields = 29

/// Materialized external blob v2 descriptor (`kind = 3`) before Lance packed encoding.
struct BlobV2ExternalDescriptor {
    std::uint8_t kind = 3;
    std::uint64_t position = 0;
    std::uint64_t size = 0;
    std::uint32_t blob_id = 0;
    std::string blob_uri;
};

/// Lance's Blob v2 storage kinds (the descriptor's `kind`).
constexpr std::uint8_t kBlobKindInline = 0;     // in the data file, at `position`
constexpr std::uint8_t kBlobKindPacked = 1;     // in a sidecar file shared with other blobs, at `position`
constexpr std::uint8_t kBlobKindDedicated = 2;  // a sidecar file of its own
constexpr std::uint8_t kBlobKindExternal = 3;   // at `blob_uri`, `position`
/// Writer-side only, never on disk: a blob given as bytes, placed (inline, packed or dedicated) when
/// its data file is written. The row's trailing bytes -- a URI's place -- are the blob itself.
constexpr std::uint8_t kBlobKindPendingData = 0xFF;
/// Writer-side only, never on disk: a blob given as inline bytes by a descriptor (kind 0). Placed like
/// kBlobKindPendingData but always inline, whatever its size.
constexpr std::uint8_t kBlobKindPendingInline = 0xFE;

/// Pack one external descriptor into the Lance 2.2 blob v2 per-row record bytes.
std::vector<std::uint8_t> blob_v2_pack_descriptor_row(const BlobV2ExternalDescriptor& descriptor);

/// One row of a legacy (v1) blob column -- (position, size) of bytes in `out.blob_v2.data_file` --
/// appended as a descriptor row of kind inline, so every blob read shape serves it. `null` marks
/// the row null (format 2.1's reading of a non-zero position with no size).
void blob_append_legacy_row(ColumnValues& out, std::uint64_t position, std::uint64_t size, bool null);

/// Parse a Lance blob v2 control buffer into per-row packed sizes (`num_rows` must match the on-disk fragment row count).
bool blob_v2_control_buffer_to_row_sizes(const std::vector<std::uint8_t>& control, std::uint64_t num_rows,
                                           std::vector<std::uint32_t>& row_packed_sizes, std::string& error);

}  // namespace nano_lance
