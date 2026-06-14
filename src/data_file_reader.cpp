// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/data_file_reader.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace nano_lance {
namespace {

bool read_le16(const unsigned char* p, std::uint16_t& v) {
    v = static_cast<std::uint16_t>(static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8U));
    return true;
}

bool read_le32(const unsigned char* p, std::uint32_t& v) {
    v = static_cast<std::uint32_t>(static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8U) |
                                   (static_cast<unsigned>(p[2]) << 16U) | (static_cast<unsigned>(p[3]) << 24U));
    return true;
}

bool read_le64(const unsigned char* p, std::uint64_t& v) {
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return true;
}

}  // namespace

bool read_lance_data_file_footer_and_descriptor(const std::filesystem::path& path, pb::FileDescriptor& descriptor,
                                                LanceDataFileFooterLayout& layout, std::string& error) {
    descriptor = pb::FileDescriptor{};
    layout = LanceDataFileFooterLayout{};
    error.clear();

    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size < 64U) {
        error = "data file is too small or unreadable";
        return false;
    }

    constexpr std::uint64_t kTailBytes = 512;
    const auto tail_len = static_cast<std::streamsize>(std::min(kTailBytes, file_size));
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "failed to open data file";
        return false;
    }
    in.seekg(static_cast<std::streamoff>(file_size - static_cast<std::uintmax_t>(tail_len)));
    std::vector<unsigned char> tail(static_cast<std::size_t>(tail_len));
    in.read(reinterpret_cast<char*>(tail.data()), tail_len);
    if (!in || in.gcount() != tail_len) {
        error = "failed to read data file footer tail";
        return false;
    }

    constexpr unsigned char kMagic[] = {'L', 'A', 'N', 'C'};
    std::size_t magic_idx = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i + 4U <= tail.size(); ++i) {
        if (std::memcmp(tail.data() + i, kMagic, 4) == 0) {
            magic_idx = i;
        }
    }
    if (magic_idx == static_cast<std::size_t>(-1) || magic_idx < 52U) {
        error = "LANC magic not found in data file tail";
        return false;
    }

    std::uint16_t minor = 0;
    std::uint16_t major = 0;
    if (!read_le16(tail.data() + magic_idx - 4U, minor) || !read_le16(tail.data() + magic_idx - 2U, major)) {
        error = "failed to read data file version";
        return false;
    }
    if (minor != 2U || major != 2U) {
        error = "unsupported Lance data file version (expected 2.2)";
        return false;
    }

    std::uint32_t num_columns = 0;
    std::uint32_t const1 = 0;
    if (!read_le32(tail.data() + magic_idx - 8U, num_columns) || !read_le32(tail.data() + magic_idx - 12U, const1)) {
        error = "failed to read data file column counts";
        return false;
    }
    if (const1 != 1U) {
        error = "unexpected Lance data file footer constant";
        return false;
    }

    const std::size_t u64_block = magic_idx - 52U;
    std::uint64_t global_buffer_offset = 0;
    std::uint64_t descriptor_size = 0;
    std::uint64_t column_metadata_start = 0;
    std::uint64_t column_offsets_start = 0;
    std::uint64_t global_offsets_start = 0;
    if (!read_le64(tail.data() + u64_block + 0U, global_buffer_offset) ||
        !read_le64(tail.data() + u64_block + 8U, descriptor_size) ||
        !read_le64(tail.data() + u64_block + 16U, column_metadata_start) ||
        !read_le64(tail.data() + u64_block + 24U, column_offsets_start) ||
        !read_le64(tail.data() + u64_block + 32U, global_offsets_start)) {
        error = "failed to read data file footer offset block";
        return false;
    }

    const std::uint64_t tail_base = file_size - static_cast<std::uint64_t>(tail_len);
    const std::uint64_t abs_u64_block = tail_base + u64_block;
    if (global_offsets_start != abs_u64_block) {
        error = "data file footer global_offsets_start mismatch";
        return false;
    }
    if (descriptor_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        global_buffer_offset > file_size || global_buffer_offset + descriptor_size > file_size) {
        error = "invalid data file descriptor bounds";
        return false;
    }

    in.clear();
    in.seekg(static_cast<std::streamoff>(global_buffer_offset));
    std::vector<std::uint8_t> desc_bytes(static_cast<std::size_t>(descriptor_size));
    in.read(reinterpret_cast<char*>(desc_bytes.data()), static_cast<std::streamsize>(descriptor_size));
    if (!in || static_cast<std::size_t>(in.gcount()) != static_cast<std::size_t>(descriptor_size)) {
        error = "failed to read data file descriptor bytes";
        return false;
    }
    if (!pb::decode_file_descriptor(desc_bytes, descriptor)) {
        error = "failed to decode data file protobuf descriptor";
        return false;
    }

    layout.global_buffer_offset = global_buffer_offset;
    layout.descriptor_size = descriptor_size;
    layout.column_metadata_start = column_metadata_start;
    layout.column_offsets_start = column_offsets_start;
    layout.global_offsets_start = global_offsets_start;
    layout.num_columns = num_columns;
    return true;
}

bool read_lance_data_file_bytes(const std::filesystem::path& path, const std::uint64_t offset,
                                const std::uint64_t size, std::vector<std::uint8_t>& out, std::string& error) {
    error.clear();
    out.clear();
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        error = "read size overflow";
        return false;
    }
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "failed to stat data file: " + ec.message();
        return false;
    }
    if (offset > file_size || offset + size > file_size) {
        error = "read range exceeds data file size";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "failed to open data file for read";
        return false;
    }
    in.seekg(static_cast<std::streamoff>(offset));
    out.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!in || static_cast<std::uint64_t>(in.gcount()) != size) {
        error = "failed to read data file byte range";
        out.clear();
        return false;
    }
    return true;
}

bool read_lance_data_file_column_metadatas(const std::filesystem::path& path,
                                           const LanceDataFileFooterLayout& layout,
                                           std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    error.clear();
    columns.clear();
    if (layout.num_columns == 0U) {
        return true;
    }
    const auto table_bytes = static_cast<std::uint64_t>(layout.num_columns) * 16U;
    std::vector<std::uint8_t> table;
    if (!read_lance_data_file_bytes(path, layout.column_offsets_start, table_bytes, table, error)) {
        return false;
    }
    columns.resize(layout.num_columns);
    for (std::uint32_t col = 0; col < layout.num_columns; ++col) {
        const auto base = static_cast<std::size_t>(col) * 16U;
        std::uint64_t meta_offset = 0;
        std::uint64_t meta_size = 0;
        if (!read_le64(table.data() + base, meta_offset) || !read_le64(table.data() + base + 8U, meta_size)) {
            error = "failed to parse column metadata offset table";
            return false;
        }
        std::vector<std::uint8_t> meta_bytes;
        if (!read_lance_data_file_bytes(path, meta_offset, meta_size, meta_bytes, error)) {
            return false;
        }
        if (!pb::decode_column_metadata(meta_bytes, columns[col])) {
            error = "failed to decode column metadata for column " + std::to_string(col);
            return false;
        }
    }
    return true;
}

}  // namespace nano_lance
