// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/schema_mapper.hpp"

#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

// `require(f(error), error)` is a trap: the two arguments are evaluated in unspecified
// order, so c_str() can capture a pointer into the EMPTY string before f() runs. When f() then fails
// and assigns a long message, the string reallocates and the captured pointer dangles -- a real
// failure printed a stray "N" instead of its message. Taking the string by reference and calling
// c_str() only after the condition is known fixes it.
void require(bool condition, const std::string& message) {
    require(condition, message.c_str());
}

void release_schema(ArrowSchema& schema) {
    if (schema.release != nullptr) {
        schema.release(&schema);
    }
}

bool set_metadata(ArrowSchema& schema, const char* key, const char* value) {
    ArrowBuffer buffer;
    if (ArrowMetadataBuilderInit(&buffer, nullptr) != NANOARROW_OK) {
        return false;
    }
    ArrowStringView key_view{key, static_cast<int64_t>(std::strlen(key))};
    ArrowStringView value_view{value, static_cast<int64_t>(std::strlen(value))};
    if (ArrowMetadataBuilderAppend(&buffer, key_view, value_view) != NANOARROW_OK) {
        ArrowBufferReset(&buffer);
        return false;
    }
    const auto status = ArrowSchemaSetMetadata(&schema, reinterpret_cast<const char*>(buffer.data));
    ArrowBufferReset(&buffer);
    return status == NANOARROW_OK;
}

const nano_lance::LanceField* find_field(const nano_lance::LanceSchemaMapping& mapping, const char* name) {
    for (const auto& field : mapping.fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

void test_flat_schema() {
    ArrowSchema id{};
    id.format = "l";
    id.name = "id";
    id.flags = 0;

    ArrowSchema score{};
    score.format = "g";
    score.name = "score";
    score.flags = 0;

    ArrowSchema* children[] = {&id, &score};
    ArrowSchema root{};
    root.format = "+s";
    root.name = "";
    root.flags = 0;
    root.n_children = 2;
    root.children = children;

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(root, mapping, error), error);
    require(mapping.fields.size() == 2, "expected two fields");
    require(mapping.fields[0].name == "id", "id field name mismatch");
    require(mapping.fields[0].logical_type == "int64", "id logical type mismatch");
    require(mapping.fields[0].column_index == 0, "id column index mismatch");
    require(mapping.fields[0].parent_id == -1, "id parent mismatch");
    require(!mapping.fields[0].nullable, "id nullability mismatch");
    require(mapping.fields[1].name == "score", "score field name mismatch");
    require(mapping.fields[1].logical_type == "double", "score logical type mismatch");
    require(mapping.fields[1].column_index == 1, "score column index mismatch");
    require(nano_lance::lance_physical_fields(mapping).size() == 2, "flat physical field count mismatch");
}

// Mapping a nullable-FLAGGED field always succeeds, with or without the legacy ignore_nullability
// argument. It used to be rejected unless the flag was passed, which rejected essentially every
// pyarrow table (pyarrow marks all fields nullable) while protecting nothing -- what needs guarding
// is a null VALUE, and ingest refuses those unconditionally (tests/test_null_rejection.cpp).
void test_nullable_flag_is_accepted() {
    ArrowSchema nullable{};
    nullable.format = "l";
    nullable.name = "nullable";
    nullable.flags = ARROW_FLAG_NULLABLE;

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(nullable, mapping, error), "nullable flag should map by default");
    require(mapping.fields.size() == 1, "nullable field count mismatch");
    // The Lance field mirrors the Arrow flag, as pylance's does. It was hardcoded false while nulls
    // were refused outright; now that they are stored, a column with nulls under a field declared
    // non-nullable makes stock Lance reject the file.
    require(mapping.fields[0].nullable, "nullable Arrow flag should produce a nullable Lance field");

    // The legacy argument is accepted and ignored, so old call sites keep compiling and behave the same.
    nano_lance::LanceSchemaMapping legacy;
    require(nano_lance::map_arrow_schema(nullable, legacy, error, true),
            "nullable flag should still map with the legacy ignore argument");
    require(legacy.fields.size() == mapping.fields.size(), "legacy argument changed the mapping");
    require(legacy.fields[0].nullable == mapping.fields[0].nullable, "legacy argument changed nullability");
}

void test_fixed_size_binary() {
    ArrowSchema mac{};
    mac.format = "w:6";
    mac.name = "src_mac";
    mac.flags = 0;

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(mac, mapping, error), error);
    // The width is carried in the logical type (Lance's own form is "fixed_size_binary:<N>") so it can
    // be recovered from the manifest on read.
    require(mapping.fields[0].logical_type == "fixed_size_binary:6", "fixed size binary logical type mismatch");
    require(mapping.fields[0].arrow_format == "w:6", "fixed size binary format mismatch");
}

void test_dictionary_schema() {
    ArrowSchema uri_index{};
    uri_index.format = "i";
    uri_index.name = "blob_uri";
    uri_index.flags = 0;

    ArrowSchema dictionary_values{};
    dictionary_values.format = "u";
    dictionary_values.name = "lance-dictionary-utf8";
    dictionary_values.flags = 0;
    uri_index.dictionary = &dictionary_values;

    // An Arrow dictionary column is stored as Lance stores one: its VALUES' type (so it decodes,
    // filters and encodes as its values) with the index type alongside, and "dict:<value>:<index>:
    // <ordered>" on disk. (It used to map to a bare index column whose dictionary values were stored
    // nowhere; then it was refused.)
    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(uri_index, mapping, error), error);
    require(mapping.fields.size() == 1U && mapping.fields[0].logical_type == "utf8" &&
                mapping.fields[0].arrow_format == "u" && mapping.fields[0].dictionary_index_format == "i" &&
                mapping.fields[0].column_index == 0,
            "dictionary column should map to its values with an int32 index");
    require(nano_lance::lance_field_disk_logical_type(mapping.fields[0]) == "dict:string:int32:false",
            "dictionary column's Lance logical type");

    // A dictionary of dictionaries, or of a nested type, is still refused, by name.
    ArrowSchema inner_values{};
    inner_values.format = "u";
    inner_values.name = "v";
    ArrowSchema inner{};
    inner.format = "i";
    inner.name = "inner";
    inner.dictionary = &inner_values;
    ArrowSchema outer{};
    outer.format = "i";
    outer.name = "outer";
    outer.dictionary = &inner;
    nano_lance::LanceSchemaMapping refused;
    require(!nano_lance::map_arrow_schema(outer, refused, error), "a dictionary of dictionaries should be refused");
    require(error.find("dictionary") != std::string::npos,
            "dictionary rejection should say what was refused, got: " + error);
}

void test_extension_struct() {
    ArrowSchema position{};
    position.format = "L";
    position.name = "position";
    position.flags = 0;

    ArrowSchema size_field{};
    size_field.format = "L";
    size_field.name = "size";
    size_field.flags = 0;

    ArrowSchema data{};
    data.format = "Z";
    data.name = "data";
    data.flags = 0;

    ArrowSchema uri{};
    uri.format = "u";
    uri.name = "uri";
    uri.flags = 0;

    ArrowSchema* payload_children[] = {&data, &uri, &position, &size_field};
    ArrowSchema payload{};
    payload.format = "+s";
    payload.name = "payload_ref";
    payload.flags = 0;
    payload.n_children = 4;
    payload.children = payload_children;
    require(set_metadata(payload, "ARROW:extension:name", "lance.blob.v2"), "failed to set extension metadata");

    ArrowSchema* root_children[] = {&payload};
    ArrowSchema root{};
    root.format = "+s";
    root.name = "";
    root.flags = 0;
    root.n_children = 1;
    root.children = root_children;

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(root, mapping, error), error);
    require(mapping.fields.size() == 5, "extension struct field count mismatch");

    const auto* parent = find_field(mapping, "payload_ref");
    require(parent != nullptr, "missing payload_ref parent");
    require(parent->logical_type == "struct", "payload_ref logical type mismatch");
    require(parent->extension_name == "lance.blob.v2", "payload_ref extension name mismatch");
    require(parent->column_index == -1, "payload_ref must be logical-only");
    require(parent->metadata.at("ARROW:extension:name") == "lance.blob.v2", "payload_ref metadata mismatch");

    const auto* data_field = find_field(mapping, "data");
    require(data_field != nullptr && data_field->parent_id == parent->id, "data parent mismatch");
    require(data_field->logical_type == "large_binary", "data logical type mismatch");
    require(data_field->column_index == 0, "data column index mismatch");

    const auto* uri_field = find_field(mapping, "uri");
    require(uri_field != nullptr && uri_field->logical_type == "utf8", "uri logical type mismatch");

    require(nano_lance::lance_physical_fields(mapping).size() == 4, "extension struct physical field count mismatch");

    // `payload` is a hand-built ArrowSchema (no release callback), so set_metadata()'s
    // ArrowSchemaSetMetadata() allocation is never freed by an owning release() call. Free it directly.
    if (payload.metadata != nullptr) {
        ArrowFree(const_cast<char*>(payload.metadata));
    }
}

void test_top_level_named_struct_maps_as_field() {
    ArrowSchema value{};
    value.format = "L";
    value.name = "position";
    value.flags = 0;

    ArrowSchema* children[] = {&value};
    ArrowSchema payload{};
    payload.format = "+s";
    payload.name = "payload_ref";
    payload.flags = 0;
    payload.n_children = 1;
    payload.children = children;

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(payload, mapping, error), error);
    require(mapping.fields.size() == 2, "named top-level struct should map parent and child");
    require(mapping.fields[0].name == "payload_ref", "named top-level struct parent missing");
    require(mapping.fields[0].column_index == -1, "named top-level struct should be logical-only");
    require(mapping.fields[1].parent_id == mapping.fields[0].id, "named top-level struct child parent mismatch");
}

void test_golden_blob_ipc_schema() {
    const auto ipc_path = std::filesystem::path(NANO_LANCE_BLOB_V2_GOLDEN_DIR) / "input.arrow";
    if (!std::filesystem::exists(ipc_path)) {
        std::cerr << "skip golden blob ipc test: missing " << ipc_path << '\n';
        return;
    }

    FILE* file = std::fopen(ipc_path.string().c_str(), "rb");
    require(file != nullptr, "failed to open golden blob ipc file");

    ArrowIpcInputStream ipc_input{};
    require(ArrowIpcInputStreamInitFile(&ipc_input, file, 0) == NANOARROW_OK, "failed to init golden blob ipc input");

    ArrowArrayStream stream;
    ArrowIpcArrayStreamReaderOptions options{};
    std::memset(&options, 0, sizeof(options));
    options.field_index = -1;
    require(ArrowIpcArrayStreamReaderInit(&stream, &ipc_input, &options) == NANOARROW_OK,
            "failed to init golden blob ipc stream reader");

    ArrowSchema schema;
    require(stream.get_schema(&stream, &schema) == NANOARROW_OK, "failed to read golden blob ipc schema");

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(schema, mapping, error, true), error);

    const auto* packet_id = find_field(mapping, "packet_id");
    const auto* payload_ref = find_field(mapping, "payload_ref");
    require(packet_id != nullptr, "missing packet_id in golden schema");
    require(payload_ref != nullptr, "missing payload_ref in golden schema");
    require(payload_ref->extension_name == "lance.blob.v2", "golden payload_ref extension mismatch");
    require(find_field(mapping, "data") != nullptr, "missing data child in golden schema");
    require(find_field(mapping, "uri") != nullptr, "missing uri child in golden schema");
    require(find_field(mapping, "position") != nullptr, "missing position child in golden schema");
    require(find_field(mapping, "size") != nullptr, "missing size child in golden schema");
    require(nano_lance::lance_physical_fields(mapping).size() == 5, "golden physical field count mismatch");

    if (stream.release != nullptr) {
        stream.release(&stream);
    }
    if (ipc_input.release != nullptr) {
        ipc_input.release(&ipc_input);
    }
    release_schema(schema);
}

// The unenforced primary key of an Arrow field, read from its metadata the way pylance 12 reads it (measured:
// a position that parses wins over the flag; a truthy flag alone is position 0; the clustering position is
// only read when it parses). The write checks use pylance's own messages.
void test_unenforced_primary_key() {
    const char* const kKey = "lance-schema:unenforced-primary-key";
    const char* const kPosition = "lance-schema:unenforced-primary-key:position";
    const char* const kClustering = "lance-schema:unenforced-clustering-key:position";

    // Maps one non-null int64 column `id` carrying the given metadata; returns the mapped field.
    auto map_one = [](const char* key, const char* value, nano_lance::LanceField& out) {
        ArrowSchema id{};
        id.format = "l";
        id.name = "id";
        id.flags = 0;
        require(set_metadata(id, key, value), "failed to set key metadata");
        ArrowSchema* children[] = {&id};
        ArrowSchema root{};
        root.format = "+s";
        root.name = "";
        root.flags = 0;
        root.n_children = 1;
        root.children = children;
        nano_lance::LanceSchemaMapping mapping;
        std::string error;
        const bool ok = nano_lance::map_arrow_schema(root, mapping, error);
        if (ok) {
            out = mapping.fields[0];
        }
        if (id.metadata != nullptr) {
            ArrowFree(const_cast<char*>(id.metadata));
        }
        require(ok, error);
    };

    nano_lance::LanceField key;
    map_one(kKey, "true", key);
    require(key.unenforced_primary_key && key.unenforced_primary_key_position == 0, "a truthy flag is a key at 0");
    map_one(kKey, "TRUE", key);
    require(key.unenforced_primary_key, "the flag is read without case");
    map_one(kKey, "yes", key);
    require(key.unenforced_primary_key, "yes is truthy");
    map_one(kKey, "y", key);
    require(key.unenforced_primary_key, "y is truthy");
    map_one(kKey, "1", key);
    require(key.unenforced_primary_key, "1 is truthy");
    for (const char* falsy : {"false", "0", "no", "", "2", "t", " true"}) {
        map_one(kKey, falsy, key);
        require(!key.unenforced_primary_key, "a falsy flag is not a key");
    }
    map_one(kPosition, "2", key);
    require(key.unenforced_primary_key && key.unenforced_primary_key_position == 2, "a position makes a key");
    map_one(kPosition, "+007", key);
    require(key.unenforced_primary_key && key.unenforced_primary_key_position == 7, "+007 parses as 7");
    map_one(kPosition, "0", key);
    require(key.unenforced_primary_key && key.unenforced_primary_key_position == 0, "position 0 is still a key");
    map_one(kPosition, "4294967295", key);
    require(key.unenforced_primary_key && key.unenforced_primary_key_position == 4294967295U, "u32 max parses");
    map_one(kPosition, "abc", key);
    require(!key.unenforced_primary_key, "an unparseable position without a flag is no key");
    // A position that does not parse does not stop the flag from making a key.
    map_one(kPosition, "-1", key);
    require(!key.unenforced_primary_key, "a position that does not parse is no key");
    map_one(kClustering, "1", key);
    require(!key.unenforced_primary_key && key.unenforced_clustering_key_position == 1 &&
                key.unenforced_primary_key_position == 0,
            "a clustering position is not a primary key");
    map_one(kClustering, "0", key);
    require(key.unenforced_clustering_key_position == 0, "clustering position 0 is not in the key");

    // A non-null leaf inside a non-null struct maps; the Field carries the key, and the metadata stays as given.
    ArrowSchema leaf{};
    leaf.format = "l";
    leaf.name = "id";
    leaf.flags = 0;
    require(set_metadata(leaf, kPosition, "1"), "failed to set key metadata on a struct leaf");
    ArrowSchema* struct_children[] = {&leaf};
    ArrowSchema strukt{};
    strukt.format = "+s";
    strukt.name = "s";
    strukt.flags = 0;
    strukt.n_children = 1;
    strukt.children = struct_children;
    ArrowSchema* root_children[] = {&strukt};
    ArrowSchema root{};
    root.format = "+s";
    root.name = "";
    root.flags = 0;
    root.n_children = 1;
    root.children = root_children;
    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(root, mapping, error), error);
    const auto* leaf_field = find_field(mapping, "id");
    require(leaf_field != nullptr && leaf_field->unenforced_primary_key_position == 1 &&
                leaf_field->metadata.count(kPosition) == 1,
            "a non-null leaf inside a non-null struct is a key, and keeps its metadata");
    if (leaf.metadata != nullptr) {
        ArrowFree(const_cast<char*>(leaf.metadata));
    }

    // The refusals, with pylance 12's messages.
    {
        ArrowSchema nullable_key{};
        nullable_key.format = "l";
        nullable_key.name = "id";
        nullable_key.flags = ARROW_FLAG_NULLABLE;
        require(set_metadata(nullable_key, kKey, "true"), "failed to set key metadata");
        ArrowSchema* children[] = {&nullable_key};
        ArrowSchema root_schema{};
        root_schema.format = "+s";
        root_schema.name = "";
        root_schema.flags = 0;
        root_schema.n_children = 1;
        root_schema.children = children;
        nano_lance::LanceSchemaMapping refused;
        std::string refusal;
        require(!nano_lance::map_arrow_schema(root_schema, refused, refusal), "a nullable key is refused");
        require(refusal == "Primary key column and all its ancestors must not be nullable: "
                           "Field(id=0, name=id, type=int64)",
                refusal.c_str());
        ArrowFree(const_cast<char*>(nullable_key.metadata));
    }
    {
        ArrowSchema list_item{};
        list_item.format = "i";
        list_item.name = "item";
        list_item.flags = ARROW_FLAG_NULLABLE;
        ArrowSchema* item_children[] = {&list_item};
        ArrowSchema list{};
        list.format = "+l";
        list.name = "l";
        list.flags = 0;
        list.n_children = 1;
        list.children = item_children;
        require(set_metadata(list, kKey, "true"), "failed to set key metadata on a list");
        ArrowSchema* children[] = {&list};
        ArrowSchema root_schema{};
        root_schema.format = "+s";
        root_schema.name = "";
        root_schema.flags = 0;
        root_schema.n_children = 1;
        root_schema.children = children;
        nano_lance::LanceSchemaMapping refused;
        std::string refusal;
        require(!nano_lance::map_arrow_schema(root_schema, refused, refusal), "a list key is refused");
        require(refusal == "Primary key column must be a leaf: "
                           "Field(id=0, name=l, type=list, children=[Field(id=1, name=item, type=int32), ])",
                refusal.c_str());
        ArrowFree(const_cast<char*>(list.metadata));
    }
}

}  // namespace

int main() {
    test_flat_schema();
    test_nullable_flag_is_accepted();
    test_unenforced_primary_key();
    test_fixed_size_binary();
    test_dictionary_schema();
    test_extension_struct();
    test_top_level_named_struct_maps_as_field();
    test_golden_blob_ipc_schema();
    return 0;
}
