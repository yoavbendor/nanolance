// Lance's protobuf metadata, read and written with nanom.
//
// The wire format lives in one place: nanom/formats/lance_protobuf.hpp declares each Lance message
// once (field numbers and types), and nanom/protobuf.hpp + nanom/protobuf_encode.hpp read and
// write it. Writing is canonical proto3 (what Lance's own prost writer produces): zero / empty
// implicit fields are left out and repeated scalars are packed. Reading accepts both packed and
// unpacked repeated scalars and rejects malformed or truncated messages.
//
// Fields this codec does not model are not lost: the messages a commit hands back (Manifest,
// DataFragment, DataFile, DeletionFile, Field, IndexMetadata) keep them in the model's pb_unknown
// member, and this file carries them in each struct's `unknown` bytes, so a manifest written by
// another Lance writer keeps what nanolance does not understand.
//
// This file only converts between that wire model (views into the bytes) and nanolance's owned
// structs in lance_minimal.pb.hpp, applying the few rules that are nanolance's, not the wire's
// (marked below).

#include "lance_minimal.pb.hpp"
#include "nanolance/roaring_bitmap.hpp"

#include <nanom/formats/lance_protobuf.hpp>
#include <nanom/protobuf_encode.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

namespace nano_lance::pb {
namespace {

namespace nm = ::nanom;
namespace wire = ::nanom_formats::lance;

// --- shared helpers ---------------------------------------------------------------------------

nm::bytes view(const std::vector<std::uint8_t>& b) {
    return nm::bytes(reinterpret_cast<const std::byte*>(b.data()), b.size());
}
std::vector<std::uint8_t> owned(nm::bytes b) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(b.data());
    return std::vector<std::uint8_t>(p, p + b.size());
}
std::string owned(std::string_view s) { return std::string(s); }

/// pb_unknown <-> nanolance's `unknown` bytes.
nm::unknown_fields to_unknown(const std::vector<std::uint8_t>& u) {
    nm::unknown_fields out;
    const auto* p = reinterpret_cast<const std::byte*>(u.data());
    out.bytes.assign(p, p + u.size());
    return out;
}
std::vector<std::uint8_t> from_unknown(const nm::unknown_fields& u) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(u.bytes.data());
    return std::vector<std::uint8_t>(p, p + u.bytes.size());
}

/// nanom byte sink appending to nanolance's byte vectors.
struct u8_sink {
    std::vector<std::uint8_t>* out;
    bool put(const std::byte* p, std::size_t n) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(p);
        out->insert(out->end(), b, b + n);
        return true;
    }
};

template <class M>
std::vector<std::uint8_t> encode(const M& m) {
    std::vector<std::uint8_t> out;
    u8_sink sink{&out};
    if (!nm::protobuf_encode(m, sink)) {
        out.clear();  // only on limits no Lance metadata reaches (2 GiB, nesting depth)
    }
    return out;
}

template <class M>
bool decode(const std::uint8_t* data, std::size_t size, M& out) {
    const auto in = nm::from(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size));
    auto r = nm::protobuf<M>()(in);
    if (!r) {
        return false;
    }
    out = std::move(r->value);
    return true;
}
template <class M>
bool decode(const std::vector<std::uint8_t>& bytes, M& out) {
    return decode(bytes.data(), bytes.size(), out);
}

std::vector<wire::MetadataEntry> to_wire(const std::map<std::string, std::vector<std::uint8_t>>& map) {
    std::vector<wire::MetadataEntry> out;
    out.reserve(map.size());
    for (const auto& [key, value] : map) {
        wire::MetadataEntry e;
        e.key = std::string_view(key);
        e.value = view(value);
        out.push_back(e);
    }
    return out;
}
void from_wire(const std::vector<wire::MetadataEntry>& entries, std::map<std::string, std::vector<std::uint8_t>>& out) {
    for (const auto& e : entries) {
        out[owned(*e.key)] = owned(*e.value);
    }
}
std::vector<wire::StringEntry> to_wire(const std::map<std::string, std::string>& map) {
    std::vector<wire::StringEntry> out;
    out.reserve(map.size());
    for (const auto& [key, value] : map) {
        wire::StringEntry e;
        e.key = std::string_view(key);
        e.value = std::string_view(value);
        out.push_back(e);
    }
    return out;
}
void from_wire(const std::vector<wire::StringEntry>& entries, std::map<std::string, std::string>& out) {
    for (const auto& e : entries) {
        out[owned(*e.key)] = owned(*e.value);
    }
}

// --- Field ------------------------------------------------------------------------------------

wire::Field to_wire(const Field& f) {
    wire::Field w;
    w.unknown = to_unknown(f.unknown);
    w.type = wire::FieldType(f.type);
    w.name = std::string_view(f.name);
    w.id = f.id;
    w.parent_id = f.parent_id;
    w.logical_type = std::string_view(f.logical_type);
    w.nullable = f.nullable;
    w.encoding = wire::FieldEncoding(f.encoding);
    w.metadata = to_wire(f.metadata);
    return w;
}

Field from_wire(const wire::Field& w) {
    Field f;
    // proto3: an absent field is zero, whatever the struct's default (-1 parent, LEAF, PLAIN,
    // nullable). Absent parent_id therefore means "parent is field 0", not "root" -- a pylance
    // dataset whose first column is a struct depends on it.
    f.type = std::int32_t(*w.type);
    f.name = owned(*w.name);
    f.id = *w.id;
    f.parent_id = *w.parent_id;
    f.logical_type = owned(*w.logical_type);
    f.nullable = *w.nullable;
    f.encoding = std::int32_t(*w.encoding);
    from_wire(*w.metadata, f.metadata);
    f.unknown = from_unknown(*w.unknown);
    // nanolance: a field cannot be its own parent. That is field 0 with no parent_id on the wire,
    // i.e. the root of a schema whose writer left the zero out; it is a root.
    if (f.parent_id == f.id) {
        f.parent_id = -1;
    }
    return f;
}

std::vector<wire::Field> to_wire(const std::vector<Field>& fields) {
    std::vector<wire::Field> out;
    out.reserve(fields.size());
    for (const auto& f : fields) {
        out.push_back(to_wire(f));
    }
    return out;
}
std::vector<Field> from_wire(const std::vector<wire::Field>& fields) {
    std::vector<Field> out;
    out.reserve(fields.size());
    for (const auto& f : fields) {
        out.push_back(from_wire(f));
    }
    return out;
}

// --- DataFile / DataFragment ------------------------------------------------------------------

wire::DataFile to_wire(const DataFile& f) {
    wire::DataFile w;
    w.unknown = to_unknown(f.unknown);
    w.path = std::string_view(f.path);
    w.fields = f.fields;
    w.column_indices = f.column_indices;
    w.file_major_version = f.file_major_version;
    w.file_minor_version = f.file_minor_version;
    w.file_size_bytes = f.file_size_bytes;  // `optional` in Lance: always written, 0 included
    return w;
}

DataFile from_wire(wire::DataFile&& w) {
    DataFile f;
    f.path = owned(*w.path);
    f.fields = std::move(w.fields.v);
    f.column_indices = std::move(w.column_indices.v);
    f.file_major_version = *w.file_major_version;
    f.file_minor_version = *w.file_minor_version;
    f.file_size_bytes = w.file_size_bytes->value_or(0);
    f.unknown = from_unknown(*w.unknown);
    return f;
}

wire::DataFragment to_wire(const DataFragment& f) {
    wire::DataFragment w;
    w.unknown = to_unknown(f.unknown);
    w.id = f.id;
    w.files->reserve(f.files.size());
    for (const auto& file : f.files) {
        w.files->push_back(to_wire(file));
    }
    // The deletion file was once not written back, so an append to a dataset with deleted rows
    // brought them back.
    if (f.deletion_file.present) {
        wire::DeletionFile d;
        d.unknown = to_unknown(f.deletion_file.unknown);
        d.file_type = wire::DeletionFileType(f.deletion_file.file_type);
        d.read_version = f.deletion_file.read_version;
        d.id = f.deletion_file.id;
        d.num_deleted_rows = f.deletion_file.num_deleted_rows;
        w.deletion_file = std::move(d);
    }
    w.physical_rows = f.physical_rows;
    return w;
}

DataFragment from_wire(wire::DataFragment&& w) {
    DataFragment f;
    f.id = *w.id;
    f.files.reserve(w.files->size());
    for (auto& file : *w.files) {
        f.files.push_back(from_wire(std::move(file)));
    }
    if (const auto& d = *w.deletion_file) {
        f.deletion_file.present = true;
        f.deletion_file.file_type = std::uint32_t(*d->file_type);
        f.deletion_file.read_version = *d->read_version;
        f.deletion_file.id = *d->id;
        f.deletion_file.num_deleted_rows = *d->num_deleted_rows;
        f.deletion_file.unknown = from_unknown(*d->unknown);
    }
    f.physical_rows = *w.physical_rows;
    f.unknown = from_unknown(*w.unknown);
    return f;
}

// --- ColumnMetadata ---------------------------------------------------------------------------

/// nanolance keeps an Encoding as the raw DirectEncoding bytes (a lance.encodings21 descriptor,
/// parsed by page_layout.hpp); empty means "no encoding" and is not written.
std::optional<wire::Encoding> to_wire_encoding(const std::vector<std::uint8_t>& encoding) {
    if (encoding.empty()) {
        return std::nullopt;
    }
    wire::Encoding e;
    e.direct = wire::DirectEncoding{};
    (*e.direct)->encoding = view(encoding);
    return e;
}
std::vector<std::uint8_t> from_wire_encoding(const std::optional<wire::Encoding>& e) {
    if (!e || !*e->direct) {
        return {};  // absent, or an indirect / none encoding: nothing inline
    }
    return owned(*(*e->direct)->encoding);
}

// --- indices ----------------------------------------------------------------------------------

/// Decode one IndexMetadata message. `index.raw` keeps the message as read: an index nanolance
/// does not change is written back byte for byte.
bool decode_index_metadata(const std::uint8_t* data, std::size_t size, IndexMetadata& index, std::string& error) {
    index = IndexMetadata{};
    index.raw.assign(data, data + size);
    wire::IndexMetadata w;
    if (!decode(data, size, w)) {
        error = "index metadata is malformed";
        return false;
    }
    index.fields = *w.fields;
    index.name = owned(*w.name);
    index.dataset_version = *w.dataset_version;
    if (const auto& uuid = *w.uuid; uuid && uuid->uuid->size() == index.uuid.size()) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(uuid->uuid->data());
        std::copy(p, p + index.uuid.size(), index.uuid.begin());
    }
    if (const auto& details = *w.index_details) {
        index.details_type_url = owned(*details->type_url);
        index.details_value = owned(*details->value);
    }
    index.index_version = static_cast<std::uint32_t>(w.index_version->value_or(0));
    index.created_at = w.created_at->value_or(0);
    for (const auto& f : *w.files) {
        index.files.push_back(IndexMetadata::File{owned(*f.path), *f.size});
    }
    if (const auto& bitmap = *w.fragment_bitmap) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(bitmap->data());
        if (!roaring::decode(p, bitmap->size(), index.fragment_ids, error)) {
            error = "index '" + index.name + "': " + error;
            return false;
        }
        index.has_fragment_bitmap = true;
    }
    return true;
}

/// An index's message: as read, but for its fragment bitmap when the writer changed it.
std::vector<std::uint8_t> encode_index_metadata(const IndexMetadata& index) {
    if (!index.fragment_bitmap_changed) {
        return index.raw;
    }
    wire::IndexMetadata w;
    if (!decode(index.raw, w)) {
        return index.raw;  // decode_index_metadata accepted it, so this does not happen
    }
    const auto bitmap = index.has_fragment_bitmap ? roaring::encode(index.fragment_ids) : std::vector<std::uint8_t>{};
    if (index.has_fragment_bitmap) {
        w.fragment_bitmap = view(bitmap);
    } else {
        w.fragment_bitmap = std::nullopt;
    }
    return encode(w);
}

}  // namespace

// --- public API -------------------------------------------------------------------------------

IndexMetadata make_index_metadata(const std::array<std::uint8_t, 16>& uuid, const std::vector<std::int32_t>& fields,
                                  const std::string& name, std::uint64_t dataset_version,
                                  const std::vector<std::uint32_t>& fragment_ids, const std::string& details_type_url,
                                  std::uint32_t index_version, std::uint64_t created_at,
                                  const std::vector<IndexMetadata::File>& files,
                                  const std::vector<std::uint8_t>& details_value) {
    const auto bitmap = roaring::encode(fragment_ids);
    wire::IndexMetadata w;
    w.uuid = wire::Uuid{};
    (*w.uuid)->uuid = nm::bytes(reinterpret_cast<const std::byte*>(uuid.data()), uuid.size());
    w.fields = fields;
    w.name = std::string_view(name);
    w.dataset_version = dataset_version;
    w.fragment_bitmap = view(bitmap);
    w.index_details = wire::Any{};
    (*w.index_details)->type_url = std::string_view(details_type_url);
    if (!details_value.empty()) {
        (*w.index_details)->value = view(details_value);
    }
    w.index_version = static_cast<std::int32_t>(index_version);
    w.created_at = created_at;
    for (const auto& file : files) {
        wire::IndexFile f;
        f.path = std::string_view(file.path);
        f.size = file.size;
        w.files->push_back(f);
    }
    const auto raw = encode(w);
    IndexMetadata index;
    std::string error;
    decode_index_metadata(raw.data(), raw.size(), index, error);  // what was just written decodes
    return index;
}

std::string uuid_string(const std::array<std::uint8_t, 16>& uuid) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < uuid.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            s.push_back('-');
        }
        s.push_back(hex[uuid[i] >> 4U]);
        s.push_back(hex[uuid[i] & 0x0FU]);
    }
    return s;
}

bool decode_index_message(const std::uint8_t* data, std::size_t size, IndexMetadata& index, std::string& error) {
    return decode_index_metadata(data, size, index, error);
}

std::vector<std::uint8_t> encode_index_message(const IndexMetadata& index) {
    return encode_index_metadata(index);
}

bool decode_index_section(const std::vector<std::uint8_t>& bytes, std::vector<IndexMetadata>& out,
                          std::string& error) {
    out.clear();
    wire::IndexSection w;
    if (!decode(bytes, w)) {
        error = "index section is truncated or malformed";
        return false;
    }
    out.reserve(w.indices->size());
    for (const auto& raw : *w.indices) {
        IndexMetadata index;
        if (!decode_index_metadata(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size(), index, error)) {
            return false;
        }
        out.push_back(std::move(index));
    }
    return true;
}

std::vector<std::uint8_t> encode_index_section(const std::vector<IndexMetadata>& indices) {
    std::vector<std::vector<std::uint8_t>> messages;  // owns what the section's views point at
    messages.reserve(indices.size());
    wire::IndexSection w;
    w.indices->reserve(indices.size());
    for (const auto& index : indices) {
        messages.push_back(encode_index_metadata(index));
        w.indices->push_back(view(messages.back()));
    }
    return encode(w);
}

std::vector<std::uint8_t> encode_file_descriptor(const FileDescriptor& descriptor) {
    wire::FileDescriptor w;
    w.schema = wire::Schema{};
    (*w.schema)->fields = to_wire(descriptor.fields);
    (*w.schema)->metadata = to_wire(descriptor.schema_metadata);
    w.length = descriptor.length;
    return encode(w);
}

bool decode_file_descriptor(const std::vector<std::uint8_t>& bytes, FileDescriptor& descriptor) {
    wire::FileDescriptor w;
    if (!decode(bytes, w)) {
        return false;
    }
    descriptor = FileDescriptor{};
    if (*w.schema) {
        descriptor.fields = from_wire(*(*w.schema)->fields);
        from_wire(*(*w.schema)->metadata, descriptor.schema_metadata);
    }
    descriptor.length = *w.length;
    return true;
}

std::vector<std::uint8_t> encode_manifest(const Manifest& manifest) {
    wire::Manifest w;
    w.unknown = to_unknown(manifest.unknown);
    w.fields = to_wire(manifest.fields);
    w.fragments->reserve(manifest.fragments.size());
    for (const auto& fragment : manifest.fragments) {
        w.fragments->push_back(to_wire(fragment));
    }
    w.version = manifest.version;
    w.schema_metadata = to_wire(manifest.schema_metadata);
    if (manifest.has_index_section) {
        w.index_section = manifest.index_section;
    }
    if (manifest.has_timestamp) {
        wire::Timestamp ts;
        ts.seconds = manifest.timestamp_seconds;
        ts.nanos = manifest.timestamp_nanos;
        w.timestamp = ts;
    }
    w.tag = std::string_view(manifest.tag);
    w.reader_feature_flags = manifest.reader_feature_flags;
    w.writer_feature_flags = manifest.writer_feature_flags;
    if (manifest.has_max_fragment_id) {
        w.max_fragment_id = manifest.max_fragment_id;
    }
    w.transaction_file = std::string_view(manifest.transaction_file);
    if (!manifest.writer_library.empty() || !manifest.writer_version.empty()) {
        wire::WriterVersion wv;
        wv.library = std::string_view(manifest.writer_library);
        wv.version = std::string_view(manifest.writer_version);
        w.writer_version = wv;
    }
    w.next_row_id = manifest.next_row_id;
    w.data_format = wire::DataStorageFormat{};
    (*w.data_format)->file_format = std::string_view(manifest.data_format.file_format);
    (*w.data_format)->version = std::string_view(manifest.data_format.version);
    w.config = to_wire(manifest.config);
    w.table_metadata = to_wire(manifest.table_metadata);
    // version_aux_data (4) and transaction_section (21) point into the manifest file they were read
    // from; they are never carried into another one.
    return encode(w);
}

bool decode_manifest(const std::vector<std::uint8_t>& bytes, Manifest& manifest) {
    wire::Manifest w;
    if (!decode(bytes, w)) {
        return false;
    }
    manifest = Manifest{};
    manifest.fields = from_wire(*w.fields);
    manifest.fragments.reserve(w.fragments->size());
    for (auto& fragment : *w.fragments) {
        manifest.fragments.push_back(from_wire(std::move(fragment)));
    }
    manifest.version = *w.version;
    from_wire(*w.schema_metadata, manifest.schema_metadata);
    if (*w.index_section) {
        manifest.has_index_section = true;
        manifest.index_section = **w.index_section;
    }
    if (const auto& ts = *w.timestamp) {
        manifest.has_timestamp = true;
        manifest.timestamp_seconds = *ts->seconds;
        manifest.timestamp_nanos = *ts->nanos;
    }
    manifest.tag = owned(*w.tag);
    manifest.reader_feature_flags = *w.reader_feature_flags;
    manifest.writer_feature_flags = *w.writer_feature_flags;
    if (*w.max_fragment_id) {
        manifest.has_max_fragment_id = true;
        manifest.max_fragment_id = **w.max_fragment_id;
    }
    manifest.transaction_file = owned(*w.transaction_file);
    if (const auto& wv = *w.writer_version) {
        manifest.writer_library = owned(*wv->library);
        manifest.writer_version = owned(*wv->version);
    }
    manifest.next_row_id = *w.next_row_id;
    if (*w.data_format) {
        manifest.data_format.file_format = owned(*(*w.data_format)->file_format);
        manifest.data_format.version = owned(*(*w.data_format)->version);
    }
    from_wire(*w.config, manifest.config);
    from_wire(*w.table_metadata, manifest.table_metadata);
    manifest.unknown = from_unknown(*w.unknown);
    return true;
}

std::vector<std::uint8_t> encode_data_fragment(const DataFragment& fragment) { return encode(to_wire(fragment)); }

bool decode_data_fragment(const std::vector<std::uint8_t>& bytes, DataFragment& fragment) {
    wire::DataFragment w;
    if (!decode(bytes, w)) {
        return false;
    }
    fragment = from_wire(std::move(w));
    return true;
}

std::vector<std::uint8_t> encode_data_file(const DataFile& file) { return encode(to_wire(file)); }

bool decode_data_file(const std::vector<std::uint8_t>& bytes, DataFile& file) {
    wire::DataFile w;
    if (!decode(bytes, w)) {
        return false;
    }
    file = from_wire(std::move(w));
    return true;
}

bool decode_field(const std::vector<std::uint8_t>& bytes, Field& field) {
    wire::Field w;
    if (!decode(bytes, w)) {
        return false;
    }
    field = from_wire(w);
    return true;
}

std::vector<std::uint8_t> encode_metadata(const Metadata& metadata) {
    wire::Metadata w;
    w.batch_offsets = metadata.batch_offsets;
    w.page_table_position = metadata.page_table_position;
    return encode(w);
}

bool decode_metadata(const std::vector<std::uint8_t>& bytes, Metadata& metadata) {
    wire::Metadata w;
    if (!decode(bytes, w)) {
        return false;
    }
    metadata.batch_offsets.insert(metadata.batch_offsets.end(), w.batch_offsets->begin(), w.batch_offsets->end());
    metadata.page_table_position = *w.page_table_position;
    return true;
}

std::vector<std::uint8_t> encode_column_metadata(const ColumnMetadata& metadata) {
    wire::ColumnMetadata w;
    w.encoding = to_wire_encoding(metadata.encoding);
    w.pages->reserve(metadata.pages.size());
    for (const auto& page : metadata.pages) {
        wire::Page p;
        p.buffer_offsets = page.buffer_offsets;
        p.buffer_sizes = page.buffer_sizes;
        p.length = page.length;
        p.encoding = to_wire_encoding(page.encoding);
        p.priority = page.priority;
        w.pages->push_back(std::move(p));
    }
    w.buffer_offsets = metadata.buffer_offsets;
    w.buffer_sizes = metadata.buffer_sizes;
    return encode(w);
}

bool decode_column_metadata(const std::vector<std::uint8_t>& bytes, ColumnMetadata& metadata) {
    wire::ColumnMetadata w;
    if (!decode(bytes, w)) {
        return false;
    }
    metadata.encoding = from_wire_encoding(*w.encoding);
    metadata.pages.reserve(metadata.pages.size() + w.pages->size());
    for (auto& p : *w.pages) {
        ColumnPage page;
        page.buffer_offsets = std::move(p.buffer_offsets.v);
        page.buffer_sizes = std::move(p.buffer_sizes.v);
        page.length = *p.length;
        page.priority = *p.priority;
        page.encoding = from_wire_encoding(*p.encoding);
        metadata.pages.push_back(std::move(page));
    }
    metadata.buffer_offsets.insert(metadata.buffer_offsets.end(), w.buffer_offsets->begin(), w.buffer_offsets->end());
    metadata.buffer_sizes.insert(metadata.buffer_sizes.end(), w.buffer_sizes->begin(), w.buffer_sizes->end());
    return true;
}

}  // namespace nano_lance::pb
