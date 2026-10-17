// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nano_lance::io {

/// Where a Lance file's bytes come from: a local file, or (registered by the embedder) an HTTP /
/// object-store source for a URI. A source reads ranges at absolute offsets and keeps no position,
/// so one source serves any reader in any order.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    /// The file's size, known when the source was opened.
    virtual std::uint64_t size() const = 0;
    /// Exactly `n` bytes at `offset` into `dst`; false and `error` on a short or failed read.
    virtual bool read_at(std::uint64_t offset, std::uint64_t n, std::uint8_t* dst, std::string& error) = 0;
    /// A token that changes when the file is replaced (a local file's mtime). Remote sources whose
    /// objects never change in place (Lance's data files do not) return 0.
    virtual std::int64_t version_token() const { return 0; }
    /// Reads smaller than this are served from one cached block of this size around them (a local
    /// file: 8 KiB, the stream buffer the reader had before; a remote source: larger, so many small
    /// reads become one request). 0 turns the block cache off.
    virtual std::uint64_t small_read_block() const { return 8U * 1024U; }
    /// How much of the file's end the footer reader fetches at once: the footer, the descriptor and
    /// often the column metadatas. Small locally (a read is cheap; a large tail costs copies on files
    /// of a few hundred KB); a remote source returns more, since every extra request is a round trip.
    virtual std::uint64_t footer_read_size() const { return 8U * 1024U; }
};

/// Opens a local file (POSIX `pread`, Windows `ReadFile` at an offset).
std::unique_ptr<ByteSource> open_local_file(const std::string& path, std::string& error);

/// A factory for sources of one URI scheme (`"https"`, `"s3"`, ...). `uri` is the whole URI.
using SourceFactory = std::function<std::unique_ptr<ByteSource>(const std::string& uri, std::string& error)>;

/// Registers (or, with an empty factory, removes) the factory for `scheme`. Paths without a
/// registered `scheme://` prefix are local files.
void register_source_factory(const std::string& scheme, SourceFactory factory);

/// Whether `path_or_uri` names a registered `scheme://` source rather than a local file.
bool is_remote(const std::string& path_or_uri);

/// Size and version token (as `version_token()` reports them) of a local file by path, without
/// opening it; false when it cannot be stat'ed.
bool stat_local(const std::string& path, std::uint64_t& size, std::int64_t& version);

/// Opens `path_or_uri`: through the factory registered for its scheme, else as a local file.
std::unique_ptr<ByteSource> open_source(const std::string& path_or_uri, std::string& error);

}  // namespace nano_lance::io
