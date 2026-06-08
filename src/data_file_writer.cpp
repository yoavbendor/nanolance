#include "nano_lance_writer/data_file_writer.hpp"

#include "lance_minimal.pb.hpp"
#include "nano_lance_writer/blob_v2_external.hpp"
#include "nano_lance_writer/schema_mapper.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nano_lance {
namespace {

void write_le16(std::ostream& out, std::uint16_t value) {
    const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU)};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_le32(std::ostream& out, std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xFFU),
        static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU),
        static_cast<char>((value >> 24U) & 0xFFU),
    };
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_le64(std::ostream& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        const auto byte = static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU);
        out.write(&byte, 1);
    }
}

std::uint64_t pos(std::ostream& out) {
    return static_cast<std::uint64_t>(out.tellp());
}

void align64(std::ostream& out) {
    const auto current = pos(out);
    const auto padding = (64U - (current % 64U)) % 64U;
    static constexpr std::array<char, 64> zeros{};
    out.write(zeros.data(), static_cast<std::streamsize>(padding));
}

std::uint32_t bits_per_value(const LanceField& field) {
    if (field.logical_type == "bool") {
        return 1;
    }
    if (field.logical_type == "int8" || field.logical_type == "uint8") {
        return 8;
    }
    if (field.logical_type == "int16" || field.logical_type == "uint16") {
        return 16;
    }
    if (field.logical_type == "int32" || field.logical_type == "uint32" || field.logical_type == "float") {
        return 32;
    }
    return 64;
}

std::size_t value_width_bytes(const LanceField& field) {
    const auto bits = bits_per_value(field);
    if (bits < 8U) {
        return 1U;
    }
    return bits / 8U;
}

constexpr std::uint32_t kMaxEightByteWordsPerMetadata = 4095U;
constexpr std::uint32_t kMaxUncompressedMiniblockBytes = 800U;

struct MiniblockChunk {
    std::vector<std::uint8_t> bytes;
    std::size_t value_count = 0;
};

void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void write_length_delimited(std::vector<std::uint8_t>& out, std::uint32_t field_number, const std::vector<std::uint8_t>& payload) {
    out.push_back(static_cast<std::uint8_t>((field_number << 3U) | 2U));
    append_varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

void write_string_field(std::vector<std::uint8_t>& out, std::uint32_t field_number, std::string_view value) {
    write_length_delimited(out, field_number, std::vector<std::uint8_t>(value.begin(), value.end()));
}

std::uint8_t flat_bits_per_value_token(const LanceField& field) {
    const auto bits = bits_per_value(field);
    if (bits == 64U) {
        return 0x40U;
    }
    if (bits == 8U) {
        return 0x08U;
    }
    return 0x20U;
}

std::vector<std::uint8_t> build_mini_block_layout(std::uint8_t bits_token, std::uint64_t num_items) {
    std::vector<std::uint8_t> mini;
    mini.push_back(0x1aU);
    mini.push_back(0x04U);
    mini.push_back(0x0aU);
    mini.push_back(0x02U);
    mini.push_back(0x08U);
    mini.push_back(bits_token);
    mini.push_back(0x32U);
    mini.push_back(0x01U);
    mini.push_back(0x01U);
    mini.push_back(0x38U);
    mini.push_back(0x01U);
    mini.push_back(0x48U);
    append_varint(mini, num_items);
    mini.push_back(0x50U);
    mini.push_back(0x01U);
    return mini;
}

void append_le16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

std::size_t max_values_per_metadata_chunk(std::size_t bytes_per_value) {
    const auto max_bytes = static_cast<std::size_t>(kMaxEightByteWordsPerMetadata) * 8U;
    return max_bytes / bytes_per_value;
}

std::size_t max_values_per_uncompressed_chunk(std::size_t bytes_per_value) {
    if (bytes_per_value == 0U) {
        return 1U;
    }
    const auto by_bytes = kMaxUncompressedMiniblockBytes / bytes_per_value;
    const auto by_metadata = max_values_per_metadata_chunk(bytes_per_value);
    return std::max<std::size_t>(1U, std::min(by_bytes, by_metadata));
}

bool build_miniblock_chunks(const std::vector<std::uint8_t>& values,
                            std::size_t bytes_per_value,
                            int compression_level,
                            std::vector<MiniblockChunk>& chunks,
                            std::string& error) {
    (void)compression_level;
    chunks.clear();
    if (bytes_per_value == 0U || values.empty()) {
        return true;
    }
    const auto total_values = values.size() / bytes_per_value;
    const auto max_chunk_values = max_values_per_uncompressed_chunk(bytes_per_value);
    std::size_t offset_values = 0;
    while (offset_values < total_values) {
        const auto remaining = total_values - offset_values;
        const auto chunk_values = std::min(remaining, max_chunk_values);
        const auto chunk_bytes = chunk_values * bytes_per_value;
        const auto* chunk_start = values.data() + offset_values * bytes_per_value;
        MiniblockChunk chunk;
        chunk.value_count = chunk_values;
        chunk.bytes.assign(chunk_start, chunk_start + static_cast<std::ptrdiff_t>(chunk_bytes));
        chunks.push_back(std::move(chunk));
        offset_values += chunk_values;
    }
    return true;
}

std::vector<std::uint8_t> control_buffer_for(const std::vector<MiniblockChunk>& chunks) {
    std::vector<std::uint8_t> out;
    out.reserve(chunks.size() * 2U);
    for (std::size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
        const auto& chunk = chunks[chunk_index];
        const auto words = static_cast<std::uint16_t>((chunk.bytes.size() + 7U) / 8U);
        std::uint16_t entry = static_cast<std::uint16_t>(words << 4U);
        const bool is_last = chunk_index + 1U == chunks.size();
        (void)chunk.value_count;
        entry |= 0U;
        append_le16(out, entry);
    }
    if (out.size() < 4U) {
        out.push_back(0U);
        out.push_back(0U);
    }
    return out;
}

std::vector<std::uint8_t> miniblock_payload(const std::vector<MiniblockChunk>& chunks) {
    std::vector<std::uint8_t> out;
    for (const auto& chunk : chunks) {
        out.push_back(0U);
        out.push_back(0U);
        append_le16(out, static_cast<std::uint16_t>(chunk.bytes.size()));
        out.push_back(0U);
        out.push_back(0U);
        out.push_back(0xFEU);
        out.push_back(0xFEU);
        out.insert(out.end(), chunk.bytes.begin(), chunk.bytes.end());
    }
    return out;
}

std::vector<std::uint8_t> bytes_from_hex(std::string_view hex) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2U);
    for (std::size_t i = 0; i + 1U < hex.size(); i += 2U) {
        const auto pair = std::string(hex.substr(i, 2U));
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16)));
    }
    return bytes;
}

std::vector<std::uint8_t> column_encoding_bytes() {
    return bytes_from_hex("0a1f2f6c616e63652e656e636f64696e67732e436f6c756d6e456e636f64696e6712020a00");
}

std::vector<std::uint8_t> variable_width_structural_payload(std::uint8_t bits_token, std::uint64_t rows) {
    const auto mini_block = build_mini_block_layout(bits_token, rows);
    if (mini_block.size() < 6U) {
        return mini_block;
    }
    // Matches IPC2Lance / Lance reference PageLayout for utf8/binary columns.
    std::vector<std::uint8_t> wrapped;
    wrapped.push_back(0x1aU);
    wrapped.push_back(0x08U);
    wrapped.push_back(0x12U);
    wrapped.push_back(0x06U);
    wrapped.push_back(0x0aU);
    wrapped.push_back(0x04U);
    wrapped.push_back(0x0aU);
    wrapped.push_back(0x02U);
    wrapped.push_back(0x08U);
    wrapped.push_back(bits_token);
    wrapped.insert(wrapped.end(), mini_block.begin() + 6, mini_block.end());
    return wrapped;
}

std::vector<std::uint8_t> page_layout_bytes(std::uint8_t bits_token, std::uint64_t rows, bool variable_width) {
    std::vector<std::uint8_t> page_layout;
    if (variable_width) {
        write_length_delimited(page_layout, 1, variable_width_structural_payload(bits_token, rows));
    } else {
        write_length_delimited(page_layout, 1, build_mini_block_layout(bits_token, rows));
    }

    std::vector<std::uint8_t> encoding;
    write_string_field(encoding, 1, "/lance.encodings21.PageLayout");
    write_length_delimited(encoding, 2, page_layout);
    return encoding;
}

std::vector<std::uint8_t> page_layout_bytes(const LanceField& field, std::uint64_t rows) {
    return page_layout_bytes(flat_bits_per_value_token(field), rows, false);
}

std::size_t padded_size(std::size_t size, std::size_t alignment) {
    return (size + alignment - 1U) & ~(alignment - 1U);
}

template <typename OffsetType>
bool read_offsets(const std::vector<std::uint8_t>& bytes, std::vector<OffsetType>& out, std::string& error) {
    if (bytes.size() % sizeof(OffsetType) != 0U) {
        error = "offset buffer size is not aligned";
        return false;
    }
    out.resize(bytes.size() / sizeof(OffsetType));
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

template <typename OffsetType>
bool build_variable_chunk_bytes(const std::vector<OffsetType>& offsets,
                                const std::vector<std::uint8_t>& data,
                                std::size_t first_value,
                                std::size_t last_value,
                                std::vector<std::uint8_t>& out) {
    const auto offset_width = sizeof(OffsetType);
    const auto num_values = last_value - first_value;
    const auto chunk_bytes = offsets[last_value] - offsets[first_value];
    const auto offsets_bytes = (num_values + 1U) * offset_width;
    const auto padded = padded_size(offsets_bytes + static_cast<std::size_t>(chunk_bytes), 8U);
    out.resize(padded, 0U);
    std::size_t write_offset = 0;
    for (std::size_t i = first_value; i <= last_value; ++i) {
        const auto relative = offsets[i] - offsets[first_value] + static_cast<OffsetType>(offsets_bytes);
        std::memcpy(out.data() + write_offset, &relative, offset_width);
        write_offset += offset_width;
    }
    const auto* chunk_start = data.data() + static_cast<std::size_t>(offsets[first_value]);
    std::memcpy(out.data() + offsets_bytes, chunk_start, static_cast<std::size_t>(chunk_bytes));
    return true;
}

template <typename OffsetType>
bool build_variable_chunks(const VariableWidthColumnValues& column,
                           std::vector<MiniblockChunk>& chunks,
                           std::string& error) {
    std::vector<OffsetType> offsets;
    if (!read_offsets(column.offsets, offsets, error)) {
        return false;
    }
    if (offsets.size() < 2U) {
        MiniblockChunk chunk;
        chunk.value_count = 0;
        chunks.push_back(std::move(chunk));
        return true;
    }
    const auto total_values = offsets.size() - 1U;
    std::size_t first_value = 0;
    while (first_value < total_values) {
        std::size_t last_value = first_value + 1U;
        while (last_value < total_values) {
            std::vector<std::uint8_t> probe;
            if (!build_variable_chunk_bytes(offsets, column.data, first_value, last_value + 1U, probe)) {
                break;
            }
            if (probe.size() > kMaxUncompressedMiniblockBytes) {
                break;
            }
            last_value++;
        }
        MiniblockChunk chunk;
        if (!build_variable_chunk_bytes(offsets, column.data, first_value, last_value, chunk.bytes)) {
            error = "failed to build variable-width miniblock chunk";
            return false;
        }
        chunk.value_count = last_value - first_value;
        chunks.push_back(std::move(chunk));
        first_value = last_value;
    }
    return true;
}

bool build_variable_chunks_for_column(const VariableWidthColumnValues& column,
                                      std::vector<MiniblockChunk>& chunks,
                                      std::string& error) {
    if (column.large) {
        return build_variable_chunks<std::int64_t>(column, chunks, error);
    }
    return build_variable_chunks<std::int32_t>(column, chunks, error);
}

}  // namespace

bool write_lance_data_file(const std::filesystem::path& dataset_path,
                           const std::string& file_name,
                           const LanceSchemaMapping& mapping,
                           const std::vector<ColumnValues>& column_values,
                           std::uint64_t rows,
                           int compression_level,
                           DataFileResult& result,
                           std::string& error) {
    error.clear();
    if (mapping.fields.empty()) {
        error = "cannot write Lance data file without mapped fields";
        return false;
    }
    const auto physical_fields = lance_physical_fields(mapping);
    if (column_values.size() != physical_fields.size()) {
        error = "column value count does not match physical schema field count";
        return false;
    }

    const auto relative_path = std::filesystem::path("data") / file_name;
    const auto full_path = dataset_path / relative_path;
    std::error_code ec;
    std::filesystem::create_directories(full_path.parent_path(), ec);
    if (ec) {
        error = "failed to create data directory: " + ec.message();
        return false;
    }

    std::ofstream out(full_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open data file for writing";
        return false;
    }

    std::vector<pb::ColumnMetadata> columns;
    columns.reserve(physical_fields.size());
    for (std::size_t field_index = 0; field_index < physical_fields.size(); ++field_index) {
        const auto& field = *physical_fields[field_index];
        const auto& values = column_values[field_index];
        if (values.kind == ColumnValues::Kind::BlobV2External) {
            const auto& blob = values.blob_v2;
            if (blob.row_packed_sizes.size() != static_cast<std::size_t>(rows)) {
                error = "blob v2 row count does not match fragment row count for ";
                error += field.name;
                return false;
            }
            if (blob.packed_payload.empty() && rows != 0U) {
                error = "blob v2 packed payload is empty for ";
                error += field.name;
                return false;
            }

            const auto control = blob_v2_build_control_buffer(blob.row_packed_sizes);

            align64(out);
            const auto values_offset = pos(out);
            out.write(reinterpret_cast<const char*>(blob.packed_payload.data()),
                      static_cast<std::streamsize>(blob.packed_payload.size()));

            align64(out);
            const auto control_offset = pos(out);
            out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));

            pb::ColumnMetadata column;
            column.encoding = column_encoding_bytes();
            pb::ColumnPage page;
            page.buffer_offsets.push_back(values_offset);
            page.buffer_offsets.push_back(control_offset);
            page.buffer_sizes.push_back(blob.packed_payload.size());
            page.buffer_sizes.push_back(control.size());
            page.length = rows;
            page.priority = 0;
            page.encoding = blob_v2_column_page_encoding();
            column.pages.push_back(std::move(page));
            columns.push_back(std::move(column));
            continue;
        }

        std::vector<MiniblockChunk> chunks;
        const bool is_variable = values.kind == ColumnValues::Kind::VariableWidth;
        if (is_variable) {
            if (!build_variable_chunks_for_column(values.variable, chunks, error)) {
                return false;
            }
        } else {
            const auto bytes_per_value = value_width_bytes(field);
            if (values.fixed.size() % bytes_per_value != 0U) {
                error = "column value buffer size is not aligned to field width for ";
                error += field.name;
                return false;
            }
            if (values.fixed.size() / bytes_per_value != static_cast<std::size_t>(rows)) {
                error = "column value count does not match row count for ";
                error += field.name;
                return false;
            }
            if (!build_miniblock_chunks(values.fixed, bytes_per_value, compression_level, chunks, error)) {
                return false;
            }
        }

        pb::ColumnMetadata column;
        column.encoding = column_encoding_bytes();
        for (const auto& chunk : chunks) {
            const std::vector<MiniblockChunk> single_chunk{chunk};
            const auto control = control_buffer_for(single_chunk);
            const auto payload = miniblock_payload(single_chunk);

            align64(out);
            const auto control_offset = pos(out);
            out.write(reinterpret_cast<const char*>(control.data()), static_cast<std::streamsize>(control.size()));

            align64(out);
            const auto payload_offset = pos(out);
            out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));

            pb::ColumnPage page;
            page.buffer_offsets.push_back(control_offset);
            page.buffer_offsets.push_back(payload_offset);
            page.buffer_sizes.push_back(control.size());
            page.buffer_sizes.push_back(payload.size());
            page.length = chunk.value_count;
            page.priority = 0;
            if (is_variable) {
                const auto bits_token =
                    values.variable.large ? static_cast<std::uint8_t>(0x40U) : static_cast<std::uint8_t>(0x20U);
                page.encoding = page_layout_bytes(bits_token, chunk.value_count, true);
            } else {
                page.encoding = page_layout_bytes(field, chunk.value_count);
            }
            column.pages.push_back(std::move(page));
        }
        columns.push_back(std::move(column));
    }

    pb::FileDescriptor descriptor;
    descriptor.length = rows;
    for (const auto& mapped_field : mapping.fields) {
        pb::Field field;
        field.name = mapped_field.name;
        field.logical_type = lance_on_disk_logical_type(mapped_field.logical_type);
        field.id = mapped_field.id;
        field.parent_id = mapped_field.parent_id;
        field.type = 2;
        field.nullable = mapped_field.nullable;
        field.encoding = lance_on_disk_field_encoding(mapped_field.logical_type);
        for (const auto& kv : mapped_field.metadata) {
            field.metadata[kv.first] = std::vector<std::uint8_t>(kv.second.begin(), kv.second.end());
        }
        descriptor.fields.push_back(std::move(field));
    }
    const auto descriptor_bytes = pb::encode_file_descriptor(descriptor);
    align64(out);
    const auto global_buffer_offset = pos(out);
    out.write(reinterpret_cast<const char*>(descriptor_bytes.data()), static_cast<std::streamsize>(descriptor_bytes.size()));

    align64(out);
    const auto column_metadata_start = pos(out);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> column_metadata_offsets;
    for (const auto& column : columns) {
        const auto encoded = pb::encode_column_metadata(column);
        const auto offset = pos(out);
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        column_metadata_offsets.emplace_back(offset, encoded.size());
    }

    align64(out);
    const auto column_offsets_start = pos(out);
    for (const auto& [offset, size] : column_metadata_offsets) {
        write_le64(out, offset);
        write_le64(out, size);
    }

    const auto global_offsets_start = pos(out);
    write_le64(out, global_buffer_offset);
    write_le64(out, descriptor_bytes.size());

    write_le64(out, column_metadata_start);
    write_le64(out, column_offsets_start);
    write_le64(out, global_offsets_start);
    write_le32(out, 1);
    write_le32(out, static_cast<std::uint32_t>(columns.size()));
    write_le16(out, 2);
    write_le16(out, 2);
    out.write("LANC", 4);
    out.flush();
    if (!out) {
        error = "failed to write Lance data file";
        return false;
    }

    result.relative_path = relative_path;
    result.file_size_bytes = std::filesystem::file_size(full_path, ec);
    if (ec) {
        error = "failed to stat data file: " + ec.message();
        return false;
    }
    return true;
}

}  // namespace nano_lance
