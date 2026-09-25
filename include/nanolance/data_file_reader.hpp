// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace nano_lance {

/// Parsed Lance 2.2 data file footer offsets (see `data_file_writer.cpp`).
struct LanceDataFileFooterLayout {
    std::uint64_t global_buffer_offset = 0;
    std::uint64_t descriptor_size = 0;
    std::uint64_t column_metadata_start = 0;
    std::uint64_t column_offsets_start = 0;
    std::uint64_t global_offsets_start = 0;
    std::uint32_t num_columns = 0;
};

/// Read tail footer, validate magic/version, decode protobuf `FileDescriptor` at `global_buffer_offset`.
bool read_lance_data_file_footer_and_descriptor(const std::filesystem::path& path, pb::FileDescriptor& descriptor,
                                                LanceDataFileFooterLayout& layout, std::string& error);

/// Read `num_columns` column metadata protobuf blobs using footer `column_offsets_start`.
bool read_lance_data_file_column_metadatas(const std::filesystem::path& path, const LanceDataFileFooterLayout& layout,
                                           std::vector<pb::ColumnMetadata>& columns, std::string& error);

/// One read operation's boundary for the reader's cache of open data files.
///
/// That cache re-stats a file's size and mtime to notice that the same path now names a *different*
/// file -- fragment names are assigned from the lowest unused suffix, so wiping a dataset directory
/// and rewriting it reproduces "fragment-0.lance" with different bytes (nanolance's own tests do
/// exactly this). Correct, but it was doing it on every page-buffer read: two `stat` syscalls per
/// page, 48% of a read's syscall time, to re-answer a question that cannot change inside one read.
///
/// Constructing this says "from here until scope exit is one read operation": the first lookup of a
/// given file still validates, and the rest of the operation's reads of that file skip the stats.
/// Outside any scope nothing is cached across calls -- every lookup validates, as before -- so the
/// guarantee is opt-in rather than something a caller can lose by forgetting.
///
/// Scopes nest, and a nested one is its own operation: leaving it restores the enclosing scope,
/// whose next lookup validates again.
class DataFileReadScope {
  public:
    DataFileReadScope();
    ~DataFileReadScope();
    DataFileReadScope(const DataFileReadScope&) = delete;
    DataFileReadScope& operator=(const DataFileReadScope&) = delete;
    DataFileReadScope(DataFileReadScope&&) = delete;
    DataFileReadScope& operator=(DataFileReadScope&&) = delete;

  private:
    std::uint64_t saved_ = 0;
};

/// Read a byte range from a Lance data file.
bool read_lance_data_file_bytes(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t size,
                                std::vector<std::uint8_t>& out, std::string& error);

}  // namespace nano_lance
