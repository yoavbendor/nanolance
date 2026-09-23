// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "lance_minimal.pb.hpp"

#include <algorithm>
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
        df.fields = {2};
        df.column_indices = {0};
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
        require(decoded.fragments[0].files[0].fields.size() == 1 && decoded.fragments[0].files[0].fields[0] == 2,
                "data file fields");
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
    return 0;
}
