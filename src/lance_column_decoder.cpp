#include "nanolance/lance_column_decoder.hpp"

#include "nanolance/blob_v2_external.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/schema_mapper.hpp"

#include <zstd.h>

#include <cstring>
#include <limits>

namespace nano_lance {
namespace {

bool field_metadata_is_true(const pb::Field& field, const char* key) {
    const auto it = field.metadata.find(key);
    if (it == field.metadata.end()) {
        return false;
    }
    static const std::vector<std::uint8_t> kTrue{'t', 'r', 'u', 'e'};
    return it->second == kTrue;
}

bool field_metadata_equals(const pb::Field& field, const char* key, const char* value) {
    const auto it = field.metadata.find(key);
    if (it == field.metadata.end()) {
        return false;
    }
    const std::string v(it->second.begin(), it->second.end());
    return v == value;
}

// Inverse of zstd_frame_buffer: [u64 LE uncompressed size][zstd frame] -> raw bytes.
bool zstd_unframe_buffer(const std::vector<std::uint8_t>& framed, std::vector<std::uint8_t>& out, std::string& error) {
    if (framed.size() < 8U) {
        error = "zstd frame shorter than size header";
        return false;
    }
    std::uint64_t uncompressed = 0;
    for (int i = 0; i < 8; ++i) {
        uncompressed |= static_cast<std::uint64_t>(framed[static_cast<std::size_t>(i)]) << (8 * i);
    }
    out.assign(uncompressed, 0U);
    const auto got = ZSTD_decompress(out.data(), uncompressed, framed.data() + 8U, framed.size() - 8U);
    if (ZSTD_isError(got) != 0U || got != uncompressed) {
        error = "zstd decompress failed for variable-width column";
        return false;
    }
    return true;
}

bool read_le16(const std::uint8_t* p, std::uint16_t& v) {
    v = static_cast<std::uint16_t>(static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8U));
    return true;
}

bool parse_miniblock_payload_chunks(const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& out,
                                  std::string& error) {
    out.clear();
    std::size_t offset = 0;
    while (offset < payload.size()) {
        if (offset + 8U > payload.size()) {
            error = "truncated miniblock payload header";
            return false;
        }
        if (payload[offset] != 0U || payload[offset + 1U] != 0U) {
            error = "unexpected miniblock payload prefix";
            return false;
        }
        std::uint16_t chunk_size = 0;
        if (!read_le16(payload.data() + offset + 2U, chunk_size)) {
            error = "failed to read miniblock chunk size";
            return false;
        }
        if (payload[offset + 6U] != 0xFEU || payload[offset + 7U] != 0xFEU) {
            error = "unexpected miniblock payload marker";
            return false;
        }
        const auto data_start = offset + 8U;
        const auto data_end = data_start + static_cast<std::size_t>(chunk_size);
        if (data_end > payload.size()) {
            error = "miniblock chunk exceeds payload";
            return false;
        }
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(data_start),
                   payload.begin() + static_cast<std::ptrdiff_t>(data_end));
        offset = data_end;
    }
    return true;
}

auto read_list_offset = [](const std::vector<std::uint8_t>& bytes, std::size_t index, bool large) -> std::int64_t {
    const auto offset_width = large ? 8U : 4U;
    const auto* src = bytes.data() + index * offset_width;
    if (large) {
        std::int64_t v = 0;
        std::memcpy(&v, src, sizeof(v));
        return v;
    }
    std::int32_t v = 0;
    std::memcpy(&v, src, sizeof(v));
    return static_cast<std::int64_t>(v);
};

void append_list_offset(std::vector<std::uint8_t>& out_offsets, std::int64_t value, bool large) {
    if (large) {
        const auto write_at = out_offsets.size();
        out_offsets.resize(write_at + 8U);
        std::memcpy(out_offsets.data() + write_at, &value, sizeof(value));
        return;
    }
    const auto write_at = out_offsets.size();
    out_offsets.resize(write_at + 4U);
    const auto stored = static_cast<std::int32_t>(value);
    std::memcpy(out_offsets.data() + write_at, &stored, sizeof(stored));
}

bool decode_variable_width_page(const std::vector<std::uint8_t>& chunk_bytes, const std::uint64_t num_values,
                                const bool large, std::vector<std::uint8_t>& out_offsets,
                                std::vector<std::uint8_t>& out_data, std::string& error) {
    if (num_values == 0U) {
        return true;
    }
    const auto offset_width = large ? 8U : 4U;
    const auto offsets_bytes = static_cast<std::size_t>(num_values + 1U) * offset_width;
    if (chunk_bytes.size() < offsets_bytes) {
        error = "variable-width chunk smaller than offset table";
        return false;
    }

    const auto data_base_in_chunk = read_list_offset(chunk_bytes, 0, large);
    if (data_base_in_chunk < 0 ||
        static_cast<std::size_t>(data_base_in_chunk) > chunk_bytes.size()) {
        error = "variable-width chunk data base out of range";
        return false;
    }

    if (out_offsets.empty()) {
        for (std::uint64_t i = 0; i <= num_values; ++i) {
            append_list_offset(out_offsets, read_list_offset(chunk_bytes, i, large) - data_base_in_chunk, large);
        }
        // Append only the value bytes [data_base, terminal_offset); the chunk is padded to 8 bytes at
        // the end, and including that padding would misalign every subsequent page's data.
        const auto data_end_in_chunk = read_list_offset(chunk_bytes, num_values, large);
        if (data_end_in_chunk < data_base_in_chunk ||
            static_cast<std::size_t>(data_end_in_chunk) > chunk_bytes.size()) {
            error = "variable-width chunk terminal offset out of range";
            return false;
        }
        out_data.insert(out_data.end(), chunk_bytes.begin() + static_cast<std::ptrdiff_t>(data_base_in_chunk),
                        chunk_bytes.begin() + static_cast<std::ptrdiff_t>(data_end_in_chunk));
        return true;
    }

    for (std::uint64_t i = 0; i < num_values; ++i) {
        const auto rel_start = read_list_offset(chunk_bytes, i, large) - data_base_in_chunk;
        const auto rel_end = read_list_offset(chunk_bytes, i + 1U, large) - data_base_in_chunk;
        if (rel_start < 0 || rel_end < rel_start || static_cast<std::size_t>(rel_end) > chunk_bytes.size()) {
            error = "variable-width chunk string bounds out of range";
            return false;
        }
        out_data.insert(out_data.end(),
                        chunk_bytes.begin() + static_cast<std::ptrdiff_t>(data_base_in_chunk + rel_start),
                        chunk_bytes.begin() + static_cast<std::ptrdiff_t>(data_base_in_chunk + rel_end));
        append_list_offset(out_offsets, static_cast<std::int64_t>(out_data.size()), large);
    }
    return true;
}

bool read_page_buffers(const std::filesystem::path& path, const pb::ColumnPage& page, bool blob_layout,
                       std::vector<std::uint8_t>& first, std::vector<std::uint8_t>& second, std::string& error) {
    if (page.buffer_offsets.size() < 2U || page.buffer_sizes.size() < 2U) {
        error = "column page is missing buffer offsets";
        return false;
    }
    const auto first_offset = page.buffer_offsets[blob_layout ? 1U : 0U];
    const auto first_size = page.buffer_sizes[blob_layout ? 1U : 0U];
    const auto second_offset = page.buffer_offsets[blob_layout ? 0U : 1U];
    const auto second_size = page.buffer_sizes[blob_layout ? 0U : 1U];
    if (!read_lance_data_file_bytes(path, first_offset, first_size, first, error)) {
        return false;
    }
    return read_lance_data_file_bytes(path, second_offset, second_size, second, error);
}

}  // namespace

bool decode_lance_physical_column(const std::filesystem::path& data_file_path, const pb::Field& on_disk_field,
                                  const pb::ColumnMetadata& column_metadata, ColumnValues& out, std::string& error) {
    error.clear();
    out = ColumnValues{};

    const bool blob_packed = field_metadata_is_true(on_disk_field, "lance-encoding:blob");
    if (blob_packed) {
        out.kind = ColumnValues::Kind::BlobV2External;
        for (const auto& page : column_metadata.pages) {
            std::vector<std::uint8_t> control;
            std::vector<std::uint8_t> values;
            if (!read_page_buffers(data_file_path, page, true, control, values, error)) {
                return false;
            }
            std::vector<std::uint32_t> row_sizes;
            if (!blob_v2_control_buffer_to_row_sizes(control, page.length, row_sizes, error)) {
                return false;
            }
            std::size_t offset = 0;
            for (const auto row_size : row_sizes) {
                if (offset + row_size > values.size()) {
                    error = "blob packed row exceeds values buffer";
                    return false;
                }
                out.blob_v2.packed_payload.insert(
                    out.blob_v2.packed_payload.end(),
                    values.begin() + static_cast<std::ptrdiff_t>(offset),
                    values.begin() + static_cast<std::ptrdiff_t>(offset + row_size));
                out.blob_v2.row_packed_sizes.push_back(row_size);
                offset += row_size;
            }
            if (offset != values.size()) {
                error = "blob values buffer has trailing bytes";
                return false;
            }
        }
        return true;
    }

    const bool variable = on_disk_field.encoding == 2;
    if (variable) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = on_disk_field.logical_type == "large_utf8" || on_disk_field.logical_type == "large_binary";
        const bool zstd = field_metadata_equals(on_disk_field, "lance-encoding:compression", "zstd");
        for (const auto& page : column_metadata.pages) {
            std::vector<std::uint8_t> control;
            std::vector<std::uint8_t> payload;
            if (!read_page_buffers(data_file_path, page, false, control, payload, error)) {
                return false;
            }
            std::vector<std::uint8_t> chunk_bytes;
            if (!parse_miniblock_payload_chunks(payload, chunk_bytes, error)) {
                return false;
            }
            if (zstd) {
                // One chunk per page in nanolance's writer, so the payload holds one [u64][zstd] frame.
                std::vector<std::uint8_t> raw;
                if (!zstd_unframe_buffer(chunk_bytes, raw, error)) {
                    return false;
                }
                chunk_bytes = std::move(raw);
            }
            if (!decode_variable_width_page(chunk_bytes, page.length, out.variable.large, out.variable.offsets,
                                          out.variable.data, error)) {
                return false;
            }
        }
        return true;
    }

    out.kind = ColumnValues::Kind::FixedWidth;
    std::string internal_type = on_disk_field.logical_type;
    if (internal_type == "string") {
        internal_type = "utf8";
    }
    const auto bytes_per_value = lance_logical_type_value_bytes(internal_type);
    for (const auto& page : column_metadata.pages) {
        std::vector<std::uint8_t> control;
        std::vector<std::uint8_t> payload;
        if (!read_page_buffers(data_file_path, page, false, control, payload, error)) {
            return false;
        }
        std::vector<std::uint8_t> chunk_bytes;
        if (!parse_miniblock_payload_chunks(payload, chunk_bytes, error)) {
            return false;
        }
        if (chunk_bytes.size() != page.length * bytes_per_value) {
            error = "fixed-width page byte count mismatch";
            return false;
        }
        out.fixed.insert(out.fixed.end(), chunk_bytes.begin(), chunk_bytes.end());
    }
    return true;
}

}  // namespace nano_lance
