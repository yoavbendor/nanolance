// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nano_lance {

/// The bytes of one data file, as the page decoders need them (Lance's EncodingsIo): the file layer
/// implements it over a data file (DataFileIo in data_file_reader.hpp).
class EncodingsIo {
public:
    virtual ~EncodingsIo() = default;
    /// `size` bytes at `offset` into `out`.
    virtual bool read(std::uint64_t offset, std::uint64_t size, std::vector<std::uint8_t>& out,
                      std::string& error) const = 0;
    /// The file's size and version token (its mtime for a local file), for caches keyed on the file.
    virtual bool identity(std::uint64_t& size, std::int64_t& version) const = 0;
    /// The file's name (its path), for cache keys and messages.
    virtual const std::string& name() const = 0;
};

}  // namespace nano_lance
