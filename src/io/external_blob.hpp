// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nano_lance::io {

/// How an external-blob call ended. The C API (nano_lance_reader.h) maps it to its return codes.
enum class BlobStatus { Ok, InvalidArgument, IoError, Unsupported };

/// Reads up to `size` bytes (all of `out_cap` when `size` is UINT64_MAX) of the object at `uri`
/// (file:// or, with S3, s3://) from `position` into `out_buf`; `bytes_read` is what was read. Open objects
/// are kept per thread (a few, most recently used first). A file:// path may not contain "..", and must lie
/// under NANO_LANCE_BLOB_BASE_DIR when that is set.
BlobStatus fetch_external_blob(const std::string& uri, std::uint64_t position, std::uint64_t size,
                               std::uint8_t* out_buf, std::size_t out_cap, std::size_t& bytes_read,
                               std::string& error);

/// The size of the object at `uri`, under the same rules.
BlobStatus external_blob_size(const std::string& uri, std::uint64_t& size, std::string& error);

/// The S3 reader's on-disk block cache: `max_blocks` > 0 turns it on in `cache_dir`, 0 turns it off.
/// Unsupported when this build has no cache (no S3, or the AWS SDK provider).
BlobStatus configure_block_cache(const char* cache_dir, int max_blocks, std::string& error);
void block_cache_stats(std::uint64_t* hits, std::uint64_t* misses);

}  // namespace nano_lance::io
