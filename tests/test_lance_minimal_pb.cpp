// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

/// Append a protobuf varint key/value pair by hand, so a test can write wire bytes our own encoder
/// would never produce -- specifically, one that LEAVES OUT a field.
void put_varint(std::vector<std::uint8_t>& out, std::uint32_t field_number, std::int64_t value) {
    auto put = [&out](std::uint64_t v) {
        while (v >= 0x80U) {
            out.push_back(static_cast<std::uint8_t>((v & 0x7FU) | 0x80U));
            v >>= 7U;
        }
        out.push_back(static_cast<std::uint8_t>(v));
    };
    put((static_cast<std::uint64_t>(field_number) << 3U) | 0U);
    put(static_cast<std::uint64_t>(value));
}

bool contains_bytes(const std::vector<std::uint8_t>& haystack, const std::string& needle) {
    const auto needle_begin = reinterpret_cast<const std::uint8_t*>(needle.data());
    const std::vector<std::uint8_t> bytes(needle_begin, needle_begin + needle.size());
    return std::search(haystack.begin(), haystack.end(), bytes.begin(), bytes.end()) != haystack.end();
}

}  // namespace

int main() {
    {
        nano_lance::pb::Manifest manifest;
        manifest.version = 7;
        manifest.data_format.file_format = "lance";
        manifest.data_format.version = "2.2";
        nano_lance::pb::Manifest decoded;
        require(nano_lance::pb::decode_manifest(nano_lance::pb::encode_manifest(manifest), decoded), "manifest decode failed");
        require(decoded.version == manifest.version, "manifest version mismatch");
        require(decoded.data_format.file_format == "lance", "manifest file format mismatch");
        require(decoded.data_format.version == "2.2", "manifest data version mismatch");
    }
    {
        nano_lance::pb::Field field;
        field.name = "payload_ref";
        field.logical_type = "struct";
        field.id = 7;
        field.parent_id = -1;
        const std::string metadata_key = "ARROW:extension:name";
        const std::string metadata_value = "lance.blob.v2";
        field.metadata[metadata_key] =
            std::vector<std::uint8_t>(metadata_value.begin(), metadata_value.end());

        nano_lance::pb::Manifest manifest;
        manifest.fields.push_back(field);
        const auto manifest_bytes = nano_lance::pb::encode_manifest(manifest);
        require(contains_bytes(manifest_bytes, metadata_key), "manifest field metadata key missing");
        require(contains_bytes(manifest_bytes, metadata_value), "manifest field metadata value missing");

        nano_lance::pb::FileDescriptor descriptor;
        descriptor.fields.push_back(field);
        const auto descriptor_bytes = nano_lance::pb::encode_file_descriptor(descriptor);
        require(contains_bytes(descriptor_bytes, metadata_key), "file descriptor metadata key missing");
        require(contains_bytes(descriptor_bytes, metadata_value), "file descriptor metadata value missing");
    }
    {
        nano_lance::pb::DataFragment fragment;
        fragment.id = 11;
        fragment.physical_rows = 42;
        nano_lance::pb::DataFragment decoded;
        require(nano_lance::pb::decode_data_fragment(nano_lance::pb::encode_data_fragment(fragment), decoded),
                "fragment decode failed");
        require(decoded.id == 11, "fragment id mismatch");
        require(decoded.physical_rows == 42, "fragment physical rows mismatch");
    }
    {
        nano_lance::pb::DataFile file;
        file.path = "data/fragment-0.lance";
        file.fields = {1, 2, 3};
        file.column_indices = {0, 1, 2};
        file.file_major_version = 2;
        file.file_minor_version = 2;
        file.file_size_bytes = 1024;
        nano_lance::pb::DataFile decoded;
        require(nano_lance::pb::decode_data_file(nano_lance::pb::encode_data_file(file), decoded), "file decode failed");
        require(decoded.path == file.path, "file path mismatch");
        require(decoded.fields == file.fields, "file fields mismatch");
        require(decoded.column_indices == file.column_indices, "file column indices mismatch");
        require(decoded.file_major_version == 2, "file major mismatch");
        require(decoded.file_minor_version == 2, "file minor mismatch");
        require(decoded.file_size_bytes == 1024, "file size mismatch");
    }
    {
        nano_lance::pb::Field f1;
        f1.name = "id";
        f1.logical_type = "int64";
        f1.id = 1;
        f1.parent_id = -1;
        f1.type = 2;
        f1.nullable = false;
        f1.encoding = 1;
        nano_lance::pb::Manifest mwrap;
        mwrap.version = 1;
        mwrap.data_format.file_format = "lance";
        mwrap.data_format.version = "2.2";
        mwrap.fields.push_back(f1);
        nano_lance::pb::Manifest mdecoded;
        require(nano_lance::pb::decode_manifest(nano_lance::pb::encode_manifest(mwrap), mdecoded), "field via manifest round trip failed");
        require(mdecoded.fields.size() == 1, "field count");
        const auto& f1d = mdecoded.fields[0];
        require(f1d.name == f1.name && f1d.logical_type == f1.logical_type && f1d.id == f1.id && f1d.parent_id == f1.parent_id,
                "field mismatch");
    }
    {
        nano_lance::pb::Manifest manifest;
        manifest.version = 9;
        manifest.has_max_fragment_id = true;
        manifest.max_fragment_id = 3;
        manifest.data_format.file_format = "lance";
        manifest.data_format.version = "2.2";
        nano_lance::pb::Field f;
        f.name = "x";
        f.logical_type = "int32";
        f.id = 2;
        f.parent_id = -1;
        manifest.fields.push_back(f);
        nano_lance::pb::DataFile df;
        df.path = "fragment-1.lance";
        df.fields = {2, -2};  // -2: a column the schema dropped, still in the file (real LanceDB tables)
        df.column_indices = {0, 1};
        df.file_major_version = 2;
        df.file_minor_version = 2;
        df.file_size_bytes = 2048;
        nano_lance::pb::DataFragment frag;
        frag.id = 0;
        frag.physical_rows = 100;
        frag.files.push_back(df);
        manifest.fragments.push_back(frag);
        nano_lance::pb::Manifest decoded;
        require(nano_lance::pb::decode_manifest(nano_lance::pb::encode_manifest(manifest), decoded), "full manifest decode failed");
        require(decoded.version == manifest.version, "manifest version");
        require(decoded.has_max_fragment_id && decoded.max_fragment_id == 3, "max_fragment_id");
        require(decoded.fields.size() == 1 && decoded.fields[0].name == "x", "manifest field");
        require(decoded.fragments.size() == 1 && decoded.fragments[0].physical_rows == 100, "fragment rows");
        require(decoded.fragments[0].files.size() == 1 && decoded.fragments[0].files[0].path == "fragment-1.lance",
                "fragment file");
        require(decoded.fragments[0].files[0].fields == std::vector<std::int32_t>{2, -2}, "data file fields");
        require(decoded.fragments[0].files[0].column_indices == std::vector<std::int32_t>{0, 1}, "column indices");
    }
    {
        nano_lance::pb::FileDescriptor d;
        d.length = 123;
        nano_lance::pb::Field f;
        f.name = "id";
        f.logical_type = "int64";
        f.id = 1;
        f.parent_id = -1;
        d.fields.push_back(f);
        const auto bytes = nano_lance::pb::encode_file_descriptor(d);
        nano_lance::pb::FileDescriptor decoded_fd;
        require(nano_lance::pb::decode_file_descriptor(bytes, decoded_fd), "file descriptor decode failed");
        require(decoded_fd.length == 123ULL, "file descriptor length mismatch");
        require(decoded_fd.fields.size() == 1U && decoded_fd.fields[0].name == "id", "file descriptor field");
    }
    {
        nano_lance::pb::Metadata metadata;
        metadata.batch_offsets = {0, 128};
        metadata.page_table_position = 4096;
        nano_lance::pb::Metadata decoded;
        require(nano_lance::pb::decode_metadata(nano_lance::pb::encode_metadata(metadata), decoded),
                "metadata decode failed");
        require(decoded.batch_offsets.size() == 2, "metadata batch offsets mismatch");
        require(decoded.page_table_position == 4096, "metadata page table mismatch");
    }
    {
        nano_lance::pb::ColumnMetadata metadata;
        nano_lance::pb::ColumnPage page;
        page.buffer_offsets = {64};
        page.buffer_sizes = {16};
        page.length = 4;
        page.priority = 0;
        metadata.pages.push_back(page);
        nano_lance::pb::ColumnMetadata decoded;
        require(nano_lance::pb::decode_column_metadata(nano_lance::pb::encode_column_metadata(metadata), decoded),
                "column metadata decode failed");
        require(decoded.pages.size() == 1, "column page count mismatch");
        require(decoded.pages[0].buffer_offsets[0] == 64, "column page offset mismatch");
        require(decoded.pages[0].buffer_sizes[0] == 16, "column page size mismatch");
        require(decoded.pages[0].length == 4, "column page length mismatch");
    }
    {
        // Lance's `Field.parent_id` is a proto3 int32, and proto3 puts no zero on the wire. So an
        // ABSENT field 4 means parent_id 0 -- "my parent is field id 0" -- and NOT "I am a root".
        // Roots carry an explicit -1, which is non-zero and therefore always serialized.
        //
        // Reading the absence as -1 detached every child of field id 0 from its parent. The visible
        // damage was a pylance dataset whose first column was a struct: its children became roots,
        // the struct was left childless, and the whole dataset failed to open. The same struct in
        // second position was fine, because then the ids shift and the parent link is non-zero.
        //
        // Our own encoder always writes field 4, so a round-trip test cannot reach this. The bytes
        // have to be built by hand.
        std::vector<std::uint8_t> child;
        put_varint(child, 3, 1);  // id = 1; no field 4
        nano_lance::pb::Field decoded;
        require(nano_lance::pb::decode_field(child, decoded), "field decode failed");
        require(decoded.id == 1, "field id mismatch");
        require(decoded.parent_id == 0, "an absent parent_id must mean field 0, not root");

        // ...except for field 0 itself, which cannot be its own parent. That is a root.
        std::vector<std::uint8_t> root_zero;
        put_varint(root_zero, 3, 0);  // id = 0; no field 4
        require(nano_lance::pb::decode_field(root_zero, decoded), "field decode failed");
        require(decoded.parent_id == -1, "field 0 with no parent_id on the wire is a root");

        // An explicit -1 is a root at any id, and an explicit parent is honoured as written.
        std::vector<std::uint8_t> root;
        put_varint(root, 3, 0);
        put_varint(root, 4, -1);
        require(nano_lance::pb::decode_field(root, decoded), "field decode failed");
        require(decoded.parent_id == -1, "an explicit -1 parent_id is a root");

        std::vector<std::uint8_t> nested;
        put_varint(nested, 3, 2);
        put_varint(nested, 4, 1);
        require(nano_lance::pb::decode_field(nested, decoded), "field decode failed");
        require(decoded.parent_id == 1, "an explicit parent_id must be kept");
    }
    {
        // `type` and `encoding` follow the same proto3 rule: absent means 0, not the struct default.
        // A Lance struct parent carries neither (PARENT = 0, encoding NONE = 0).
        std::vector<std::uint8_t> parent;
        put_varint(parent, 3, 5);
        nano_lance::pb::Field decoded;
        require(nano_lance::pb::decode_field(parent, decoded), "field decode failed");
        require(decoded.type == 0, "an absent type must decode as 0 (PARENT)");
        require(decoded.encoding == 0, "an absent encoding must decode as 0 (NONE)");

        // And our own LEAF (2) must survive a round trip, which needs the encoder to write it.
        nano_lance::pb::Field leaf;
        leaf.name = "x";
        leaf.type = 2;
        leaf.encoding = 1;
        nano_lance::pb::FileDescriptor descriptor;
        descriptor.fields.push_back(leaf);
        nano_lance::pb::FileDescriptor back;
        require(nano_lance::pb::decode_file_descriptor(nano_lance::pb::encode_file_descriptor(descriptor), back) &&
                    back.fields.size() == 1U,
                "file descriptor round trip failed");
        require(back.fields[0].type == 2, "a LEAF type must round-trip");
        require(back.fields[0].encoding == 1, "a PLAIN encoding must round-trip");
    }
    {
        // Manifests nanolance wrote before its protobuf codec moved to nanom (unpacked repeated
        // int32s, zeros written out) must keep reading the same. These bytes are that old
        // encoder's output, kept verbatim.
        const std::vector<std::uint8_t> old_bytes = {
            0x0a, 0x24, 0x08, 0x02, 0x12, 0x02, 0x69, 0x64, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0x01, 0x2a, 0x05, 0x69, 0x6e, 0x74, 0x36, 0x34, 0x30, 0x01, 0x38, 0x01, 0x52, 0x06,
            0x0a, 0x01, 0x6b, 0x12, 0x01, 0x76, 0x12, 0x1d, 0x08, 0x00, 0x12, 0x17, 0x0a, 0x07, 0x61, 0x2e,
            0x6c, 0x61, 0x6e, 0x63, 0x65, 0x10, 0x00, 0x10, 0x01, 0x18, 0x00, 0x18, 0x01, 0x20, 0x02, 0x28,
            0x01, 0x30, 0x64, 0x20, 0x03, 0x18, 0x01, 0x58, 0x00, 0x7a, 0x0c, 0x0a, 0x05, 0x6c, 0x61, 0x6e,
            0x63, 0x65, 0x12, 0x03, 0x32, 0x2e, 0x31};
        nano_lance::pb::Manifest m;
        require(nano_lance::pb::decode_manifest(old_bytes, m), "an old-encoder manifest must decode");
        require(m.version == 1 && m.has_max_fragment_id && m.max_fragment_id == 0, "old manifest header");
        require(m.data_format.file_format == "lance" && m.data_format.version == "2.1", "old manifest format");
        require(m.fields.size() == 1 && m.fields[0].name == "id" && m.fields[0].parent_id == -1 &&
                    m.fields[0].type == 2 && m.fields[0].nullable && m.fields[0].encoding == 1 &&
                    m.fields[0].metadata.at("k") == std::vector<std::uint8_t>{'v'},
                "old manifest field");
        require(m.fragments.size() == 1 && m.fragments[0].physical_rows == 3 && m.fragments[0].files.size() == 1,
                "old manifest fragment");
        const auto& f = m.fragments[0].files[0];
        require(f.path == "a.lance" && f.fields == std::vector<std::int32_t>{0, 1} &&
                    f.column_indices == std::vector<std::int32_t>{0, 1} && f.file_major_version == 2 &&
                    f.file_minor_version == 1 && f.file_size_bytes == 100,
                "old manifest data file (unpacked repeated int32)");

        // Re-encoding is canonical proto3 (repeated int32s packed, zeros left out) and reads back
        // to the same manifest.
        const auto again = nano_lance::pb::encode_manifest(m);
        require(again.size() < old_bytes.size(), "the canonical encoding is the compact one");
        nano_lance::pb::Manifest back;
        require(nano_lance::pb::decode_manifest(again, back) && back.fragments.size() == 1 &&
                    back.fragments[0].files[0].fields == f.fields && back.fields[0].metadata == m.fields[0].metadata,
                "canonical re-encoding round trip");
    }
    {
        // A fragment's deletion file is written back. The hand-written encoder dropped it, so a
        // nanolance append to a dataset with deletions resurrected the deleted rows.
        nano_lance::pb::DataFragment fragment;
        fragment.id = 4;
        fragment.physical_rows = 100;
        fragment.deletion_file.present = true;
        fragment.deletion_file.file_type = 1;  // BITMAP
        fragment.deletion_file.read_version = 7;
        fragment.deletion_file.id = 99;
        fragment.deletion_file.num_deleted_rows = 12;
        nano_lance::pb::Manifest manifest;
        manifest.fragments.push_back(fragment);
        nano_lance::pb::Manifest back;
        require(nano_lance::pb::decode_manifest(nano_lance::pb::encode_manifest(manifest), back) &&
                    back.fragments.size() == 1,
                "manifest with a deletion file round trip");
        const auto& d = back.fragments[0].deletion_file;
        require(d.present && d.file_type == 1 && d.read_version == 7 && d.id == 99 && d.num_deleted_rows == 12,
                "deletion file must survive a manifest rewrite");

        // ...and a fragment without one stays without one, even when every value is zero.
        nano_lance::pb::DataFragment plain;
        nano_lance::pb::DataFragment plain_back;
        require(nano_lance::pb::decode_data_fragment(nano_lance::pb::encode_data_fragment(plain), plain_back) &&
                    !plain_back.deletion_file.present,
                "no deletion file stays absent");
    }
    {
        // Lance marks dropped columns in a data file with negative field ids (-2); they are valid
        // int32s (10-byte varints on the wire) and must round-trip, not fail the decode.
        nano_lance::pb::DataFile file;
        file.path = "x.lance";
        file.fields = {0, -2, 3};
        file.column_indices = {0, -1, 1};
        nano_lance::pb::DataFile back;
        require(nano_lance::pb::decode_data_file(nano_lance::pb::encode_data_file(file), back) &&
                    back.fields == file.fields && back.column_indices == file.column_indices,
                "negative data-file field ids round trip");
    }
    {
        // The column-level encoding round-trips like the page-level one.
        nano_lance::pb::ColumnMetadata column;
        column.encoding = {0x0a, 0x00};
        nano_lance::pb::ColumnPage page;
        page.length = 3;
        page.encoding = {0x12, 0x01, 0x05};
        column.pages.push_back(page);
        nano_lance::pb::ColumnMetadata back;
        require(nano_lance::pb::decode_column_metadata(nano_lance::pb::encode_column_metadata(column), back) &&
                    back.encoding == column.encoding && back.pages.size() == 1 &&
                    back.pages[0].encoding == page.encoding && back.pages[0].length == 3,
                "column metadata encodings round trip");
    }
    {
        // Packed batch offsets (what protobuf writers emit for a proto3 repeated int32) are read.
        // The hand-written decoder skipped them.
        nano_lance::pb::Metadata metadata;
        metadata.batch_offsets = {0, 128, 4096};
        const auto bytes = nano_lance::pb::encode_metadata(metadata);
        require(bytes.size() > 2 && bytes[0] == 0x12, "batch_offsets are written packed (field 2, wire type 2)");
        nano_lance::pb::Metadata back;
        require(nano_lance::pb::decode_metadata(bytes, back) && back.batch_offsets == metadata.batch_offsets,
                "packed batch offsets round trip");
    }
    {
        // Malformed or truncated metadata is rejected, never half-read.
        nano_lance::pb::Manifest manifest;
        manifest.version = 3;
        manifest.data_format.file_format = "lance";
        nano_lance::pb::Field f;
        f.name = "a_name";
        manifest.fields.push_back(f);
        const auto bytes = nano_lance::pb::encode_manifest(manifest);
        for (std::size_t cut = 1; cut < bytes.size(); ++cut) {
            const std::vector<std::uint8_t> prefix(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
            nano_lance::pb::Manifest ignored;
            (void)nano_lance::pb::decode_manifest(prefix, ignored);  // must not crash (ASan in CI)
        }
        const std::vector<std::uint8_t> group = {0x0b};  // wire type 3 (a group): not protobuf 3
        nano_lance::pb::Manifest ignored;
        require(!nano_lance::pb::decode_manifest(group, ignored), "a group wire type is rejected");
        const std::vector<std::uint8_t> long_len = {0x0a, 0x7f, 0x00};  // length past the end
        require(!nano_lance::pb::decode_manifest(long_len, ignored), "a length past the end is rejected");
    }
    {
        // Fields another Lance writer put in a manifest that nanolance does not model (base paths,
        // a branch, a fragment's row id sequence, a field's dictionary, ...) survive a rewrite:
        // nanolance commits rewrite the manifest of every version they append to.
        std::vector<std::uint8_t> field_bytes;
        put_varint(field_bytes, 3, 7);       // id = 7
        put_varint(field_bytes, 12, 1);      // unenforced_primary_key = true (not modelled)
        std::vector<std::uint8_t> fragment_bytes;
        put_varint(fragment_bytes, 1, 4);    // id = 4
        put_varint(fragment_bytes, 4, 10);   // physical_rows = 10
        fragment_bytes.insert(fragment_bytes.end(), {0x2a, 0x02, 0x08, 0x01});  // 5: row_id_sequence
        std::vector<std::uint8_t> manifest_bytes = {0x0a, static_cast<std::uint8_t>(field_bytes.size())};
        manifest_bytes.insert(manifest_bytes.end(), field_bytes.begin(), field_bytes.end());
        manifest_bytes.push_back(0x12);
        manifest_bytes.push_back(static_cast<std::uint8_t>(fragment_bytes.size()));
        manifest_bytes.insert(manifest_bytes.end(), fragment_bytes.begin(), fragment_bytes.end());
        put_varint(manifest_bytes, 3, 5);    // version = 5
        put_varint(manifest_bytes, 4, 99);   // version_aux_data: a position in THIS file, dropped
        manifest_bytes.insert(manifest_bytes.end(), {0x8a, 0x01, 0x03, 'b', 'r', 'x'});  // 17: branch
        put_varint(manifest_bytes, 21, 77);  // transaction_section: a position in THIS file, dropped

        nano_lance::pb::Manifest m;
        require(nano_lance::pb::decode_manifest(manifest_bytes, m), "a manifest with unmodelled fields decodes");
        require(m.version == 5 && m.fields.size() == 1 && m.fields[0].id == 7 && m.fragments.size() == 1 &&
                    m.fragments[0].physical_rows == 10,
                "modelled fields of that manifest");
        require(m.fields[0].unknown == std::vector<std::uint8_t>{0x60, 0x01}, "the field keeps its unmodelled field");
        require(m.fragments[0].unknown == std::vector<std::uint8_t>{0x2a, 0x02, 0x08, 0x01},
                "the fragment keeps its row id sequence");
        require(m.unknown == std::vector<std::uint8_t>{0x8a, 0x01, 0x03, 'b', 'r', 'x'},
                "the manifest keeps its branch, and drops the positions into the old file");

        m.version = 6;  // a commit changes something and writes it back
        nano_lance::pb::Manifest again;
        require(nano_lance::pb::decode_manifest(nano_lance::pb::encode_manifest(m), again), "rewritten manifest");
        require(again.version == 6 && again.unknown == m.unknown && again.fields[0].unknown == m.fields[0].unknown &&
                    again.fragments[0].unknown == m.fragments[0].unknown,
                "unmodelled fields survive the rewrite");
    }
    {
        // An index the commit does not touch is copied byte for byte, whatever it holds; one whose
        // fragment coverage changes is rewritten with the new bitmap and keeps the rest.
        std::array<std::uint8_t, 16> uuid{};
        uuid[0] = 0xab;
        auto index = nano_lance::pb::make_index_metadata(uuid, {3}, "id_idx", 2, {0, 1, 2},
                                                         "/lance.table.BTreeIndexDetails", 0, 1234,
                                                         {{"page_data.lance", 10}});
        require(index.has_fragment_bitmap && index.fragment_ids == std::vector<std::uint32_t>({0, 1, 2}) &&
                    index.name == "id_idx" && index.uuid == uuid && index.files.size() == 1,
                "a new index decodes to what was made");
        index.raw.insert(index.raw.end(), {0x98, 0x01, 0x2a});  // 19: a field from a newer Lance
        std::vector<nano_lance::pb::IndexMetadata> section{index};
        std::vector<nano_lance::pb::IndexMetadata> back;
        std::string error;
        require(nano_lance::pb::decode_index_section(nano_lance::pb::encode_index_section(section), back, error) &&
                    back.size() == 1 && back[0].raw == index.raw,
                "an untouched index is copied byte for byte");
        section[0] = back[0];
        section[0].fragment_ids = {0, 2};
        section[0].fragment_bitmap_changed = true;
        require(nano_lance::pb::decode_index_section(nano_lance::pb::encode_index_section(section), back, error) &&
                    back.size() == 1 && back[0].fragment_ids == std::vector<std::uint32_t>({0, 2}) &&
                    back[0].name == "id_idx" && back[0].created_at == 1234,
                "a changed index has its new coverage");
        const std::vector<std::uint8_t> newer_field = {0x98, 0x01, 0x2a};
        require(std::search(back[0].raw.begin(), back[0].raw.end(), newer_field.begin(), newer_field.end()) !=
                    back[0].raw.end(),
                "...and keeps the fields it does not model");
    }
    return 0;
}
