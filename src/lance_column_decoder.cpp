#include "nanolance/lance_column_decoder.hpp"

#include "nanolance/blob_v2_external.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/fastlanes_bitpack.hpp"
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

const std::vector<std::uint8_t>* field_metadata_bytes(const pb::Field& field, const char* key) {
    const auto it = field.metadata.find(key);
    return it == field.metadata.end() ? nullptr : &it->second;
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

// Decode one bitpacked page chunk ([bit_width word][FastLanes packed 1024]) into `num_values`
// little-endian fixed-width values appended to out_fixed.
template <class T>
bool unpack_bitpacked_page(const std::vector<std::uint8_t>& chunk, std::uint64_t num_values,
                           std::vector<std::uint8_t>& out_fixed, std::string& error) {
    if (chunk.size() < sizeof(T)) {
        error = "bitpacked chunk shorter than width header";
        return false;
    }
    T width_word = 0;
    std::memcpy(&width_word, chunk.data(), sizeof(T));
    const unsigned width = static_cast<unsigned>(width_word);
    if (width > sizeof(T) * 8U) {
        error = "bitpacked chunk has invalid bit width";
        return false;
    }
    const auto packed_words = nano_lance::fastlanes::packed_words_1024<T>(width);
    if (chunk.size() != sizeof(T) * (1U + packed_words)) {
        error = "bitpacked chunk size does not match bit width";
        return false;
    }
    std::vector<T> packed(packed_words, 0);
    if (packed_words != 0U) {
        std::memcpy(packed.data(), chunk.data() + sizeof(T), packed_words * sizeof(T));
    }
    T values[1024];
    nano_lance::fastlanes::unpack_1024<T>(width, packed.data(), values);
    const auto bytes = static_cast<std::size_t>(num_values) * sizeof(T);
    const auto* p = reinterpret_cast<const std::uint8_t*>(values);
    out_fixed.insert(out_fixed.end(), p, p + bytes);
    return true;
}

bool unpack_bitpacked_page_dispatch(const std::vector<std::uint8_t>& chunk, std::uint64_t num_values,
                                    std::size_t bytes_per_value, std::vector<std::uint8_t>& out_fixed,
                                    std::string& error) {
    switch (bytes_per_value) {
        case 1U:
            return unpack_bitpacked_page<std::uint8_t>(chunk, num_values, out_fixed, error);
        case 2U:
            return unpack_bitpacked_page<std::uint16_t>(chunk, num_values, out_fixed, error);
        case 4U:
            return unpack_bitpacked_page<std::uint32_t>(chunk, num_values, out_fixed, error);
        default:
            return unpack_bitpacked_page<std::uint64_t>(chunk, num_values, out_fixed, error);
    }
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

    // Constant column: the single value is stored once in field metadata; expand to one value per row.
    if (field_metadata_equals(on_disk_field, "nanolance:packing", "constant")) {
        const auto* value = field_metadata_bytes(on_disk_field, "nanolance:const-value");
        if (value == nullptr) {
            error = "constant column missing nanolance:const-value";
            return false;
        }
        std::uint64_t total_rows = 0;
        for (const auto& page : column_metadata.pages) {
            total_rows += page.length;
        }
        if (on_disk_field.encoding == 2) {  // variable-width
            out.kind = ColumnValues::Kind::VariableWidth;
            out.variable.large = on_disk_field.logical_type == "large_utf8" ||
                                 on_disk_field.logical_type == "large_binary";
            const auto ow = out.variable.large ? 8U : 4U;
            out.variable.data.reserve(static_cast<std::size_t>(total_rows) * value->size());
            out.variable.offsets.reserve((static_cast<std::size_t>(total_rows) + 1U) * ow);
            std::uint64_t cumulative = 0;
            auto push_offset = [&](std::uint64_t v) {
                if (out.variable.large) {
                    out.variable.offsets.insert(out.variable.offsets.end(),
                                                reinterpret_cast<const std::uint8_t*>(&v),
                                                reinterpret_cast<const std::uint8_t*>(&v) + 8);
                } else {
                    const auto v32 = static_cast<std::uint32_t>(v);
                    out.variable.offsets.insert(out.variable.offsets.end(),
                                                reinterpret_cast<const std::uint8_t*>(&v32),
                                                reinterpret_cast<const std::uint8_t*>(&v32) + 4);
                }
            };
            push_offset(0);
            for (std::uint64_t i = 0; i < total_rows; ++i) {
                out.variable.data.insert(out.variable.data.end(), value->begin(), value->end());
                cumulative += value->size();
                push_offset(cumulative);
            }
            return true;
        }
        out.kind = ColumnValues::Kind::FixedWidth;
        out.fixed.reserve(static_cast<std::size_t>(total_rows) * value->size());
        for (std::uint64_t i = 0; i < total_rows; ++i) {
            out.fixed.insert(out.fixed.end(), value->begin(), value->end());
        }
        return true;
    }

    // Run-length encoded fixed-width column: one chunk with two buffers (run values + run lengths).
    if (field_metadata_equals(on_disk_field, "nanolance:packing", "rle")) {
        out.kind = ColumnValues::Kind::FixedWidth;
        std::string internal = on_disk_field.logical_type;
        if (internal == "string") {
            internal = "utf8";
        }
        const auto bpv = lance_logical_type_value_bytes(internal);
        const std::size_t length_bytes = 1U;  // Lance RLE uses 8-bit run lengths
        for (const auto& page : column_metadata.pages) {
            std::vector<std::uint8_t> control;
            std::vector<std::uint8_t> data;
            if (!read_page_buffers(data_file_path, page, false, control, data, error)) {
                return false;
            }
            if (data.size() < 10U) {
                error = "rle chunk too short";
                return false;
            }
            std::uint32_t size0 = 0;
            std::uint32_t size1 = 0;
            std::memcpy(&size0, data.data() + 2U, 4U);
            std::memcpy(&size1, data.data() + 6U, 4U);
            std::size_t off = 10U;
            off += (8U - (off % 8U)) % 8U;  // pad to 8 after the [num_levels][size0][size1] header
            const std::size_t values_off = off;
            std::size_t lengths_off = values_off + size0;
            lengths_off += (8U - (lengths_off % 8U)) % 8U;
            if (lengths_off + size1 > data.size() || size0 % bpv != 0U || length_bytes == 0U ||
                size1 % length_bytes != 0U) {
                error = "rle chunk buffer sizes invalid";
                return false;
            }
            const std::size_t num_runs = size0 / bpv;
            if (num_runs != size1 / length_bytes) {
                error = "rle run count mismatch between values and lengths";
                return false;
            }
            auto run_length_at = [&](std::size_t r) -> std::uint64_t {
                std::uint64_t run = 0;
                for (std::size_t k = 0; k < length_bytes; ++k) {
                    run |= static_cast<std::uint64_t>(data[lengths_off + r * length_bytes + k]) << (8U * k);
                }
                return run;
            };
            std::size_t total_vals = 0;
            for (std::size_t r = 0; r < num_runs; ++r) {
                total_vals += run_length_at(r);
            }
            out.fixed.reserve(out.fixed.size() + total_vals * bpv);  // avoid per-row reallocation
            for (std::size_t r = 0; r < num_runs; ++r) {
                const std::uint64_t run = run_length_at(r);
                const auto* vptr = data.data() + values_off + r * bpv;
                for (std::uint64_t c = 0; c < run; ++c) {
                    out.fixed.insert(out.fixed.end(), vptr, vptr + bpv);
                }
            }
        }
        return true;
    }

    // Dictionary + RLE variable-width column: buffer[1] = RLE'd u32 indices, buffer[2] = dictionary.
    if (field_metadata_equals(on_disk_field, "nanolance:packing", "dict-rle")) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = on_disk_field.logical_type == "large_utf8" || on_disk_field.logical_type == "large_binary";
        const auto ow = out.variable.large ? 8U : 4U;
        for (const auto& page : column_metadata.pages) {
            if (page.buffer_offsets.size() < 3U || page.buffer_sizes.size() < 3U) {
                error = "dict-rle page missing buffers";
                return false;
            }
            std::vector<std::uint8_t> data;       // buffer[1]: RLE chunk of indices
            std::vector<std::uint8_t> dict_frame;  // buffer[2]: dictionary
            if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[1], page.buffer_sizes[1], data, error) ||
                !read_lance_data_file_bytes(data_file_path, page.buffer_offsets[2], page.buffer_sizes[2], dict_frame,
                                            error)) {
                return false;
            }
            // Decode the dictionary: un-zstd -> [u32 32][u32 bytes_start][u32 offsets][data].
            std::vector<std::uint8_t> dict_block;
            if (!zstd_unframe_buffer(dict_frame, dict_block, error)) {
                return false;
            }
            if (dict_block.size() < 8U) {
                error = "dict block too short";
                return false;
            }
            std::uint32_t bytes_start = 0;
            std::memcpy(&bytes_start, dict_block.data() + 4U, 4U);
            if (bytes_start < 12U || bytes_start > dict_block.size() || (bytes_start - 8U) % 4U != 0U) {
                error = "dict block header invalid";
                return false;
            }
            const std::size_t num_dict = (bytes_start - 8U) / 4U - 1U;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> dict_ranges(num_dict);
            for (std::size_t d = 0; d < num_dict; ++d) {
                std::uint32_t a = 0;
                std::uint32_t b = 0;
                std::memcpy(&a, dict_block.data() + 8U + d * 4U, 4U);
                std::memcpy(&b, dict_block.data() + 8U + (d + 1U) * 4U, 4U);
                if (bytes_start + b > dict_block.size() || b < a) {
                    error = "dict offsets out of range";
                    return false;
                }
                dict_ranges[d] = {bytes_start + a, b - a};
            }
            // Decode the RLE chunk of u32 indices (same framing as the fixed-width RLE path).
            if (data.size() < 10U) {
                error = "dict-rle data chunk too short";
                return false;
            }
            std::uint32_t size0 = 0;
            std::uint32_t size1 = 0;
            std::memcpy(&size0, data.data() + 2U, 4U);
            std::memcpy(&size1, data.data() + 6U, 4U);
            std::size_t voff = 10U;
            voff += (8U - (voff % 8U)) % 8U;
            std::size_t loff = voff + size0;
            loff += (8U - (loff % 8U)) % 8U;
            if (loff + size1 > data.size() || size0 % 4U != 0U || size0 / 4U != size1) {
                error = "dict-rle chunk sizes invalid";
                return false;
            }
            const std::size_t num_runs = size1;
            std::uint64_t cumulative = 0;
            auto push_offset = [&](std::uint64_t v) {
                if (out.variable.large) {
                    out.variable.offsets.insert(out.variable.offsets.end(),
                                                reinterpret_cast<const std::uint8_t*>(&v),
                                                reinterpret_cast<const std::uint8_t*>(&v) + 8);
                } else {
                    const auto v32 = static_cast<std::uint32_t>(v);
                    out.variable.offsets.insert(out.variable.offsets.end(),
                                                reinterpret_cast<const std::uint8_t*>(&v32),
                                                reinterpret_cast<const std::uint8_t*>(&v32) + 4);
                }
            };
            // Pre-pass: reserve data + offsets to final size so the expansion never reallocates.
            std::size_t total_rows = 0;
            std::size_t total_data = 0;
            for (std::size_t r = 0; r < num_runs; ++r) {
                std::uint32_t index = 0;
                std::memcpy(&index, data.data() + voff + r * 4U, 4U);
                if (index >= num_dict) {
                    error = "dict-rle index out of range";
                    return false;
                }
                const std::uint8_t run = data[loff + r];
                total_rows += run;
                total_data += static_cast<std::size_t>(run) * dict_ranges[index].second;
            }
            out.variable.data.reserve(out.variable.data.size() + total_data);
            out.variable.offsets.reserve(out.variable.offsets.size() + (total_rows + 1U) * ow);

            if (out.variable.offsets.empty()) {
                push_offset(0);
            }
            for (std::size_t r = 0; r < num_runs; ++r) {
                std::uint32_t index = 0;
                std::memcpy(&index, data.data() + voff + r * 4U, 4U);
                const std::uint8_t run = data[loff + r];
                const auto [start, len] = dict_ranges[index];
                for (std::uint8_t c = 0; c < run; ++c) {
                    out.variable.data.insert(out.variable.data.end(), dict_block.begin() + start,
                                             dict_block.begin() + start + len);
                    cumulative += len;
                    push_offset(cumulative);
                }
            }
            (void)ow;
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
    const bool bitpacked = field_metadata_equals(on_disk_field, "nanolance:packing", "bitpack");
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
        if (bitpacked) {
            if (!unpack_bitpacked_page_dispatch(chunk_bytes, page.length, bytes_per_value, out.fixed, error)) {
                return false;
            }
            continue;
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
