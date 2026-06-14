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

/// Read a byte range from a Lance data file.
bool read_lance_data_file_bytes(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t size,
                                std::vector<std::uint8_t>& out, std::string& error);

}  // namespace nano_lance
