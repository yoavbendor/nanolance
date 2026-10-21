// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
#pragma once

#include "nanolance/byte_source.hpp"

#include <memory>
#include <string>
#include <vector>

namespace nano_lance::io {

// The `s3` scheme's factory and lister (src/s3_byte_source.cpp, built when nanos3reader is). byte_source.cpp
// registers them for "s3" the first time its registry is used.

/// Opens `s3://bucket/key`: a HEAD for the size, then exact range reads through nanos3reader's S3Client.
std::unique_ptr<ByteSource> open_s3_source(const std::string& uri, std::string& error);

/// The immediate entries under `dir_uri` (a ListObjectsV2 with delimiter "/"; every page).
bool list_s3(const std::string& dir_uri, std::vector<DirEntry>& out, std::string& error);

}  // namespace nano_lance::io
