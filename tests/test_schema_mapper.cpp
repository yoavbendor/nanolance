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
    require(nano_lance::map_arrow_schema(root, mapping, error), error.c_str());
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

void test_nullability_ignore() {
    ArrowSchema nullable{};
    nullable.format = "l";
    nullable.name = "nullable";
    nullable.flags = ARROW_FLAG_NULLABLE;

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(!nano_lance::map_arrow_schema(nullable, mapping, error), "nullable type should fail");
    require(nano_lance::map_arrow_schema(nullable, mapping, error, true), "nullable type should pass with ignore flag");
    require(mapping.fields.size() == 1, "nullable ignore field count mismatch");
    require(!mapping.fields[0].nullable, "nullable ignore should produce non-null Lance field");
}

void test_fixed_size_binary() {
    ArrowSchema mac{};
    mac.format = "w:6";
    mac.name = "src_mac";
    mac.flags = 0;

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(mac, mapping, error), error.c_str());
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

    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    require(nano_lance::map_arrow_schema(uri_index, mapping, error), error.c_str());
    require(mapping.fields.size() == 1, "dictionary field count mismatch");
    require(mapping.fields[0].is_dictionary_index, "dictionary index flag mismatch");
    require(mapping.fields[0].dictionary_value_logical_type == "utf8", "dictionary value type mismatch");
    require(mapping.fields[0].column_index == 0, "dictionary column index mismatch");
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
    require(nano_lance::map_arrow_schema(root, mapping, error), error.c_str());
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
    require(nano_lance::map_arrow_schema(payload, mapping, error), error.c_str());
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
    require(nano_lance::map_arrow_schema(schema, mapping, error, true), error.c_str());

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

}  // namespace

int main() {
    test_flat_schema();
    test_nullability_ignore();
    test_fixed_size_binary();
    test_dictionary_schema();
    test_extension_struct();
    test_top_level_named_struct_maps_as_field();
    test_golden_blob_ipc_schema();
    return 0;
}
