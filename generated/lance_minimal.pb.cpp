#include "lance_minimal.pb.hpp"

#include <cstddef>
#include <limits>
#include <utility>

namespace nano_lance::pb {
namespace {

constexpr std::uint8_t kWireVarint = 0;
constexpr std::uint8_t kWireBytes = 2;

void write_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

bool read_varint(const std::vector<std::uint8_t>& data, std::size_t& pos, std::uint64_t& value) {
    value = 0;
    for (std::uint32_t shift = 0; shift <= 63U; shift += 7U) {
        if (pos >= data.size()) {
            return false;
        }
        const auto byte = data[pos++];
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U) {
            return true;
        }
    }
    return false;
}

void write_key(std::vector<std::uint8_t>& out, std::uint32_t field_number, std::uint8_t wire_type) {
    write_varint(out, (static_cast<std::uint64_t>(field_number) << 3U) | wire_type);
}

void write_uint64(std::vector<std::uint8_t>& out, std::uint32_t field_number, std::uint64_t value) {
    write_key(out, field_number, kWireVarint);
    write_varint(out, value);
}

void write_int32(std::vector<std::uint8_t>& out, std::uint32_t field_number, std::int32_t value) {
    write_key(out, field_number, kWireVarint);
    write_varint(out, static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
}

void write_bool(std::vector<std::uint8_t>& out, std::uint32_t field_number, bool value) {
    write_uint64(out, field_number, value ? 1U : 0U);
}

void write_packed_uint64(std::vector<std::uint8_t>& out, std::uint32_t field_number, const std::vector<std::uint64_t>& values) {
    std::vector<std::uint8_t> packed;
    for (const auto value : values) {
        write_varint(packed, value);
    }
    write_key(out, field_number, kWireBytes);
    write_varint(out, packed.size());
    out.insert(out.end(), packed.begin(), packed.end());
}

void write_string(std::vector<std::uint8_t>& out, std::uint32_t field_number, const std::string& value) {
    write_key(out, field_number, kWireBytes);
    write_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void write_message(std::vector<std::uint8_t>& out, std::uint32_t field_number, const std::vector<std::uint8_t>& value) {
    write_key(out, field_number, kWireBytes);
    write_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> encode_direct_encoding(const std::vector<std::uint8_t>& encoding) {
    std::vector<std::uint8_t> direct;
    write_key(direct, 1, kWireBytes);
    write_varint(direct, encoding.size());
    direct.insert(direct.end(), encoding.begin(), encoding.end());

    std::vector<std::uint8_t> out;
    write_message(out, 2, direct);
    return out;
}

/// Inverse of encode_direct_encoding: unwrap Encoding{ f2 direct = DirectEncoding{ f1 encoding } }
/// back to the raw descriptor bytes, so ColumnPage::encoding round-trips exactly what was written.
bool decode_direct_encoding(const std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& out);

bool read_bytes(const std::vector<std::uint8_t>& data, std::size_t& pos, std::vector<std::uint8_t>& value) {
    std::uint64_t length = 0;
    if (!read_varint(data, pos, length) || length > data.size() - pos) {
        return false;
    }
    value.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
                 data.begin() + static_cast<std::ptrdiff_t>(pos + length));
    pos += static_cast<std::size_t>(length);
    return true;
}

bool read_string(const std::vector<std::uint8_t>& data, std::size_t& pos, std::string& value) {
    std::vector<std::uint8_t> bytes;
    if (!read_bytes(data, pos, bytes)) {
        return false;
    }
    value.assign(bytes.begin(), bytes.end());
    return true;
}

bool skip_field(const std::vector<std::uint8_t>& data, std::size_t& pos, std::uint8_t wire_type) {
    if (wire_type == kWireVarint) {
        std::uint64_t ignored = 0;
        return read_varint(data, pos, ignored);
    }
    if (wire_type == kWireBytes) {
        std::vector<std::uint8_t> ignored;
        return read_bytes(data, pos, ignored);
    }
    // Wire type 1 = 64-bit fixed (8 bytes), wire type 5 = 32-bit fixed (4 bytes).
    // Both are valid protobuf wire types that newer Lance manifests may use in unknown fields.
    if (wire_type == 1U) {
        if (pos + 8U > data.size()) { return false; }
        pos += 8U;
        return true;
    }
    if (wire_type == 5U) {
        if (pos + 4U > data.size()) { return false; }
        pos += 4U;
        return true;
    }
    return false;
}

/// Skip a field this codec does not model, appending its wire bytes (key included) to `unknown` so an
/// encoder can write it back.
bool keep_field(const std::vector<std::uint8_t>& data, std::size_t key_start, std::size_t& pos, std::uint8_t wire_type,
                std::vector<std::uint8_t>& unknown) {
    if (!skip_field(data, pos, wire_type)) {
        return false;
    }
    unknown.insert(unknown.end(), data.begin() + static_cast<std::ptrdiff_t>(key_start),
                   data.begin() + static_cast<std::ptrdiff_t>(pos));
    return true;
}

void write_string_map_entry(std::vector<std::uint8_t>& out, std::uint32_t field_number, const std::string& key,
                            const std::string& value) {
    std::vector<std::uint8_t> entry;
    write_string(entry, 1, key);
    write_string(entry, 2, value);
    write_message(out, field_number, entry);
}

std::vector<std::uint8_t> encode_data_storage_format(const DataStorageFormat& format) {
    std::vector<std::uint8_t> out;
    write_string(out, 1, format.file_format);
    write_string(out, 2, format.version);
    return out;
}

std::vector<std::uint8_t> encode_field_message(const Field& field) {
    std::vector<std::uint8_t> out;
    // proto3: a field is omitted only when it equals ZERO. This used to omit `type` when it was 2
    // (LEAF) -- the struct's default -- so any proto3 reader decoded every nanolance leaf as PARENT.
    // Harmless in practice (Lance neither writes nor reads Field.type), but the same shape of bug as
    // `parent_id`, which was not harmless.
    if (field.type != 0) {
        write_int32(out, 1, field.type);
    }
    write_string(out, 2, field.name);
    if (field.id != 0) {
        write_int32(out, 3, field.id);
    }
    write_int32(out, 4, field.parent_id);
    write_string(out, 5, field.logical_type);
    if (field.nullable) {
        write_bool(out, 6, field.nullable);
    }
    if (field.encoding != 0) {
        write_int32(out, 7, field.encoding);
    }
    for (const auto& meta : field.metadata) {
        std::vector<std::uint8_t> map_entry;
        write_string(map_entry, 1, meta.first);
        write_key(map_entry, 2, kWireBytes);
        write_varint(map_entry, meta.second.size());
        map_entry.insert(map_entry.end(), meta.second.begin(), meta.second.end());
        write_message(out, 10, map_entry);
    }
    out.insert(out.end(), field.unknown.begin(), field.unknown.end());
    return out;
}

std::vector<std::uint8_t> encode_schema_message(const std::vector<Field>& fields) {
    std::vector<std::uint8_t> out;
    for (const auto& field : fields) {
        write_message(out, 1, encode_field_message(field));
    }
    return out;
}

std::vector<std::uint8_t> encode_data_file_message(const DataFile& file) {
    std::vector<std::uint8_t> out;
    write_string(out, 1, file.path);
    for (const auto field_id : file.fields) {
        write_int32(out, 2, field_id);
    }
    for (const auto column_index : file.column_indices) {
        write_int32(out, 3, column_index);
    }
    write_uint64(out, 4, file.file_major_version);
    write_uint64(out, 5, file.file_minor_version);
    write_uint64(out, 6, file.file_size_bytes);
    out.insert(out.end(), file.unknown.begin(), file.unknown.end());
    return out;
}

std::vector<std::uint8_t> encode_deletion_file_message(const DeletionFile& file) {
    std::vector<std::uint8_t> out;
    if (file.file_type != 0U) {
        write_uint64(out, 1, file.file_type);
    }
    write_uint64(out, 2, file.read_version);
    write_uint64(out, 3, file.id);
    write_uint64(out, 4, file.num_deleted_rows);
    out.insert(out.end(), file.unknown.begin(), file.unknown.end());
    return out;
}

std::vector<std::uint8_t> encode_data_fragment_message(const DataFragment& fragment) {
    std::vector<std::uint8_t> out;
    write_uint64(out, 1, fragment.id);
    for (const auto& file : fragment.files) {
        write_message(out, 2, encode_data_file_message(file));
    }
    // The deletion file was not written back here at one time, so appending to a dataset with deleted
    // rows brought them back.
    if (fragment.deletion_file.present) {
        write_message(out, 3, encode_deletion_file_message(fragment.deletion_file));
    }
    write_uint64(out, 4, fragment.physical_rows);
    out.insert(out.end(), fragment.unknown.begin(), fragment.unknown.end());
    return out;
}

bool decode_data_storage_format(const std::vector<std::uint8_t>& bytes, DataStorageFormat& format) {
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (field_number == 1 && wire_type == kWireBytes) {
            if (!read_string(bytes, pos, format.file_format)) {
                return false;
            }
        } else if (field_number == 2 && wire_type == kWireBytes) {
            if (!read_string(bytes, pos, format.version)) {
                return false;
            }
        } else if (!skip_field(bytes, pos, wire_type)) {
            return false;
        }
    }
    return true;
}

bool decode_direct_encoding(const std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& out) {
    // Two nested single-field messages: Encoding f2 -> DirectEncoding f1 -> the descriptor bytes.
    std::size_t pos = 0;
    std::vector<std::uint8_t> direct;
    bool have_direct = false;
    while (pos < bytes.size()) {
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (field_number == 2 && wire_type == kWireBytes) {
            if (!read_bytes(bytes, pos, direct)) {
                return false;
            }
            have_direct = true;
        } else if (!skip_field(bytes, pos, wire_type)) {
            return false;
        }
    }
    if (!have_direct) {
        return true;  // no direct encoding present; leave `out` empty
    }
    pos = 0;
    while (pos < direct.size()) {
        std::uint64_t key = 0;
        if (!read_varint(direct, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (field_number == 1 && wire_type == kWireBytes) {
            if (!read_bytes(direct, pos, out)) {
                return false;
            }
        } else if (!skip_field(direct, pos, wire_type)) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> encode_column_page(const ColumnPage& page) {
    std::vector<std::uint8_t> out;
    write_packed_uint64(out, 1, page.buffer_offsets);
    write_packed_uint64(out, 2, page.buffer_sizes);
    write_uint64(out, 3, page.length);
    if (!page.encoding.empty()) {
        write_message(out, 4, encode_direct_encoding(page.encoding));
    }
    if (page.priority != 0) {
        write_uint64(out, 5, page.priority);
    }
    return out;
}

bool decode_column_page(const std::vector<std::uint8_t>& bytes, ColumnPage& page) {
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::uint64_t key = 0;
        std::uint64_t value = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            if (field_number == 1) {
                page.buffer_offsets.push_back(value);
            } else if (field_number == 2) {
                page.buffer_sizes.push_back(value);
            } else if (field_number == 3) {
                page.length = value;
            } else if (field_number == 5) {
                page.priority = value;
            }
        } else if (wire_type == kWireBytes && field_number == 4) {
            // The /lance.encodings21.PageLayout descriptor. This was written and then skipped on
            // read for as long as the reader existed, which is why decode dispatched on nanolance's
            // private `nanolance:packing` field metadata instead -- and why a file from the Rust
            // lance crate, carrying no such metadata, could not be decoded. Keep the bytes;
            // nanolance/page_layout.hpp parses them.
            std::vector<std::uint8_t> wrapped;
            if (!read_bytes(bytes, pos, wrapped) || !decode_direct_encoding(wrapped, page.encoding)) {
                return false;
            }
        } else if (wire_type == kWireBytes && (field_number == 1 || field_number == 2)) {
            std::vector<std::uint8_t> packed;
            if (!read_bytes(bytes, pos, packed)) {
                return false;
            }
            std::size_t packed_pos = 0;
            while (packed_pos < packed.size()) {
                if (!read_varint(packed, packed_pos, value)) {
                    return false;
                }
                if (field_number == 1) {
                    page.buffer_offsets.push_back(value);
                } else {
                    page.buffer_sizes.push_back(value);
                }
            }
        } else if (!skip_field(bytes, pos, wire_type)) {
            return false;
        }
    }
    return true;
}

bool decode_map_metadata_entry(const std::vector<std::uint8_t>& nested, std::string& map_key, std::vector<std::uint8_t>& map_value) {
    map_key.clear();
    map_value.clear();
    // Only the key is tracked: the comment at the return explains why a missing value is legal, and
    // nothing reads a have_value flag.
    bool have_key = false;
    std::size_t pos = 0;
    while (pos < nested.size()) {
        std::uint64_t key = 0;
        if (!read_varint(nested, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (field_number == 1 && wire_type == kWireBytes) {
            if (!read_string(nested, pos, map_key)) {
                return false;
            }
            have_key = true;
        } else if (field_number == 2 && wire_type == kWireBytes) {
            if (!read_bytes(nested, pos, map_value)) {
                return false;
            }
        } else if (!skip_field(nested, pos, wire_type)) {
            return false;
        }
    }
    // A map entry with only a key (no value field) is valid protobuf: missing value defaults
    // to the zero value for its type, which for `bytes` is an empty buffer.
    return have_key;
}

bool decode_field_message(const std::vector<std::uint8_t>& bytes, Field& field) {
    field = Field{};
    // proto3 does not put a zero on the wire, so an ABSENT `parent_id` means 0 -- "my parent is the
    // field whose id is 0" -- not "I have no parent". Lance writes -1 for a root field, and -1 is
    // non-zero, so it is always serialized; only the 0 is ever implied.
    //
    // The struct default is -1 because the writer side wants it, so the read side has to say 0 here
    // explicitly. Getting this backwards made every child of field 0 decode as a root: a pylance
    // dataset whose FIRST column was a struct lost that struct's children and failed with "struct
    // field has no children in mapping", while the same struct in second position read fine.
    field.parent_id = 0;
    // Same rule for the two other fields whose struct default is not zero: absent means 0. For
    // `type` that is PARENT, for `encoding` it is NONE -- which is what Lance writes for a struct
    // parent. Nothing on the read path consults either for correctness today; this keeps the next
    // reader of them from inheriting the parent_id mistake.
    field.type = 0;
    field.encoding = 0;
    std::size_t pos = 0;
    bool nullable_wire_seen = false;
    while (pos < bytes.size()) {
        const std::size_t key_start = pos;
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        std::uint64_t value = 0;
        if (field_number == 1 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            field.type = static_cast<std::int32_t>(value);
        } else if (field_number == 2 && wire_type == kWireBytes) {
            if (!read_string(bytes, pos, field.name)) {
                return false;
            }
        } else if (field_number == 3 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            field.id = static_cast<std::int32_t>(value);
        } else if (field_number == 4 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            field.parent_id = static_cast<std::int32_t>(value);
        } else if (field_number == 5 && wire_type == kWireBytes) {
            if (!read_string(bytes, pos, field.logical_type)) {
                return false;
            }
        } else if (field_number == 6 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            nullable_wire_seen = true;
            field.nullable = value != 0U;
        } else if (field_number == 7 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            field.encoding = static_cast<std::int32_t>(value);
        } else if (field_number == 10 && wire_type == kWireBytes) {
            std::vector<std::uint8_t> nested;
            if (!read_bytes(bytes, pos, nested)) {
                return false;
            }
            std::string mk;
            std::vector<std::uint8_t> mv;
            if (!decode_map_metadata_entry(nested, mk, mv)) {
                return false;
            }
            field.metadata[std::move(mk)] = std::move(mv);
        } else if (!keep_field(bytes, key_start, pos, wire_type, field.unknown)) {
            return false;
        }
    }
    // encode_field_message omits wire field 6 when nullable is false; default struct value is true.
    if (!nullable_wire_seen) {
        field.nullable = false;
    }
    // A field cannot be its own parent. This is field 0 with an absent `parent_id`, i.e. the root
    // struct of a schema whose writer left the zero off the wire; it is a root.
    if (field.parent_id == field.id) {
        field.parent_id = -1;
    }
    return true;
}

bool decode_data_file_message(const std::vector<std::uint8_t>& bytes, DataFile& file) {
    file = DataFile{};
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const std::size_t key_start = pos;
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        std::uint64_t value = 0;
        if (field_number == 1 && wire_type == kWireBytes) {
            if (!read_string(bytes, pos, file.path)) {
                return false;
            }
        } else if (field_number == 2 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                return false;
            }
            file.fields.push_back(static_cast<std::int32_t>(value));
        } else if (field_number == 2 && wire_type == kWireBytes) {
            // packed repeated int32 — proto3 default encoding for numeric repeated fields
            std::vector<std::uint8_t> packed;
            if (!read_bytes(bytes, pos, packed)) { return false; }
            std::size_t pp = 0;
            while (pp < packed.size()) {
                if (!read_varint(packed, pp, value)) { return false; }
                if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) { return false; }
                file.fields.push_back(static_cast<std::int32_t>(value));
            }
        } else if (field_number == 3 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                return false;
            }
            file.column_indices.push_back(static_cast<std::int32_t>(value));
        } else if (field_number == 3 && wire_type == kWireBytes) {
            // packed repeated int32
            std::vector<std::uint8_t> packed;
            if (!read_bytes(bytes, pos, packed)) { return false; }
            std::size_t pp = 0;
            while (pp < packed.size()) {
                if (!read_varint(packed, pp, value)) { return false; }
                if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) { return false; }
                file.column_indices.push_back(static_cast<std::int32_t>(value));
            }
        } else if (field_number == 4 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
                return false;
            }
            file.file_major_version = static_cast<std::uint32_t>(value);
        } else if (field_number == 5 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
                return false;
            }
            file.file_minor_version = static_cast<std::uint32_t>(value);
        } else if (field_number == 6 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            file.file_size_bytes = value;
        } else if (!keep_field(bytes, key_start, pos, wire_type, file.unknown)) {
            return false;
        }
    }
    return true;
}

bool decode_deletion_file_message(const std::vector<std::uint8_t>& bytes, DeletionFile& out) {
    out = DeletionFile{};
    out.present = true;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const std::size_t key_start = pos;
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        std::uint64_t value = 0;
        if (field_number == 1 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            out.file_type = static_cast<std::uint32_t>(value);
        } else if (field_number == 2 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            out.read_version = value;
        } else if (field_number == 3 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            out.id = value;
        } else if (field_number == 4 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            out.num_deleted_rows = value;
        } else if (!keep_field(bytes, key_start, pos, wire_type, out.unknown)) {
            return false;
        }
    }
    return true;
}

bool decode_data_fragment_message(const std::vector<std::uint8_t>& bytes, DataFragment& fragment) {
    fragment = DataFragment{};
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const std::size_t key_start = pos;
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        std::uint64_t value = 0;
        if (field_number == 1 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            fragment.id = value;
        } else if (field_number == 2 && wire_type == kWireBytes) {
            std::vector<std::uint8_t> nested;
            if (!read_bytes(bytes, pos, nested)) {
                return false;
            }
            DataFile df;
            if (!decode_data_file_message(nested, df)) {
                return false;
            }
            fragment.files.push_back(std::move(df));
        } else if (field_number == 3 && wire_type == kWireBytes) {
            std::vector<std::uint8_t> nested;
            if (!read_bytes(bytes, pos, nested)) {
                return false;
            }
            if (!decode_deletion_file_message(nested, fragment.deletion_file)) {
                return false;
            }
        } else if (field_number == 4 && wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            fragment.physical_rows = value;
        } else if (!keep_field(bytes, key_start, pos, wire_type, fragment.unknown)) {
            return false;
        }
    }
    return true;
}

bool decode_string_map_entry(const std::vector<std::uint8_t>& nested, std::string& key, std::string& value) {
    std::vector<std::uint8_t> bytes;
    if (!decode_map_metadata_entry(nested, key, bytes)) {
        return false;
    }
    value.assign(bytes.begin(), bytes.end());
    return true;
}

bool decode_timestamp(const std::vector<std::uint8_t>& bytes, Manifest& manifest) {
    std::size_t pos = 0;
    manifest.has_timestamp = true;
    while (pos < bytes.size()) {
        std::uint64_t key = 0;
        std::uint64_t value = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (wire_type == kWireVarint && (field_number == 1 || field_number == 2)) {
            if (!read_varint(bytes, pos, value)) {
                return false;
            }
            if (field_number == 1) {
                manifest.timestamp_seconds = static_cast<std::int64_t>(value);
            } else {
                manifest.timestamp_nanos = static_cast<std::int32_t>(value);
            }
        } else if (!skip_field(bytes, pos, wire_type)) {
            return false;
        }
    }
    return true;
}

bool decode_writer_version(const std::vector<std::uint8_t>& bytes, Manifest& manifest) {
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (wire_type == kWireBytes && field_number == 1) {
            if (!read_string(bytes, pos, manifest.writer_library)) {
                return false;
            }
        } else if (wire_type == kWireBytes && field_number == 2) {
            if (!read_string(bytes, pos, manifest.writer_version)) {
                return false;
            }
        } else if (!skip_field(bytes, pos, wire_type)) {
            return false;
        }
    }
    return true;
}

bool decode_manifest_message(const std::vector<std::uint8_t>& bytes, Manifest& manifest) {
    manifest = Manifest{};
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const std::size_t key_start = pos;
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        std::uint64_t v = 0;
        std::vector<std::uint8_t> nested;
        if (field_number == 1 && wire_type == kWireBytes) {
            if (!read_bytes(bytes, pos, nested)) {
                return false;
            }
            Field f;
            if (!decode_field_message(nested, f)) {
                return false;
            }
            manifest.fields.push_back(std::move(f));
        } else if (field_number == 2 && wire_type == kWireBytes) {
            if (!read_bytes(bytes, pos, nested)) {
                return false;
            }
            DataFragment frag;
            if (!decode_data_fragment_message(nested, frag)) {
                return false;
            }
            manifest.fragments.push_back(std::move(frag));
        } else if (field_number == 3 && wire_type == kWireVarint) {
            if (!read_varint(bytes, pos, manifest.version)) {
                return false;
            }
        } else if (field_number == 5 && wire_type == kWireBytes) {
            std::string k;
            std::vector<std::uint8_t> value;
            if (!read_bytes(bytes, pos, nested) || !decode_map_metadata_entry(nested, k, value)) {
                return false;
            }
            manifest.schema_metadata[std::move(k)] = std::move(value);
        } else if (field_number == 7 && wire_type == kWireBytes) {
            if (!read_bytes(bytes, pos, nested) || !decode_timestamp(nested, manifest)) {
                return false;
            }
        } else if (field_number == 8 && wire_type == kWireBytes) {
            if (!read_string(bytes, pos, manifest.tag)) {
                return false;
            }
        } else if (field_number == 9 && wire_type == kWireVarint) {
            if (!read_varint(bytes, pos, manifest.reader_feature_flags)) {
                return false;
            }
        } else if (field_number == 10 && wire_type == kWireVarint) {
            if (!read_varint(bytes, pos, manifest.writer_feature_flags)) {
                return false;
            }
        } else if (field_number == 11 && wire_type == kWireVarint) {
            if (!read_varint(bytes, pos, v)) {
                return false;
            }
            if (v > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
                return false;
            }
            manifest.has_max_fragment_id = true;
            manifest.max_fragment_id = static_cast<std::uint32_t>(v);
        } else if (field_number == 12 && wire_type == kWireBytes) {
            if (!read_string(bytes, pos, manifest.transaction_file)) {
                return false;
            }
        } else if (field_number == 13 && wire_type == kWireBytes) {
            if (!read_bytes(bytes, pos, nested) || !decode_writer_version(nested, manifest)) {
                return false;
            }
        } else if (field_number == 14 && wire_type == kWireVarint) {
            if (!read_varint(bytes, pos, manifest.next_row_id)) {
                return false;
            }
        } else if (field_number == 15 && wire_type == kWireBytes) {
            if (!read_bytes(bytes, pos, nested) || !decode_data_storage_format(nested, manifest.data_format)) {
                return false;
            }
        } else if ((field_number == 16 || field_number == 19) && wire_type == kWireBytes) {
            std::string k;
            std::string value;
            if (!read_bytes(bytes, pos, nested) || !decode_string_map_entry(nested, k, value)) {
                return false;
            }
            (field_number == 16 ? manifest.config : manifest.table_metadata)[std::move(k)] = std::move(value);
        } else if (field_number == 4 || field_number == 6 || field_number == 21) {
            // Positions inside the manifest file this came from: meaningless in any other file.
            if (!skip_field(bytes, pos, wire_type)) {
                return false;
            }
        } else if (!keep_field(bytes, key_start, pos, wire_type, manifest.unknown)) {
            return false;
        }
    }
    return true;
}

bool decode_schema_message(const std::vector<std::uint8_t>& bytes, std::vector<Field>& fields) {
    fields.clear();
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (field_number == 1 && wire_type == kWireBytes) {
            std::vector<std::uint8_t> nested;
            if (!read_bytes(bytes, pos, nested)) {
                return false;
            }
            Field f;
            if (!decode_field_message(nested, f)) {
                return false;
            }
            fields.push_back(std::move(f));
        } else if (!skip_field(bytes, pos, wire_type)) {
            return false;
        }
    }
    return true;
}

bool decode_file_descriptor_message(const std::vector<std::uint8_t>& bytes, FileDescriptor& descriptor) {
    descriptor = FileDescriptor{};
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (field_number == 1 && wire_type == kWireBytes) {
            std::vector<std::uint8_t> nested;
            if (!read_bytes(bytes, pos, nested)) {
                return false;
            }
            if (!decode_schema_message(nested, descriptor.fields)) {
                return false;
            }
        } else if (field_number == 2 && wire_type == kWireVarint) {
            if (!read_varint(bytes, pos, descriptor.length)) {
                return false;
            }
        } else if (!skip_field(bytes, pos, wire_type)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<std::uint8_t> encode_file_descriptor(const FileDescriptor& descriptor) {
    std::vector<std::uint8_t> out;
    write_message(out, 1, encode_schema_message(descriptor.fields));
    write_uint64(out, 2, descriptor.length);
    return out;
}

bool decode_file_descriptor(const std::vector<std::uint8_t>& bytes, FileDescriptor& descriptor) {
    return decode_file_descriptor_message(bytes, descriptor);
}

std::vector<std::uint8_t> encode_manifest(const Manifest& manifest) {
    std::vector<std::uint8_t> out;
    for (const auto& field : manifest.fields) {
        write_message(out, 1, encode_field_message(field));
    }
    for (const auto& fragment : manifest.fragments) {
        write_message(out, 2, encode_data_fragment_message(fragment));
    }
    write_uint64(out, 3, manifest.version);
    for (const auto& meta : manifest.schema_metadata) {
        std::vector<std::uint8_t> entry;
        write_string(entry, 1, meta.first);
        write_key(entry, 2, kWireBytes);
        write_varint(entry, meta.second.size());
        entry.insert(entry.end(), meta.second.begin(), meta.second.end());
        write_message(out, 5, entry);
    }
    if (manifest.has_timestamp) {
        std::vector<std::uint8_t> ts;
        if (manifest.timestamp_seconds != 0) {
            write_uint64(ts, 1, static_cast<std::uint64_t>(manifest.timestamp_seconds));
        }
        if (manifest.timestamp_nanos != 0) {
            write_uint64(ts, 2, static_cast<std::uint64_t>(manifest.timestamp_nanos));
        }
        write_message(out, 7, ts);
    }
    if (!manifest.tag.empty()) {
        write_string(out, 8, manifest.tag);
    }
    if (manifest.reader_feature_flags != 0U) {
        write_uint64(out, 9, manifest.reader_feature_flags);
    }
    if (manifest.writer_feature_flags != 0U) {
        write_uint64(out, 10, manifest.writer_feature_flags);
    }
    if (manifest.has_max_fragment_id) {
        write_uint64(out, 11, manifest.max_fragment_id);
    }
    if (!manifest.transaction_file.empty()) {
        write_string(out, 12, manifest.transaction_file);
    }
    if (!manifest.writer_library.empty() || !manifest.writer_version.empty()) {
        std::vector<std::uint8_t> wv;
        write_string(wv, 1, manifest.writer_library);
        write_string(wv, 2, manifest.writer_version);
        write_message(out, 13, wv);
    }
    if (manifest.next_row_id != 0U) {
        write_uint64(out, 14, manifest.next_row_id);
    }
    write_message(out, 15, encode_data_storage_format(manifest.data_format));
    for (const auto& kv : manifest.config) {
        write_string_map_entry(out, 16, kv.first, kv.second);
    }
    for (const auto& kv : manifest.table_metadata) {
        write_string_map_entry(out, 19, kv.first, kv.second);
    }
    out.insert(out.end(), manifest.unknown.begin(), manifest.unknown.end());
    return out;
}

bool decode_manifest(const std::vector<std::uint8_t>& bytes, Manifest& manifest) {
    return decode_manifest_message(bytes, manifest);
}

std::vector<std::uint8_t> encode_data_fragment(const DataFragment& fragment) {
    return encode_data_fragment_message(fragment);
}

bool decode_data_fragment(const std::vector<std::uint8_t>& bytes, DataFragment& fragment) {
    return decode_data_fragment_message(bytes, fragment);
}

std::vector<std::uint8_t> encode_data_file(const DataFile& file) {
    return encode_data_file_message(file);
}

bool decode_data_file(const std::vector<std::uint8_t>& bytes, DataFile& file) {
    return decode_data_file_message(bytes, file);
}

bool decode_field(const std::vector<std::uint8_t>& bytes, Field& field) {
    return decode_field_message(bytes, field);
}

std::vector<std::uint8_t> encode_metadata(const Metadata& metadata) {
    std::vector<std::uint8_t> out;
    for (const auto offset : metadata.batch_offsets) {
        write_uint64(out, 2, static_cast<std::uint64_t>(offset));
    }
    write_uint64(out, 3, metadata.page_table_position);
    return out;
}

bool decode_metadata(const std::vector<std::uint8_t>& bytes, Metadata& metadata) {
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::uint64_t key = 0;
        std::uint64_t value = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (wire_type == kWireVarint && read_varint(bytes, pos, value)) {
            if (field_number == 2 && value <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                metadata.batch_offsets.push_back(static_cast<std::int32_t>(value));
            } else if (field_number == 3) {
                metadata.page_table_position = value;
            }
        } else if (!skip_field(bytes, pos, wire_type)) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> encode_column_metadata(const ColumnMetadata& metadata) {
    std::vector<std::uint8_t> out;
    if (!metadata.encoding.empty()) {
        write_message(out, 1, encode_direct_encoding(metadata.encoding));
    }
    for (const auto& page : metadata.pages) {
        write_message(out, 2, encode_column_page(page));
    }
    return out;
}

bool decode_column_metadata(const std::vector<std::uint8_t>& bytes, ColumnMetadata& metadata) {
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::uint64_t key = 0;
        if (!read_varint(bytes, pos, key)) {
            return false;
        }
        const auto field_number = static_cast<std::uint32_t>(key >> 3U);
        const auto wire_type = static_cast<std::uint8_t>(key & 0x07U);
        if (field_number == 2 && wire_type == kWireBytes) {
            std::vector<std::uint8_t> nested;
            ColumnPage page;
            if (!read_bytes(bytes, pos, nested) || !decode_column_page(nested, page)) {
                return false;
            }
            metadata.pages.push_back(std::move(page));
        } else if (!skip_field(bytes, pos, wire_type)) {
            return false;
        }
    }
    return true;
}

}  // namespace nano_lance::pb
