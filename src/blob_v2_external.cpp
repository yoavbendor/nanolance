#include "nanolance/blob_v2_external.hpp"

#include "nanolance/array_accessor.hpp"
#include "nanolance/blob_builder.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
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

void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(value));
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
    const LanceField* blob_parent = nullptr;
    for (const auto& field : mapping.fields) {
        if (field.extension_name == kBlobV2ExtensionName && field.logical_type == "struct" && field.parent_id == -1) {
            blob_parent = &field;
            break;
        }
    }
    if (blob_parent == nullptr) {
        return true;
    }

    const auto* data_f = find_child(mapping, blob_parent->id, "data");
    const auto* uri_f = find_child(mapping, blob_parent->id, "uri");
    const auto* pos_f = find_child(mapping, blob_parent->id, "position");
    const auto* size_f = find_child(mapping, blob_parent->id, "size");
    if (data_f == nullptr || uri_f == nullptr || pos_f == nullptr || size_f == nullptr) {
        const auto* kind_f = find_child(mapping, blob_parent->id, "kind");
        if (kind_f != nullptr) {
            return true;
        }
        error = "lance.blob.v2 struct must have data, uri, position, and size children";
        return false;
    }

    std::int32_t blob_column_index = 0;
    for (const auto& field : mapping.fields) {
        if (field.parent_id == -1 && field.extension_name.empty() && field.column_index >= 0) {
            blob_column_index = std::max(blob_column_index, field.column_index + 1);
        }
    }

    std::vector<LanceField> rebuilt;
    rebuilt.reserve(mapping.fields.size() + 1U);

    for (const auto& field : mapping.fields) {
        if (field.parent_id == blob_parent->id) {
            continue;
        }
        if (field.id == blob_parent->id) {
            LanceField parent = field;
            parent.column_index = blob_column_index;
            parent.metadata["lance-encoding:packed"] = "true";
            parent.metadata["lance-encoding:blob"] = "true";
            rebuilt.push_back(parent);

            auto push_child = [&](const std::string& name, const std::string& logical, const std::string& format,
                                  std::int32_t new_id) {
                LanceField f;
                f.name = name;
                f.logical_type = logical;
                f.arrow_format = format;
                f.id = new_id;
                f.parent_id = blob_parent->id;
                f.column_index = -1;
                f.nullable = false;
                rebuilt.push_back(f);
            };

            const std::int32_t base = blob_parent->id + 1;
            std::int32_t nid = base;
            push_child("kind", "uint8", "C", nid++);
            push_child("position", "uint64", "L", nid++);
            push_child("size", "uint64", "L", nid++);
            push_child("blob_id", "uint32", "I", nid++);
            push_child("blob_uri", "string", "u", nid++);
            continue;
        }
        rebuilt.push_back(field);
    }

    mapping.fields = std::move(rebuilt);
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
    const auto* struct_array = resolve_field_array(batch, mapping, blob_field);
    if (struct_array == nullptr) {
        error = "missing Arrow struct array for blob field ";
        error += blob_field.name;
        return false;
    }

    const auto* data_f = find_child(mapping, blob_field.id, "data");
    const auto* uri_f = find_child(mapping, blob_field.id, "uri");
    const auto* pos_f = find_child(mapping, blob_field.id, "position");
    const auto* size_f = find_child(mapping, blob_field.id, "size");
    if (data_f == nullptr || uri_f == nullptr || pos_f == nullptr || size_f == nullptr) {
        error = "blob field is missing expected Arrow children";
        return false;
    }

    const auto* data_a = resolve_field_array(batch, mapping, *data_f);
    const auto* uri_a = resolve_field_array(batch, mapping, *uri_f);
    const auto* pos_a = resolve_field_array(batch, mapping, *pos_f);
    const auto* size_a = resolve_field_array(batch, mapping, *size_f);
    if (data_a == nullptr || uri_a == nullptr || pos_a == nullptr || size_a == nullptr) {
        error = "missing Arrow arrays for blob children";
        return false;
    }

    out.kind = ColumnValues::Kind::BlobV2External;
    if (out.blob_v2.packed_payload.empty()) {
        out.blob_v2.row_packed_sizes.clear();
    } else if (out.kind != ColumnValues::Kind::BlobV2External) {
        error = "blob v2 column append cannot merge with non-blob column values";
        return false;
    } else {
        out.blob_v2.row_packed_sizes.reserve(out.blob_v2.row_packed_sizes.size() +
                                            static_cast<std::size_t>(struct_array->length));
    }

    const auto rows = struct_array->length;
    for (std::int64_t row = 0; row < rows; ++row) {
        BlobV2ExternalDescriptor descriptor;
        if (!preprocess_blob_v2_external_row(*data_a, *uri_a, *pos_a, *size_a, row, descriptor, error)) {
            return false;
        }
        if (dictionary_mode) {
            // Deduplicate the URI: assign a stable dictionary index and store it once.
            // The packed row then carries blob_id = index and an empty inline URI.
            auto it = out.blob_v2.uri_to_id.find(descriptor.blob_uri);
            std::uint32_t id = 0;
            if (it == out.blob_v2.uri_to_id.end()) {
                id = static_cast<std::uint32_t>(out.blob_v2.uri_dictionary.size());
                out.blob_v2.uri_dictionary.push_back(descriptor.blob_uri);
                out.blob_v2.uri_to_id.emplace(descriptor.blob_uri, id);
            } else {
                id = it->second;
            }
            descriptor.blob_id = id;
            descriptor.blob_uri.clear();
        }
        const auto row_bytes = blob_v2_pack_descriptor_row(descriptor);
        out.blob_v2.row_packed_sizes.push_back(static_cast<std::uint32_t>(row_bytes.size()));
        out.blob_v2.packed_payload.insert(out.blob_v2.packed_payload.end(), row_bytes.begin(), row_bytes.end());
    }

    return true;
}

}  // namespace nano_lance
