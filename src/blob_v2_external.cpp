// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/blob_v2_external.hpp"

#include "nanolance/array_accessor.hpp"
#include "nanolance/blob_builder.hpp"
#include "nanolance/nano_lance_reader.h"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <limits>

namespace nano_lance {

// On-disk packed layout of one external (kind=3) blob v2 descriptor row. All integers little-endian.
//
//   offset  size  field
//   ------  ----  -----------------------------------------------------------
//      0      4   prefix      = kBlobV2FixedDescriptorBytes + uri_len (record length minus this u32)
//      4      1   kind        = 3 (external reference)
//      5      8   position    byte offset of the blob inside the external object
//     13      8   size        blob length in bytes
//     21      4   blob_id     reserved; currently always 0
//     25      4   uri_len     length of the uri bytes that follow
//     29  uri_len uri         external object URI (e.g. "s3://bucket/key", "file:///path")
//
// Total record bytes = kBlobV2RecordHeaderBytes + uri_len.
// `prefix` deliberately excludes its own 4 bytes so it equals everything Lance counts as the record body.
constexpr std::uint32_t kBlobV2FixedDescriptorBytes = 25U;             // kind..uri_len, excludes leading prefix u32
constexpr std::size_t kBlobV2RecordHeaderBytes = 4U + kBlobV2FixedDescriptorBytes;  // prefix u32 + fixed fields = 29

namespace {

void append_le16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_le32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

bool arrow_row_is_null(const ArrowArray& array, std::int64_t row_index) {
    if (array.buffers == nullptr || array.buffers[0] == nullptr) {
        return false;
    }
    const auto* validity = static_cast<const std::uint8_t*>(array.buffers[0]);
    const std::int64_t i = array.offset + row_index;
    return (validity[i / 8] & static_cast<std::uint8_t>(1 << (i % 8))) == 0;
}

bool large_binary_value(const ArrowArray& array, std::int64_t row_index, bool* is_null, const std::uint8_t** data, std::int64_t* size) {
    *is_null = false;
    *data = nullptr;
    *size = 0;
    if (array.length <= 0 || row_index < 0 || row_index >= array.length) {
        return false;
    }
    if (array.n_buffers < 3 || array.buffers == nullptr || array.buffers[1] == nullptr || array.buffers[2] == nullptr) {
        return false;
    }
    if (arrow_row_is_null(array, row_index)) {
        *is_null = true;
        return true;
    }
    const auto* offsets = static_cast<const std::uint8_t*>(array.buffers[1]);
    const auto* raw_data = static_cast<const std::uint8_t*>(array.buffers[2]);
    const auto row = array.offset + row_index;
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::memcpy(&start, offsets + static_cast<std::size_t>(row) * 8U, sizeof(std::int64_t));
    std::memcpy(&end, offsets + static_cast<std::size_t>(row + 1) * 8U, sizeof(std::int64_t));
    *data = raw_data + start;
    *size = end - start;
    return true;
}

bool utf8_value(const ArrowArray& array, std::int64_t row_index, bool* is_null, const std::uint8_t** data, std::int32_t* size) {
    *is_null = false;
    *data = nullptr;
    *size = 0;
    if (array.length <= 0 || row_index < 0 || row_index >= array.length) {
        return false;
    }
    if (array.n_buffers < 3 || array.buffers == nullptr || array.buffers[1] == nullptr || array.buffers[2] == nullptr) {
        return false;
    }
    if (arrow_row_is_null(array, row_index)) {
        *is_null = true;
        return true;
    }
    const auto* offsets = static_cast<const std::uint8_t*>(array.buffers[1]);
    const auto* raw_data = static_cast<const std::uint8_t*>(array.buffers[2]);
    const auto row = array.offset + row_index;
    std::int32_t start = 0;
    std::int32_t end = 0;
    std::memcpy(&start, offsets + static_cast<std::size_t>(row) * 4U, sizeof(std::int32_t));
    std::memcpy(&end, offsets + static_cast<std::size_t>(row + 1) * 4U, sizeof(std::int32_t));
    *data = raw_data + start;
    *size = end - start;
    return true;
}

bool uint64_value(const ArrowArray& array, std::int64_t row_index, bool* is_null, std::uint64_t* out) {
    *is_null = false;
    *out = 0;
    if (array.length <= 0 || row_index < 0 || row_index >= array.length) {
        return false;
    }
    if (array.n_buffers < 2 || array.buffers == nullptr || array.buffers[1] == nullptr) {
        return false;
    }
    if (array.buffers[0] != nullptr && arrow_row_is_null(array, row_index)) {
        *is_null = true;
        return true;
    }
    const auto* values = static_cast<const std::uint8_t*>(array.buffers[1]);
    std::memcpy(out, values + static_cast<std::size_t>(array.offset + row_index) * 8U, sizeof(std::uint64_t));
    return true;
}

const LanceField* find_child(const LanceSchemaMapping& mapping, std::int32_t parent_id, const std::string& name) {
    for (const auto& field : mapping.fields) {
        if (field.parent_id == parent_id && field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

}  // namespace

const std::vector<std::uint8_t>& blob_v2_page_layout_encoding() {
    // Full PageLayout protobuf from Lance reference writer (includes outer length-delimited wrapper).
    static const std::vector<std::uint8_t> kEncoding = [] {
        const char* hex =
            "0a670a1d2f6c616e63652e656e636f64696e677332312e506167654c61796f757412461a442020280230023a386a360a080a040a02080810080a080a040a"
            "02084010400a080a040a02084010400a080a040a02082010200a0c0a0812060a040a020820182042020101";
        std::vector<std::uint8_t> out;
        for (std::size_t i = 0; i + 1 < std::strlen(hex); i += 2) {
            const auto pair = std::string(hex + i, 2);
            out.push_back(static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16)));
        }
        return out;
    }();
    return kEncoding;
}

const std::vector<std::uint8_t>& blob_v2_column_page_encoding() {
    // `encode_direct_encoding` expects the inner ColumnEncoding message (type URL + PageLayout),
    // not the extra length-delimited wrapper present in `blob_v2_page_layout_encoding()`.
    static const std::vector<std::uint8_t> kEncoding = [] {
        const auto& wrapped = blob_v2_page_layout_encoding();
        std::size_t offset = 0;
        if (wrapped.size() >= 2U && wrapped[0] == 0x0aU) {
            std::size_t index = 1U;
            std::uint64_t length = 0;
            std::uint64_t shift = 0;
            while (index < wrapped.size()) {
                const auto byte = wrapped[index++];
                length |= (byte & 0x7FU) << shift;
                if ((byte & 0x80U) == 0U) {
                    break;
                }
                shift += 7U;
            }
            if (index + length <= wrapped.size()) {
                offset = index;
            }
        }
        return std::vector<std::uint8_t>(wrapped.begin() + static_cast<std::ptrdiff_t>(offset), wrapped.end());
    }();
    return kEncoding;
}

namespace {

/// One packed descriptor row appended to `out`: the record of `blob_v2_pack_descriptor_row`, its
/// trailing bytes being the URI -- or, for a writer-pending row, the blob's own bytes.
bool append_packed_row(ColumnValues& out, std::uint8_t kind, std::uint64_t position, std::uint64_t size,
                       std::uint32_t blob_id, const std::uint8_t* tail, std::size_t tail_len, std::string& error) {
    if (tail_len > std::numeric_limits<std::uint32_t>::max() - kBlobV2RecordHeaderBytes) {
        error = "a blob of 4 GiB or more";
        return false;
    }
    auto& payload = out.blob_v2.packed_payload;
    const auto at = payload.size();
    payload.resize(at + kBlobV2RecordHeaderBytes + tail_len);
    auto* p = payload.data() + at;
    const std::uint32_t prefix = kBlobV2FixedDescriptorBytes + static_cast<std::uint32_t>(tail_len);
    const auto tail32 = static_cast<std::uint32_t>(tail_len);
    std::memcpy(p, &prefix, 4);
    p[4] = kind;
    std::memcpy(p + 5, &position, 8);
    std::memcpy(p + 13, &size, 8);
    std::memcpy(p + 21, &blob_id, 4);
    std::memcpy(p + 25, &tail32, 4);
    if (tail_len != 0U) {
        std::memcpy(p + kBlobV2RecordHeaderBytes, tail, tail_len);
    }
    out.blob_v2.row_packed_sizes.push_back(static_cast<std::uint32_t>(kBlobV2RecordHeaderBytes + tail_len));
    return true;
}

/// Row `row`'s validity in `out`, the bitmap made only once a row is null.
void record_validity(ColumnValues& out, std::size_t row, bool valid) {
    if (valid && out.validity.empty()) {
        return;
    }
    if (out.validity.empty()) {
        out.validity.assign((row + 8U) / 8U, 0xFFU);
    }
    out.validity.resize((row + 8U) / 8U, 0xFFU);
    if (valid) {
        out.validity[row / 8U] |= static_cast<std::uint8_t>(1U << (row % 8U));
    } else {
        out.validity[row / 8U] &= static_cast<std::uint8_t>(~(1U << (row % 8U)));
        ++out.null_count;
    }
}

}  // namespace

std::vector<std::uint8_t> blob_v2_descriptor_page_encoding(const std::uint64_t rows, const bool nullable) {
    // As Lance writes it: Any{type_url, PageLayout{full_zip_layout = FullZipLayout{bits_def, bits_per_offset
    // = 32, num_items, num_visible_items, value = packed struct of (kind u8, position u64, size u64,
    // blob_id u32, blob_uri variable), layers = [all-valid item | nullable item]}}}.
    static const std::uint8_t kValue[] = {
        0x6a, 0x36, 0x0a, 0x08, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x08, 0x10, 0x08, 0x0a, 0x08, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x40, 0x10,
        0x40, 0x0a, 0x08, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x40, 0x10, 0x40, 0x0a, 0x08, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x20,
        0x10, 0x20, 0x0a, 0x0c, 0x0a, 0x08, 0x12, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x20, 0x18, 0x20};
    const auto varint = [](std::vector<std::uint8_t>& out, std::uint64_t v) {
        while (v >= 0x80U) {
            out.push_back(static_cast<std::uint8_t>(v | 0x80U));
            v >>= 7U;
        }
        out.push_back(static_cast<std::uint8_t>(v));
    };
    const auto message = [&](std::vector<std::uint8_t>& out, std::uint8_t tag, const std::vector<std::uint8_t>& body) {
        out.push_back(tag);
        varint(out, body.size());
        out.insert(out.end(), body.begin(), body.end());
    };
    std::vector<std::uint8_t> full_zip;
    if (nullable) {
        full_zip.insert(full_zip.end(), {0x10, 0x01});  // bits_def = 1
    }
    full_zip.insert(full_zip.end(), {0x20, 0x20});      // bits_per_offset = 32
    full_zip.push_back(0x28);
    varint(full_zip, rows);  // num_items
    full_zip.push_back(0x30);
    varint(full_zip, rows);  // num_visible_items
    message(full_zip, 0x3a, std::vector<std::uint8_t>(std::begin(kValue), std::end(kValue)));
    full_zip.insert(full_zip.end(), {0x42, 0x01, static_cast<std::uint8_t>(nullable ? 0x03 : 0x01)});
    std::vector<std::uint8_t> layout;
    message(layout, 0x1a, full_zip);
    static const std::string kTypeUrl = "/lance.encodings21.PageLayout";
    std::vector<std::uint8_t> out;
    message(out, 0x0a, std::vector<std::uint8_t>(kTypeUrl.begin(), kTypeUrl.end()));
    message(out, 0x12, layout);
    return out;
}

bool blob_v2_thresholds(const LanceField& field, BlobV2Thresholds& out, std::string& error) {
    out = BlobV2Thresholds{};
    const struct {
        const char* key;
        std::uint64_t* value;
        bool allow_zero;
    } keys[] = {{"lance-encoding:blob-inline-size-threshold", &out.inline_max, true},
                {"lance-encoding:blob-dedicated-size-threshold", &out.dedicated_above, false},
                {"lance-encoding:blob-pack-file-size-threshold", &out.pack_file_max, false}};
    for (const auto& k : keys) {
        const auto it = field.metadata.find(k.key);
        if (it == field.metadata.end()) {
            continue;
        }
        const auto& text = it->second;
        std::uint64_t value = 0;
        bool ok = !text.empty() && text.size() <= 20U;
        for (const char c : text) {
            ok = ok && c >= '0' && c <= '9';
        }
        if (ok) {
            try {
                value = std::stoull(text);
            } catch (...) {
                ok = false;
            }
        }
        if (!ok) {
            error = std::string("Invalid blob threshold metadata ") + k.key + "=\"" + text + "\" for field '" +
                    field.name + "'; expected a non-negative integer that fits in usize";
            return false;
        }
        if (!k.allow_zero && value == 0U) {
            error = std::string("Invalid blob threshold metadata ") + k.key + "=\"" + text + "\" for field '" +
                    field.name + "'; expected a positive integer";
            return false;
        }
        *k.value = value;
    }
    return true;
}

bool BlobV2Placer::write_sidecar(const std::uint32_t id, const std::uint8_t* data, const std::uint64_t n,
                                 std::string& error) {
    const auto path = blob_v2_sidecar_path(data_file_, id);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot create blob file " + path.string();
        return false;
    }
    if (n != 0U) {
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    }
    if (!file) {
        error = "cannot write blob file " + path.string();
        return false;
    }
    return true;
}

bool blob_v2_has_pending(const ColumnValues& values) {
    std::size_t offset = 0;
    for (const auto row_size : values.blob_v2.row_packed_sizes) {
        if (row_size >= kBlobV2RecordHeaderBytes && values.blob_v2.packed_payload[offset + 4U] == kBlobKindPendingData) {
            return true;
        }
        offset += row_size;
    }
    return false;
}

bool BlobV2Placer::place(const ColumnValues& values, const BlobV2Thresholds& thresholds, std::ostream& out,
                         ColumnValues& placed, std::string& error) {
    placed = ColumnValues{};
    placed.kind = ColumnValues::Kind::BlobV2External;
    placed.validity = values.validity;
    placed.null_count = values.null_count;
    placed.blob_v2.uri_dictionary = values.blob_v2.uri_dictionary;
    placed.blob_v2.row_packed_sizes.reserve(values.blob_v2.row_packed_sizes.size());
    const auto pack_max = pack_override_ != 0U ? pack_override_ : thresholds.pack_file_max;
    std::size_t offset = 0;
    for (const auto row_size : values.blob_v2.row_packed_sizes) {
        const auto* row = values.blob_v2.packed_payload.data() + offset;
        offset += row_size;
        if (row_size < kBlobV2RecordHeaderBytes || row[4] != kBlobKindPendingData) {
            placed.blob_v2.packed_payload.insert(placed.blob_v2.packed_payload.end(), row, row + row_size);
            placed.blob_v2.row_packed_sizes.push_back(row_size);
            continue;
        }
        const auto* data = row + kBlobV2RecordHeaderBytes;
        const std::uint64_t n = row_size - kBlobV2RecordHeaderBytes;
        std::uint8_t kind = kBlobKindInline;
        std::uint64_t position = 0;
        std::uint32_t id = 0;
        if (n > thresholds.dedicated_above) {
            kind = kBlobKindDedicated;
            id = next_id_++;
            if (!write_sidecar(id, data, n, error)) {
                return false;
            }
        } else if (n > thresholds.inline_max) {
            kind = kBlobKindPacked;
            if (pack_id_ == 0U || pack_max != pack_max_ || pack_size_ + n > pack_max) {
                if (!finish(error)) {
                    return false;
                }
                pack_id_ = next_id_++;
                pack_size_ = 0;
                pack_max_ = pack_max;
                const auto path = blob_v2_sidecar_path(data_file_, pack_id_);
                std::error_code ec;
                std::filesystem::create_directories(path.parent_path(), ec);
                pack_.open(path, std::ios::binary | std::ios::trunc);
                if (!pack_) {
                    error = "cannot create blob file " + path.string();
                    return false;
                }
            }
            id = pack_id_;
            position = pack_size_;
            pack_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
            if (!pack_) {
                error = "cannot write blob file " + blob_v2_sidecar_path(data_file_, id).string();
                return false;
            }
            pack_size_ += n;
        } else {
            // Inline: in the data file itself, each blob at a 64-byte boundary (Lance's out-of-line buffers).
            const auto at = static_cast<std::uint64_t>(out.tellp());
            position = at;
            if (n != 0U) {
                out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
                static const char kPad[64] = {0};
                const auto pad = (64U - (n % 64U)) % 64U;
                out.write(kPad, static_cast<std::streamsize>(pad));
            }
            if (!out) {
                error = "cannot write inline blobs to the data file";
                return false;
            }
        }
        if (!append_packed_row(placed, kind, position, n, id, nullptr, 0, error)) {
            return false;
        }
    }
    return true;
}

bool BlobV2Placer::finish(std::string& error) {
    if (pack_id_ == 0U) {
        return true;
    }
    pack_.close();
    if (!pack_) {
        error = "cannot write blob file " + blob_v2_sidecar_path(data_file_, pack_id_).string();
        return false;
    }
    pack_.clear();
    pack_id_ = 0;
    return true;
}

std::vector<std::int32_t> blob_v2_parent_ids(const LanceSchemaMapping& mapping) {
    std::vector<std::int32_t> ids;
    for (const auto& field : mapping.fields) {
        if (field.extension_name == kBlobV2ExtensionName && field.logical_type == "struct" && field.parent_id == -1) {
            ids.push_back(field.id);
        }
    }
    return ids;
}

const LanceField* find_blob_v2_parent(const LanceSchemaMapping& mapping) {
    for (const auto& field : mapping.fields) {
        if (field.extension_name == kBlobV2ExtensionName && field.logical_type == "struct" && field.parent_id == -1) {
            return &field;
        }
    }
    return nullptr;
}

std::vector<std::uint8_t> blob_v2_build_control_buffer(const std::vector<std::uint32_t>& row_packed_sizes) {
    std::vector<std::uint8_t> out;
    std::uint64_t total = 0;
    std::vector<std::uint64_t> prefixes;
    prefixes.reserve(row_packed_sizes.size());
    for (const auto sz : row_packed_sizes) {
        total += static_cast<std::uint64_t>(sz);
        prefixes.push_back(total);
    }
    // Blob control buffer encodings (cumulative row-end offsets):
    //   narrow: [0][u8  cumulative offsets ...]            total < 256
    //   wide16: [0][0][u16 cumulative offsets ...]         total <= 65535
    //   wide32: [0][1][u32 cumulative offsets ...]         otherwise
    // The wide forms carry a width discriminator in byte[1] (the reader already keyed off it). Writing
    // a too-narrow width above wrapped the cumulative offsets and made them non-monotonic.
    const bool narrow = total < 256U;
    if (narrow) {
        out.push_back(0U);
        for (const auto p : prefixes) {
            out.push_back(static_cast<std::uint8_t>(p));
        }
        return out;
    }
    const bool wide16 = total <= static_cast<std::uint64_t>(std::numeric_limits<std::uint16_t>::max());
    out.push_back(0U);
    out.push_back(wide16 ? 0U : 1U);
    for (const auto p : prefixes) {
        if (wide16) {
            append_le16(out, static_cast<std::uint16_t>(p));
        } else {
            append_le32(out, static_cast<std::uint32_t>(p));
        }
    }
    return out;
}

bool blob_v2_control_buffer_to_row_sizes(const std::vector<std::uint8_t>& control, const std::uint64_t num_rows,
                                         std::vector<std::uint32_t>& row_packed_sizes, std::string& error) {
    row_packed_sizes.clear();
    error.clear();
    if (num_rows > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        error = "blob row count overflow";
        return false;
    }
    const auto n = static_cast<std::uint32_t>(num_rows);
    if (control.empty()) {
        error = "empty blob control buffer";
        return false;
    }
    if (control[0] != 0U) {
        error = "invalid blob control buffer prefix";
        return false;
    }
    if (n == 0U) {
        if (control.size() != 1U) {
            error = "invalid blob control buffer for zero rows";
            return false;
        }
        return true;
    }
    const auto expect_narrow = static_cast<std::size_t>(1U) + static_cast<std::size_t>(n);
    const auto expect_wide16 = static_cast<std::size_t>(2U) + 2U * static_cast<std::size_t>(n);
    const auto expect_wide32 = static_cast<std::size_t>(2U) + 4U * static_cast<std::size_t>(n);
    if (control.size() == expect_narrow) {
        std::uint8_t prev = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto cum = control[static_cast<std::size_t>(1U + i)];
            if (cum < prev) {
                error = "invalid narrow cumulative offsets in blob control buffer";
                return false;
            }
            row_packed_sizes.push_back(static_cast<std::uint32_t>(static_cast<std::uint32_t>(cum) - prev));
            prev = cum;
        }
        return true;
    }
    // Wide control buffers carry a width discriminator in byte[1]: 0 = u16 offsets, 1 = u32 offsets.
    // expect_wide16 and expect_wide32 only collide when n == 0, which is handled above.
    if (control.size() == expect_wide16) {
        if (control[1] != 0U) {
            error = "invalid wide blob control buffer header";
            return false;
        }
        std::uint16_t prev = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto base = static_cast<std::size_t>(2U + 2U * i);
            std::uint16_t cum = 0;
            std::memcpy(&cum, control.data() + base, sizeof(cum));
            if (cum < prev) {
                error = "invalid wide cumulative offsets in blob control buffer";
                return false;
            }
            row_packed_sizes.push_back(static_cast<std::uint32_t>(cum - prev));
            prev = cum;
        }
        return true;
    }
    if (control.size() == expect_wide32) {
        if (control[1] != 1U) {
            error = "invalid wide blob control buffer header";
            return false;
        }
        std::uint32_t prev = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto base = static_cast<std::size_t>(2U + 4U * i);
            std::uint32_t cum = 0;
            std::memcpy(&cum, control.data() + base, sizeof(cum));
            if (cum < prev) {
                error = "invalid wide cumulative offsets in blob control buffer";
                return false;
            }
            row_packed_sizes.push_back(cum - prev);
            prev = cum;
        }
        return true;
    }
    error = "blob control buffer size does not match row count (narrow/wide16/wide32)";
    return false;
}

bool finalize_blob_v2_schema_for_write(LanceSchemaMapping& mapping, std::string& error) {
    error.clear();
    // As Lance 12 keeps a Blob v2 field in the schema: its logical children (data, uri[, position, size]),
    // every one nullable, none a column; the field itself one column, in schema order.
    const auto ids = blob_v2_parent_ids(mapping);
    if (ids.empty()) {
        return true;
    }
    for (const auto id : ids) {
        if (find_child(mapping, id, "kind") != nullptr) {
            continue;  // an older nanolance dataset's descriptor-shaped field, appended to as it is
        }
        if (find_child(mapping, id, "data") == nullptr || find_child(mapping, id, "uri") == nullptr) {
            error = "lance.blob.v2 struct must have data and uri children";
            return false;
        }
    }
    for (auto& field : mapping.fields) {
        const bool blob = std::find(ids.begin(), ids.end(), field.id) != ids.end();
        const bool child = std::find(ids.begin(), ids.end(), field.parent_id) != ids.end();
        if (blob) {
            field.column_index = 0;  // a column; numbered below
            if (field.metadata.find("ARROW:extension:metadata") == field.metadata.end()) {
                field.metadata["ARROW:extension:metadata"] = "";
            }
        } else if (child) {
            field.column_index = -1;
            field.nullable = true;
        }
    }
    renumber_columns_for_one_file(mapping);
    return true;
}

bool preprocess_blob_v2_external_row(const ArrowArray& data,
                                     const ArrowArray& uri,
                                     const ArrowArray& position,
                                     const ArrowArray& size,
                                     std::int64_t row_index,
                                     BlobV2ExternalDescriptor& out,
                                     std::string& error) {
    error.clear();
    out = BlobV2ExternalDescriptor{};

    bool data_null = false;
    const std::uint8_t* data_ptr = nullptr;
    std::int64_t data_size = 0;
    if (!large_binary_value(data, row_index, &data_null, &data_ptr, &data_size)) {
        error = "failed to read blob v2 data child";
        return false;
    }
    if (!data_null && data_size > 0) {
        error =
            "lance.blob.v2 native writer supports only external references (kind=3): inline/packed/dedicated rows "
            "must not include non-empty `data`; use `uri` + `position` + `size` instead";
        return false;
    }

    bool uri_null = false;
    const std::uint8_t* uri_ptr = nullptr;
    std::int32_t uri_len = 0;
    if (!utf8_value(uri, row_index, &uri_null, &uri_ptr, &uri_len)) {
        error = "failed to read blob v2 uri child";
        return false;
    }
    if (uri_null || uri_len <= 0) {
        error =
            "lance.blob.v2 external rows require a non-null, non-empty `uri` (native writer does not support "
            "inline blobs)";
        return false;
    }

    bool pos_null = false;
    std::uint64_t position_value = 0;
    if (!uint64_value(position, row_index, &pos_null, &position_value) || pos_null) {
        error = "blob v2 external rows require non-null `position`";
        return false;
    }

    bool size_null = false;
    std::uint64_t size_value = 0;
    if (!uint64_value(size, row_index, &size_null, &size_value) || size_null || size_value == 0U) {
        error = "blob v2 external rows require non-null, non-zero `size`";
        return false;
    }

    out.kind = 3;
    out.position = position_value;
    out.size = size_value;
    out.blob_id = 0;
    out.blob_uri.assign(reinterpret_cast<const char*>(uri_ptr), static_cast<std::size_t>(uri_len));
    return true;
}

std::vector<std::uint8_t> blob_v2_pack_descriptor_row(const BlobV2ExternalDescriptor& descriptor) {
    const auto uri_bytes = static_cast<std::uint32_t>(descriptor.blob_uri.size());
    const std::uint32_t prefix = kBlobV2FixedDescriptorBytes + uri_bytes;

    std::vector<std::uint8_t> row_bytes;
    row_bytes.reserve(4U + 1U + 8U + 8U + 4U + 4U + descriptor.blob_uri.size());
    row_bytes.push_back(static_cast<std::uint8_t>(prefix & 0xFFU));
    row_bytes.push_back(static_cast<std::uint8_t>((prefix >> 8U) & 0xFFU));
    row_bytes.push_back(static_cast<std::uint8_t>((prefix >> 16U) & 0xFFU));
    row_bytes.push_back(static_cast<std::uint8_t>((prefix >> 24U) & 0xFFU));
    row_bytes.push_back(descriptor.kind);
    for (int shift = 0; shift < 64; shift += 8) {
        row_bytes.push_back(static_cast<std::uint8_t>((descriptor.position >> shift) & 0xFFU));
    }
    for (int shift = 0; shift < 64; shift += 8) {
        row_bytes.push_back(static_cast<std::uint8_t>((descriptor.size >> shift) & 0xFFU));
    }
    row_bytes.push_back(static_cast<std::uint8_t>(descriptor.blob_id & 0xFFU));
    row_bytes.push_back(static_cast<std::uint8_t>((descriptor.blob_id >> 8U) & 0xFFU));
    row_bytes.push_back(static_cast<std::uint8_t>((descriptor.blob_id >> 16U) & 0xFFU));
    row_bytes.push_back(static_cast<std::uint8_t>((descriptor.blob_id >> 24U) & 0xFFU));
    row_bytes.push_back(static_cast<std::uint8_t>(uri_bytes & 0xFFU));
    row_bytes.push_back(static_cast<std::uint8_t>((uri_bytes >> 8U) & 0xFFU));
    row_bytes.push_back(static_cast<std::uint8_t>((uri_bytes >> 16U) & 0xFFU));
    row_bytes.push_back(static_cast<std::uint8_t>((uri_bytes >> 24U) & 0xFFU));
    row_bytes.insert(row_bytes.end(), descriptor.blob_uri.begin(), descriptor.blob_uri.end());
    return row_bytes;
}

bool blob_v2_unpack_descriptor_row(const std::vector<std::uint8_t>& row_bytes,
                                     BlobV2ExternalDescriptor& out,
                                     std::string& error) {
    error.clear();
    out = BlobV2ExternalDescriptor{};
    if (row_bytes.size() < kBlobV2RecordHeaderBytes) {
        error = "packed blob v2 row is too short";
        return false;
    }

    std::uint32_t prefix = 0;
    std::memcpy(&prefix, row_bytes.data(), sizeof(std::uint32_t));
    out.kind = row_bytes[4];
    std::memcpy(&out.position, row_bytes.data() + 5U, sizeof(std::uint64_t));
    std::memcpy(&out.size, row_bytes.data() + 13U, sizeof(std::uint64_t));
    std::memcpy(&out.blob_id, row_bytes.data() + 21U, sizeof(std::uint32_t));
    std::uint32_t uri_len = 0;
    std::memcpy(&uri_len, row_bytes.data() + 25U, sizeof(std::uint32_t));
    if (prefix != kBlobV2FixedDescriptorBytes + uri_len) {
        error = "packed blob v2 row prefix does not match uri length";
        return false;
    }
    if (row_bytes.size() != kBlobV2RecordHeaderBytes + uri_len) {
        error = "packed blob v2 row size does not match uri payload";
        return false;
    }
    out.blob_uri.assign(reinterpret_cast<const char*>(row_bytes.data() + kBlobV2RecordHeaderBytes), uri_len);
    return true;
}

std::string blob_v2_serialize_uri_dictionary(const std::vector<std::string>& dictionary) {
    std::string out;
    for (std::size_t i = 0; i < dictionary.size(); ++i) {
        if (i != 0U) {
            out.push_back('\n');
        }
        out += dictionary[i];
    }
    return out;
}

std::vector<std::string> blob_v2_parse_uri_dictionary(const std::string& serialized) {
    std::vector<std::string> out;
    if (serialized.empty()) {
        return out;
    }
    std::size_t start = 0;
    while (true) {
        const auto nl = serialized.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(serialized.substr(start));
            break;
        }
        out.push_back(serialized.substr(start, nl - start));
        start = nl + 1U;
    }
    return out;
}

bool append_blob_v2_batch_column_values(const ArrowArray& batch,
                                        const LanceSchemaMapping& mapping,
                                        const LanceField& blob_field,
                                        bool dictionary_mode,
                                        ColumnValues& out,
                                        std::string& error) {
    error.clear();
    ArrowArray struct_array{};
    if (!resolve_field_array(batch, mapping, blob_field, struct_array)) {
        error = "missing Arrow struct array for blob field ";
        error += blob_field.name;
        return false;
    }

    // Lance's logical shape: struct<data, uri> or struct<data, uri, position, size>.
    const auto* data_f = find_child(mapping, blob_field.id, "data");
    const auto* uri_f = find_child(mapping, blob_field.id, "uri");
    const auto* pos_f = find_child(mapping, blob_field.id, "position");
    const auto* size_f = find_child(mapping, blob_field.id, "size");
    if (data_f == nullptr || uri_f == nullptr || (pos_f == nullptr) != (size_f == nullptr)) {
        error = "Blob v2 field '" + blob_field.name +
                "' must be struct<data: large_binary, uri: utf8> or struct<data, uri, position: uint64, size: uint64>";
        return false;
    }

    ArrowArray data_a{};
    ArrowArray uri_a{};
    ArrowArray pos_a{};
    ArrowArray size_a{};
    const bool ranged = pos_f != nullptr;
    if (!resolve_field_array(batch, mapping, *data_f, data_a) || !resolve_field_array(batch, mapping, *uri_f, uri_a) ||
        (ranged && (!resolve_field_array(batch, mapping, *pos_f, pos_a) ||
                    !resolve_field_array(batch, mapping, *size_f, size_a)))) {
        error = "missing Arrow arrays for blob children";
        return false;
    }

    out.kind = ColumnValues::Kind::BlobV2External;
    const auto rows = struct_array.length;
    out.blob_v2.row_packed_sizes.reserve(out.blob_v2.row_packed_sizes.size() + static_cast<std::size_t>(rows));
    const auto row_error = [&](std::int64_t row, const char* what) {
        error = "Blob v2 field '" + blob_field.name + "' row " + std::to_string(row) + " " + what;
        return false;
    };
    for (std::int64_t row = 0; row < rows; ++row) {
        const auto index = out.blob_v2.row_packed_sizes.size();
        if (arrow_row_is_null(struct_array, row)) {
            if (!blob_field.nullable) {
                error = "Column '" + blob_field.name + "' is declared as non-nullable but contains null values";
                return false;
            }
            record_validity(out, index, false);
            out.blob_v2.row_packed_sizes.push_back(0U);
            continue;
        }
        bool data_null = true;
        const std::uint8_t* data_ptr = nullptr;
        std::int64_t data_size = 0;
        bool uri_null = true;
        const std::uint8_t* uri_ptr = nullptr;
        std::int32_t uri_len = 0;
        bool pos_null = true;
        bool size_null = true;
        std::uint64_t position = 0;
        std::uint64_t size = 0;
        if ((data_a.length > 0 && !large_binary_value(data_a, row, &data_null, &data_ptr, &data_size)) ||
            (uri_a.length > 0 && !utf8_value(uri_a, row, &uri_null, &uri_ptr, &uri_len)) ||
            (ranged && pos_a.length > 0 && !uint64_value(pos_a, row, &pos_null, &position)) ||
            (ranged && size_a.length > 0 && !uint64_value(size_a, row, &size_null, &size))) {
            error = "failed to read the children of blob field " + blob_field.name;
            return false;
        }
        if (pos_null != size_null) {
            return row_error(row, "must set both `position` and `size`, or neither");
        }
        if (!pos_null && uri_null) {
            return row_error(row, "sets `position` and `size` but `uri` is null");
        }
        if (data_null == uri_null) {
            return row_error(row, "must set exactly one of `data` and `uri`");
        }
        if (!size_null && size == 0U) {
            return row_error(row, "external range `size` must be greater than zero");
        }
        record_validity(out, index, true);
        if (!data_null) {
            // Placed (inline, packed or dedicated) when its data file is written: the bytes ride along.
            if (!append_packed_row(out, kBlobKindPendingData, 0, static_cast<std::uint64_t>(data_size), 0, data_ptr,
                                   static_cast<std::size_t>(data_size), error)) {
                return false;
            }
            continue;
        }
        if (uri_len <= 0) {
            return row_error(row, "has an empty `uri`");
        }
        std::string uri(reinterpret_cast<const char*>(uri_ptr), static_cast<std::size_t>(uri_len));
        std::uint32_t blob_id = 0;
        if (dictionary_mode) {
            // Deduplicate the URI: assign a stable dictionary index and store it once.
            // The packed row then carries blob_id = index and an empty inline URI.
            const auto it = out.blob_v2.uri_to_id.find(uri);
            if (it == out.blob_v2.uri_to_id.end()) {
                blob_id = static_cast<std::uint32_t>(out.blob_v2.uri_dictionary.size());
                out.blob_v2.uri_dictionary.push_back(uri);
                out.blob_v2.uri_to_id.emplace(uri, blob_id);
            } else {
                blob_id = it->second;
            }
            uri.clear();
        }
        if (!append_packed_row(out, kBlobKindExternal, position, size, blob_id,
                               reinterpret_cast<const std::uint8_t*>(uri.data()), uri.size(), error)) {
            return false;
        }
    }
    return true;
}

// ---- Columnar build / view for the lance.blob.v2 external reference column ----------------------------

bool build_blob_v2_external_array(const std::uint64_t* positions, const std::uint64_t* sizes, std::size_t n,
                                  const char* shared_uri, ArrowArray& out_array, std::string& error) {
    error.clear();
    ArrowSchema schema;
    if (!build_blob_v2_payload_schema(schema, error)) {
        return false;
    }
    if (ArrowArrayInitFromSchema(&out_array, &schema, nullptr) != NANOARROW_OK) {
        error = "failed to init blob v2 external array";
        ArrowSchemaRelease(&schema);
        return false;
    }
    ArrowSchemaRelease(&schema);
    if (ArrowArrayStartAppending(&out_array) != NANOARROW_OK) {
        error = "failed to start appending blob v2 external array";
        ArrowArrayRelease(&out_array);
        return false;
    }
    const ArrowStringView uri_view{shared_uri,
                                   static_cast<std::int64_t>(shared_uri ? std::strlen(shared_uri) : 0)};
    for (std::size_t i = 0; i < n; ++i) {
        // Canonical child order data(0)/uri(1)/position(2)/size(3); external rows null the inline `data`.
        if (ArrowArrayAppendNull(out_array.children[0], 1) != NANOARROW_OK ||
            ArrowArrayAppendString(out_array.children[1], uri_view) != NANOARROW_OK ||
            ArrowArrayAppendUInt(out_array.children[2], positions[i]) != NANOARROW_OK ||
            ArrowArrayAppendUInt(out_array.children[3], sizes[i]) != NANOARROW_OK ||
            ArrowArrayFinishElement(&out_array) != NANOARROW_OK) {
            error = "failed to append blob v2 external row";
            ArrowArrayRelease(&out_array);
            return false;
        }
    }
    if (ArrowArrayFinishBuildingDefault(&out_array, nullptr) != NANOARROW_OK) {
        error = "failed to finalize blob v2 external array";
        ArrowArrayRelease(&out_array);
        return false;
    }
    return true;
}

bool BlobV2ColumnView::init(const ArrowSchema& payload_ref_schema, const ArrowArrayView& payload_ref_view,
                            std::string& error) {
    int pos = -1, sz = -1, uri = -1;
    for (std::int64_t i = 0; i < payload_ref_schema.n_children; ++i) {
        const char* nm = payload_ref_schema.children[i]->name;
        if (nm == nullptr) {
            continue;
        }
        if (std::strcmp(nm, "position") == 0) {
            pos = static_cast<int>(i);
        } else if (std::strcmp(nm, "size") == 0) {
            sz = static_cast<int>(i);
        } else if (std::strcmp(nm, "uri") == 0) {
            uri = static_cast<int>(i);
        }
    }
    if (pos < 0 || sz < 0 || uri < 0) {
        error = "payload_ref struct missing position/size/uri";
        return false;
    }
    pos_ = payload_ref_view.children[pos];
    size_ = payload_ref_view.children[sz];
    uri_ = payload_ref_view.children[uri];
    len_ = payload_ref_view.length;
    return true;
}

std::uint64_t BlobV2ColumnView::position(std::int64_t row) const {
    return ArrowArrayViewGetUIntUnsafe(pos_, row);
}

std::uint64_t BlobV2ColumnView::byte_size(std::int64_t row) const {
    return ArrowArrayViewGetUIntUnsafe(size_, row);
}

void BlobV2ColumnView::uri(std::int64_t row, const char** data, std::int64_t* size) const {
    const ArrowStringView sv = ArrowArrayViewGetStringUnsafe(uri_, row);
    *data = sv.data;
    *size = sv.size_bytes;
}

std::filesystem::path blob_v2_sidecar_path(const std::filesystem::path& data_file, const std::uint32_t blob_id) {
    std::uint32_t reversed = 0;
    for (int bit = 0; bit < 32; ++bit) {
        if ((blob_id >> bit) & 1U) {
            reversed |= 1U << (31 - bit);
        }
    }
    std::string name(32, '0');
    for (int bit = 0; bit < 32; ++bit) {
        if ((reversed >> (31 - bit)) & 1U) {
            name[static_cast<std::size_t>(bit)] = '1';
        }
    }
    return data_file.parent_path() / data_file.stem() / (name + ".blob");
}

bool blob_v2_locate(const BlobV2ExternalDescriptor& descriptor, const std::filesystem::path& data_file,
                    BlobV2Location& out, std::string& error) {
    out = BlobV2Location{};
    out.size = descriptor.size;
    switch (descriptor.kind) {
        case kBlobKindInline:
            out.file = data_file.string();
            out.position = descriptor.position;
            return true;
        case kBlobKindPacked:
            out.file = blob_v2_sidecar_path(data_file, descriptor.blob_id).string();
            out.position = descriptor.position;
            return true;
        case kBlobKindDedicated:
            out.file = blob_v2_sidecar_path(data_file, descriptor.blob_id).string();
            return true;
        case kBlobKindExternal:
            out.file = descriptor.blob_uri;
            out.external = true;
            out.position = descriptor.position;
            if (descriptor.size == 0U && !descriptor.blob_uri.empty()) {
                // Size 0: the blob is its object from `position` to the end, as Lance reads it.
                std::uint64_t object_size = 0;
                char message[512] = {0};
                if (nano_lance_external_blob_size(descriptor.blob_uri.c_str(), &object_size, message,
                                                  sizeof(message)) != NANO_LANCE_READER_OK) {
                    error = std::string("cannot size external blob ") + descriptor.blob_uri + ": " + message;
                    return false;
                }
                out.size = object_size >= descriptor.position ? object_size - descriptor.position : 0U;
            }
            return true;
        default:
            error = "unknown blob kind " + std::to_string(descriptor.kind);
            return false;
    }
}

bool blob_v2_read(const BlobV2Location& location, const std::uint64_t offset, const std::uint64_t length,
                  std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    if (offset > location.size || length > location.size - offset) {
        error = "read past the end of the blob";
        return false;
    }
    if (length == 0U) {
        return true;
    }
    if (location.position > std::numeric_limits<std::uint64_t>::max() - offset) {
        error = "blob position overflows";
        return false;
    }
    const std::uint64_t at = location.position + offset;
    if (length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        error = "blob too large to read into memory";
        return false;
    }
    out.resize(static_cast<std::size_t>(length));
    if (location.external) {
        std::size_t got = 0;
        char message[512] = {0};
        if (nano_lance_fetch_external_blob(location.file.c_str(), at, length, out.data(), out.size(), &got, message,
                                           sizeof(message)) != NANO_LANCE_READER_OK ||
            got != out.size()) {
            error = std::string("cannot read external blob ") + location.file + ": " + message;
            out.clear();
            return false;
        }
        return true;
    }
    std::ifstream in(location.file, std::ios::binary);
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(location.file, ec);
    if (!in || ec) {
        error = "cannot open blob file " + location.file;
        out.clear();
        return false;
    }
    if (at > file_size || length > file_size - at) {
        error = "blob range is past the end of " + location.file;
        out.clear();
        return false;
    }
    in.seekg(static_cast<std::streamoff>(at));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
    if (!in) {
        error = "cannot read blob file " + location.file;
        out.clear();
        return false;
    }
    return true;
}

void blob_append_legacy_row(ColumnValues& out, std::uint64_t position, std::uint64_t size, bool null) {
    out.kind = ColumnValues::Kind::BlobV2External;
    const auto row = out.blob_v2.row_packed_sizes.size();
    if (null || !out.validity.empty()) {
        if (out.validity.empty()) {
            out.validity.assign((row + 8U) / 8U, 0xFFU);
        }
        out.validity.resize((row + 8U) / 8U, 0xFFU);
        if (null) {
            out.validity[row / 8U] &= static_cast<std::uint8_t>(~(1U << (row % 8U)));
            ++out.null_count;
        } else {
            out.validity[row / 8U] |= static_cast<std::uint8_t>(1U << (row % 8U));
        }
    }
    if (null) {
        out.blob_v2.row_packed_sizes.push_back(0U);
        return;
    }
    BlobV2ExternalDescriptor descriptor;
    descriptor.kind = kBlobKindInline;
    descriptor.position = position;
    descriptor.size = size;
    const auto bytes = blob_v2_pack_descriptor_row(descriptor);
    out.blob_v2.packed_payload.insert(out.blob_v2.packed_payload.end(), bytes.begin(), bytes.end());
    out.blob_v2.row_packed_sizes.push_back(static_cast<std::uint32_t>(bytes.size()));
}

}  // namespace nano_lance
