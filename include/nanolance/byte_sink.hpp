// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace nano_lance::io {

// Where the bytes nanolance writes go: a local file, or (for a registered scheme, `s3` when nanolance is
// built with nanos3reader) an object store. The write-side twin of byte_source.hpp; the design is in
// docs/OBJECT_STORE_WRITES.md.

/// One file or object being written, as an `std::ostream` (tellp() counts the bytes written). A local
/// file is written in place; an object becomes visible only when commit() succeeds, and an output
/// destroyed without commit() leaves no object behind.
class OutputFile : public std::ostream {
public:
    explicit OutputFile(std::streambuf* buf) : std::ostream(buf) {}
    ~OutputFile() override = default;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    /// Flush everything and make the file / object exist. False (and `error`) when any write failed.
    virtual bool commit(std::string& error) = 0;
};

/// Opens `path_or_uri` for writing: through the backend registered for its scheme, else as a local file
/// (its parent directories created).
std::unique_ptr<OutputFile> open_output(const std::string& path_or_uri, std::string& error);

/// Creates `path_or_uri` holding `bytes` only if nothing exists there: the publish step of a commit.
/// `already_exists` is set (and false returned) when another writer got there first. Local: a temporary
/// file hard-linked into place (rename where links are not supported). Object store: a create-only PUT
/// (If-None-Match: *).
bool put_if_absent(const std::string& path_or_uri, const std::vector<std::uint8_t>& bytes, bool& already_exists,
                   std::string& error);

/// Removes one file / object; a missing one is not an error.
bool remove_output(const std::string& path_or_uri, std::string& error);

/// Called with each local path remove_output removes. The data-file reader registers one that closes its
/// cached handles on the file (it lives in the library above this one, so it registers rather than is called).
void set_output_removed_hook(void (*hook)(const std::string& path));

/// Whether `path_or_uri` is written through a registered backend (an object store) rather than as a
/// local file. Such datasets have no directories: creating one is a no-op.
bool is_object_store_output(const std::string& path_or_uri);

/// A writable object-store scheme.
struct SinkBackend {
    std::function<std::unique_ptr<OutputFile>(const std::string& uri, std::string& error)> open;
    std::function<bool(const std::string& uri, const std::vector<std::uint8_t>& bytes, bool& already_exists,
                       std::string& error)>
        put_if_absent;
    std::function<bool(const std::string& uri, std::string& error)> remove;
};

/// Registers (or, with an empty `open`, removes) the backend for `scheme`.
void register_sink_backend(const std::string& scheme, SinkBackend backend);

}  // namespace nano_lance::io
