// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
#pragma once

#include "nanolance/byte_sink.hpp"

#include <memory>
#include <string>

namespace nanos3reader {
class S3Client;
}

namespace nano_lance::io {

// The `s3` scheme's write backend (src/s3_byte_sink.cpp, built when nanos3reader is). byte_sink.cpp
// registers it for "s3" the first time its registry is used.
SinkBackend s3_sink_backend();

/// The client for `uri`'s bucket with its current storage options (src/s3_byte_source.cpp), or null with
/// `error` for a URI without a bucket.
std::shared_ptr<nanos3reader::S3Client> s3_client_for_uri(const std::string& uri, std::string& error);

}  // namespace nano_lance::io
