// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/nano_lance_writer.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct ArrowArray;
struct ArrowSchema;

namespace nano_lance {

/// The writer's options, as a C++ type: same fields as `NanoLanceWriteOptions`, spelled the way a
/// C++ caller would expect.
///
/// The one difference is `structural_encoding`, which is positive here and defaults to `true`. The C
/// struct has to negate it so that a zeroed struct means the defaults; a C++ type has member
/// initializers and does not.
struct WriteOptions {
    /// zstd level 0..22; 0 means zstd's own default.
    int compression_level = 0;
    /// Open an existing dataset for more fragments rather than creating one.
    bool append = false;
    /// zstd-compress variable-width (string/binary) column pages.
    bool compression = false;
    /// Structural re-encodings: bitpacking, constant, RLE, dictionary. On by default.
    bool structural_encoding = true;
    /// URI-dictionary encoding for `lance.blob.v2` external columns (nanolance-only; create only).
    bool blob_uri_dictionary = false;
    /// Borrow the caller's fixed-width Arrow buffers until commit instead of copying them.
    bool borrow_buffers = false;
    /// Per-column encodings: "auto" / "plain" / "bitpack" / "bss-zstd" / "zstd".
    std::vector<std::pair<std::string, std::string>> column_encodings;
    /// Memory budget for uncommitted rows, in bytes; 0 = unlimited. write_batch commits a fragment
    /// itself whenever the buffered data reaches it (see nano_lance_writer_set_max_pending_bytes).
    std::uint64_t max_pending_bytes = 0;
};

/// A `NanoLanceWriter` that closes itself.
///
/// Every C++ caller in this repository was hand-rolling the same init / write / commit / close
/// sequence, which means every one of them leaks the writer's state if anything between init and
/// close throws or returns early -- `close()` was the caller's problem, and the compiler had no way
/// to help. This owns the handle instead.
///
/// Errors follow the house style, not exceptions: each call returns `bool` and leaves a message in
/// `error()`. What changes is only the lifetime.
///
///     nano_lance::Writer writer;
///     if (!writer.open("out.lance", {.compression_level = 3, .compression = true})) { ... }
///     if (!writer.write_batch(&array, &schema)) { ... }   // returning here still closes cleanly
///     if (!writer.commit()) { ... }
///
/// `commit()` defaults `is_append` to whatever the writer was opened with, since passing the wrong
/// one is an error the options already know the answer to.
class Writer {
  public:
    Writer() = default;
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&& other) noexcept;
    Writer& operator=(Writer&& other) noexcept;

    /// Open `path`. Closes an already-open writer first, so re-opening is not a leak.
    [[nodiscard]] bool open(const std::filesystem::path& path, const WriteOptions& options = {});

    [[nodiscard]] bool write_batch(ArrowArray* batch, ArrowSchema* schema);

    /// Commit the pending batches as a fragment. `is_append` defaults to the value `open()` was given.
    [[nodiscard]] bool commit();
    [[nodiscard]] bool commit(bool is_append);

    /// Close early. Called by the destructor; safe to call twice.
    bool close();

    [[nodiscard]] bool is_open() const { return handle_.private_data != nullptr; }
    [[nodiscard]] std::uint64_t pending_batches() const;
    /// Bytes held for uncommitted rows, as max_pending_bytes counts them.
    [[nodiscard]] std::uint64_t pending_bytes() const;

    /// Why the last call returned false.
    [[nodiscard]] const char* error() const { return handle_.last_error; }

    /// The status code the last call returned, for callers that distinguish them.
    [[nodiscard]] int status() const { return status_; }

    /// The underlying handle, for the parts of the C API this does not wrap.
    [[nodiscard]] NanoLanceWriter* handle() { return &handle_; }

  private:
    NanoLanceWriter handle_{};
    int status_ = NANO_LANCE_OK;
    bool append_ = false;
};

}  // namespace nano_lance
