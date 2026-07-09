// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/lance_column_decoder.hpp"

#include "nanolance/blob_v2_external.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/fastlanes_bitpack.hpp"
#include "nanolance/read_safety.hpp"
#include "nanolance/schema_mapper.hpp"

#include <zstd.h>

#include <cstring>
#include <limits>
#include <utility>

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

// Append `count` copies of an vlen-byte value via one resize + tight memcpy loop. Much leaner than
// count separate std::vector::insert calls (whose per-call machinery dominated the read profile).
// Returns false (without touching `out`) if the resulting size would overflow — `count` can derive
// from untrusted on-disk run/row counts, so the multiply must not wrap into a small allocation.
[[nodiscard]] bool append_repeated_value(std::vector<std::uint8_t>& out, const std::uint8_t* val,
                                         std::size_t vlen, std::size_t count) {
    if (count == 0U || vlen == 0U) {
        return true;
    }
    std::uint64_t added = 0;
    std::uint64_t new_size = 0;
    if (!checked_mul(vlen, count, added) || !checked_add(out.size(), added, new_size) ||
        !fits_size_t(new_size)) {
        return false;
    }
    const std::size_t base = out.size();
    out.resize(static_cast<std::size_t>(new_size));
    std::uint8_t* dst = out.data() + base;
    for (std::size_t i = 0; i < count; ++i) {
        std::memcpy(dst, val, vlen);
        dst += vlen;
    }
    return true;
}

// Inverse of zstd_frame_buffer: [u64 LE uncompressed size][zstd frame] -> raw bytes.
bool zstd_unframe_buffer(const std::vector<std::uint8_t>& framed, std::vector<std::uint8_t>& out, std::string& error) {
    if (framed.size() < 8U) {
        error = "zstd frame shorter than size header";
        return false;
    }
    const std::uint64_t uncompressed = load_le<std::uint64_t>(framed.data());
    // The declared uncompressed size is attacker-controlled: cap it (DoS/OOM) and never allocate more
    // than the platform can index. Also cross-check it against the zstd frame's own content size when
    // the frame records one, so a lie in the header can't drive a giant allocation.
    if (uncompressed > default_read_limits().max_uncompressed_bytes || !fits_size_t(uncompressed)) {
        error = "zstd uncompressed size exceeds safety limit";
        return false;
    }
    const unsigned long long content =
        ZSTD_getFrameContentSize(framed.data() + 8U, framed.size() - 8U);
    if (content != ZSTD_CONTENTSIZE_UNKNOWN && content != ZSTD_CONTENTSIZE_ERROR &&
        content != uncompressed) {
        error = "zstd frame content size disagrees with declared size";
        return false;
    }
    // resize() (not assign(n, 0)) so a reused `out` across many page calls of similar size isn't
    // re-zeroed every time — ZSTD_decompress below unconditionally overwrites all out.size() bytes.
    out.resize(static_cast<std::size_t>(uncompressed));
    const auto got = ZSTD_decompress(out.data(), out.size(), framed.data() + 8U, framed.size() - 8U);
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
        offset = (data_end + 7U) & ~static_cast<std::size_t>(7U);
    }
    return true;
}

bool parse_miniblock_payload_chunk_list(const std::vector<std::uint8_t>& payload,
                                        std::vector<std::vector<std::uint8_t>>& out, std::string& error) {
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
        out.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(data_start),
                         payload.begin() + static_cast<std::ptrdiff_t>(data_end));
        offset = (data_end + 7U) & ~static_cast<std::size_t>(7U);
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
    // num_values comes from the untrusted page.length: compute the offset-table size with overflow-safe
    // math so a hostile count can't wrap `(num_values + 1) * offset_width` into a small value that then
    // passes the bounds check below and lets read_list_offset() over-read the chunk.
    std::uint64_t offset_entries = 0;
    std::uint64_t offsets_bytes64 = 0;
    if (!checked_add(num_values, 1U, offset_entries) ||
        !checked_mul(offset_entries, offset_width, offsets_bytes64) ||
        !fits_size_t(offsets_bytes64)) {
        error = "variable-width offset table size overflow";
        return false;
    }
    const auto offsets_bytes = static_cast<std::size_t>(offsets_bytes64);
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
    // thread_local + resize (not a fresh (packed_words, 0)-initialized vector every call): packed_words is
    // bounded by <=1024 (one FastLanes block), so this reused buffer's capacity converges after the first
    // max-sized call, and resize() only zero-inits a growing delta rather than the whole buffer every time
    // — the memcpy right below unconditionally overwrites all packed_words elements regardless.
    thread_local std::vector<T> packed;
    packed.resize(packed_words);
    if (packed_words != 0U) {
        std::memcpy(packed.data(), chunk.data() + sizeof(T), packed_words * sizeof(T));
    }
    // A FastLanes chunk unpacks into exactly 1024 values. `num_values` is attacker-controlled (it comes
    // from page.length on the untrusted read path), so it MUST NOT exceed the block size — otherwise the
    // copy below over-reads the `values` stack buffer. The last chunk legitimately emits fewer.
    if (num_values > 1024U) {
        error = "bitpacked chunk value count exceeds FastLanes block size";
        return false;
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

    // The per-page row count comes from the untrusted protobuf. Bound the declared total up front
    // (overflow-safe) so no decode branch below can be tricked into a runaway allocation; the inner
    // loops then run without re-checking.
    std::uint64_t declared_rows = 0;
    for (const auto& page : column_metadata.pages) {
        if (!checked_add(declared_rows, page.length, declared_rows)) {
            error = "column page row count overflows";
            return false;
        }
    }
    if (declared_rows > default_read_limits().max_rows_per_column) {
        error = "column row count exceeds safety limit";
        return false;
    }

    const bool blob_packed = field_metadata_is_true(on_disk_field, "lance-encoding:blob");
    if (blob_packed) {
        out.kind = ColumnValues::Kind::BlobV2External;
        // Hoisted out of the loop (not freshly declared per page): read_lance_data_file_bytes reuses
        // whatever capacity/bytes are already here rather than re-zeroing a fresh buffer every page.
        std::vector<std::uint8_t> control;
        std::vector<std::uint8_t> values;
        for (const auto& page : column_metadata.pages) {
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
            const std::size_t len = value->size();
            const auto rows = static_cast<std::size_t>(total_rows);
            if (!append_repeated_value(out.variable.data, value->data(), len, rows)) {  // every row = value
                error = "constant column expansion overflows";
                return false;
            }
            // offsets are arithmetic (0, len, 2*len, ...); build typed then one bulk copy.
            if (out.variable.large) {
                std::vector<std::uint64_t> offs(rows + 1U);
                for (std::size_t i = 0; i <= rows; ++i) offs[i] = static_cast<std::uint64_t>(i) * len;
                out.variable.offsets.resize(offs.size() * 8U);
                std::memcpy(out.variable.offsets.data(), offs.data(), offs.size() * 8U);
            } else {
                std::vector<std::uint32_t> offs(rows + 1U);
                for (std::size_t i = 0; i <= rows; ++i) offs[i] = static_cast<std::uint32_t>(i * len);
                out.variable.offsets.resize(offs.size() * 4U);
                std::memcpy(out.variable.offsets.data(), offs.data(), offs.size() * 4U);
            }
            return true;
        }
        out.kind = ColumnValues::Kind::FixedWidth;
        if (!append_repeated_value(out.fixed, value->data(), value->size(),
                                   static_cast<std::size_t>(total_rows))) {
            error = "constant column expansion overflows";
            return false;
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
        std::vector<std::uint8_t> control;
        std::vector<std::uint8_t> data;
        for (const auto& page : column_metadata.pages) {
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
            for (std::size_t r = 0; r < num_runs; ++r) {
                const std::uint64_t run = run_length_at(r);
                const auto* vptr = data.data() + values_off + r * bpv;
                if (!append_repeated_value(out.fixed, vptr, bpv, static_cast<std::size_t>(run))) {
                    error = "rle run expansion overflows";
                    return false;
                }
            }
        }
        return true;
    }

    // Dictionary + RLE variable-width column: buffer[1] = RLE'd u32 indices, buffer[2] = dictionary.
    if (field_metadata_equals(on_disk_field, "nanolance:packing", "dict-rle")) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = on_disk_field.logical_type == "large_utf8" || on_disk_field.logical_type == "large_binary";
        std::vector<std::uint8_t> data;       // buffer[1]: RLE chunk of indices
        std::vector<std::uint8_t> dict_frame;  // buffer[2]: dictionary
        std::vector<std::uint8_t> dict_block;
        for (const auto& page : column_metadata.pages) {
            if (page.buffer_offsets.size() < 3U || page.buffer_sizes.size() < 3U) {
                error = "dict-rle page missing buffers";
                return false;
            }
            if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[1], page.buffer_sizes[1], data, error) ||
                !read_lance_data_file_bytes(data_file_path, page.buffer_offsets[2], page.buffer_sizes[2], dict_frame,
                                            error)) {
                return false;
            }
            // Decode the dictionary: un-zstd -> [u32 32][u32 bytes_start][u32 offsets][data].
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
            // Pre-pass: validate indices and size the output.
            std::size_t total_rows = 0;
            std::size_t total_data = 0;
            for (std::size_t r = 0; r < num_runs; ++r) {
                std::uint32_t index = 0;
                std::memcpy(&index, data.data() + voff + r * 4U, 4U);
                if (index >= num_dict) {
                    error = "dict-rle index out of range";
                    return false;
                }
                total_rows += data[loff + r];
                total_data += static_cast<std::size_t>(data[loff + r]) * dict_ranges[index].second;
            }
            out.variable.data.reserve(out.variable.data.size() + total_data);
            const bool first_page = out.variable.offsets.empty();
            std::uint64_t cumulative = out.variable.data.size();  // byte offset (continues across pages)

            // Expand: bulk-fill data once per run; collect offsets in a typed temp, then one bulk copy.
            auto expand = [&](auto& offs) -> bool {
                offs.reserve(total_rows + (first_page ? 1U : 0U));
                using OT = typename std::decay_t<decltype(offs)>::value_type;
                if (first_page) {
                    offs.push_back(static_cast<OT>(cumulative));
                }
                for (std::size_t r = 0; r < num_runs; ++r) {
                    std::uint32_t index = 0;
                    std::memcpy(&index, data.data() + voff + r * 4U, 4U);
                    const std::uint8_t run = data[loff + r];
                    const auto [start, len] = dict_ranges[index];
                    if (!append_repeated_value(out.variable.data, dict_block.data() + start, len, run)) {
                        return false;
                    }
                    for (std::uint8_t c = 0; c < run; ++c) {
                        cumulative += len;
                        offs.push_back(static_cast<OT>(cumulative));
                    }
                }
                const std::size_t base = out.variable.offsets.size();
                out.variable.offsets.resize(base + offs.size() * sizeof(OT));
                std::memcpy(out.variable.offsets.data() + base, offs.data(), offs.size() * sizeof(OT));
                return true;
            };
            bool ok = false;
            if (out.variable.large) {
                std::vector<std::uint64_t> offs;
                ok = expand(offs);
            } else {
                std::vector<std::uint32_t> offs;
                ok = expand(offs);
            }
            if (!ok) {
                error = "dict-rle expansion overflows";
                return false;
            }
        }
        return true;
    }

    // Structural dictionary variable-width column: buffer[1] = bitpacked u32 indices, buffer[2] = dictionary.
    if (field_metadata_equals(on_disk_field, "nanolance:packing", "dict")) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = on_disk_field.logical_type == "large_utf8" || on_disk_field.logical_type == "large_binary";
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> dict_block;
        std::vector<std::uint8_t> indices_bytes;
        for (const auto& page : column_metadata.pages) {
            if (page.buffer_offsets.size() < 3U || page.buffer_sizes.size() < 3U) {
                error = "dict page missing buffers";
                return false;
            }
            if (!read_lance_data_file_bytes(data_file_path, page.buffer_offsets[1], page.buffer_sizes[1], payload,
                                            error) ||
                !read_lance_data_file_bytes(data_file_path, page.buffer_offsets[2], page.buffer_sizes[2], dict_block,
                                            error)) {
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
            std::vector<std::vector<std::uint8_t>> chunks;
            if (!parse_miniblock_payload_chunk_list(payload, chunks, error)) {
                return false;
            }
            if (chunks.empty()) {
                error = "dict page has no index chunks";
                return false;
            }
            indices_bytes.clear();  // hoisted out of the loop; accumulates fresh per page via insert()
            std::uint64_t rows_remaining = page.length;
            for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
                const auto count = std::min<std::uint64_t>(1024U, rows_remaining);
                if (!unpack_bitpacked_page_dispatch(chunks[ci], count, 4U, indices_bytes, error)) {
                    return false;
                }
                rows_remaining -= count;
            }
            if (rows_remaining != 0U || indices_bytes.size() != page.length * 4U) {
                error = "dict index count mismatch";
                return false;
            }
            const bool first_page = out.variable.offsets.empty();
            std::uint64_t cumulative = out.variable.data.size();
            if (first_page) {
                append_list_offset(out.variable.offsets, static_cast<std::int64_t>(cumulative), out.variable.large);
            }
            for (std::uint64_t r = 0; r < page.length; ++r) {
                std::uint32_t index = 0;
                std::memcpy(&index, indices_bytes.data() + r * 4U, 4U);
                if (index >= num_dict) {
                    error = "dict index out of range";
                    return false;
                }
                const auto [start, len] = dict_ranges[index];
                out.variable.data.insert(out.variable.data.end(), dict_block.begin() + static_cast<std::ptrdiff_t>(start),
                                         dict_block.begin() + static_cast<std::ptrdiff_t>(start + len));
                cumulative += len;
                append_list_offset(out.variable.offsets, static_cast<std::int64_t>(cumulative), out.variable.large);
            }
        }
        return true;
    }

    const bool variable = on_disk_field.encoding == 2;
    if (variable) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = on_disk_field.logical_type == "large_utf8" || on_disk_field.logical_type == "large_binary";
        const bool zstd = field_metadata_equals(on_disk_field, "lance-encoding:compression", "zstd");
        std::vector<std::uint8_t> control;
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> chunk_bytes;
        std::vector<std::uint8_t> raw;
        for (const auto& page : column_metadata.pages) {
            if (!read_page_buffers(data_file_path, page, false, control, payload, error)) {
                return false;
            }
            if (!parse_miniblock_payload_chunks(payload, chunk_bytes, error)) {
                return false;
            }
            if (zstd) {
                // One chunk per page in nanolance's writer, so the payload holds one [u64][zstd] frame.
                if (!zstd_unframe_buffer(chunk_bytes, raw, error)) {
                    return false;
                }
                // swap (not move): a move would leave `raw` empty every iteration, discarding its
                // capacity right when the next page's zstd_unframe_buffer call could have reused it.
                std::swap(chunk_bytes, raw);
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
    std::vector<std::uint8_t> control;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> chunk_bytes;
    for (const auto& page : column_metadata.pages) {
        if (!read_page_buffers(data_file_path, page, false, control, payload, error)) {
            return false;
        }
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
