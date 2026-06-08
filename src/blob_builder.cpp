#include "nanolance/blob_builder.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstring>
#include <utility>

namespace nano_lance {
namespace {

constexpr const char* kArrowExtensionNameKey = "ARROW:extension:name";

bool set_schema_metadata(ArrowSchema& schema, const char* key, const char* value) {
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

bool init_child_schema(ArrowSchema& parent, std::int64_t count) {
    if (ArrowSchemaSetTypeStruct(&parent, count) != NANOARROW_OK) {
        return false;
    }
    for (std::int64_t i = 0; i < count; ++i) {
        if (ArrowSchemaSetName(parent.children[i], "") != NANOARROW_OK) {
            return false;
        }
    }
    return true;
}

bool set_child_type(ArrowSchema& parent, std::int64_t index, enum ArrowType type, const char* name) {
    if (ArrowSchemaSetType(parent.children[index], type) != NANOARROW_OK) {
        return false;
    }
    return ArrowSchemaSetName(parent.children[index], name) == NANOARROW_OK;
}

bool append_null_bytes(ArrowArray& array) {
    return ArrowArrayAppendNull(&array, 1) == NANOARROW_OK;
}

bool append_uint64_child(ArrowArray& array, std::uint64_t value) {
    return ArrowArrayAppendUInt(&array, value) == NANOARROW_OK;
}

bool append_string_child(ArrowArray& array, const std::string& value) {
    ArrowStringView view{value.data(), static_cast<int64_t>(value.size())};
    return ArrowArrayAppendString(&array, view) == NANOARROW_OK;
}

bool append_large_bytes_child(ArrowArray& array, const std::vector<std::uint8_t>& value) {
    ArrowBufferView view{value.data(), static_cast<int64_t>(value.size())};
    return ArrowArrayAppendBytes(&array, view) == NANOARROW_OK;
}

bool validate_row(const BlobV2Row& row, std::string& error) {
    const bool has_inline = row.inline_data.has_value();
    const bool has_uri = row.uri.has_value() && !row.uri->empty();
    if (has_inline == has_uri) {
        error = "blob v2 row must contain exactly one of inline data or external uri";
        return false;
    }
    if (has_uri && row.uri->empty()) {
        error = "blob v2 external uri must not be empty";
        return false;
    }
    return true;
}

}  // namespace

bool build_blob_v2_payload_schema(ArrowSchema& schema, std::string& error) {
    error.clear();
    ArrowSchemaInit(&schema);
    if (!init_child_schema(schema, 4)) {
        error = "failed to allocate blob v2 payload struct schema";
        return false;
    }
    if (!set_child_type(schema, 0, NANOARROW_TYPE_LARGE_BINARY, "data") ||
        !set_child_type(schema, 1, NANOARROW_TYPE_STRING, "uri") ||
        !set_child_type(schema, 2, NANOARROW_TYPE_UINT64, "position") ||
        !set_child_type(schema, 3, NANOARROW_TYPE_UINT64, "size")) {
        error = "failed to set blob v2 payload child schema types";
        ArrowSchemaRelease(&schema);
        return false;
    }
    if (!set_schema_metadata(schema, kArrowExtensionNameKey, kBlobV2ExtensionName)) {
        error = "failed to set blob v2 extension metadata";
        ArrowSchemaRelease(&schema);
        return false;
    }
    if (ArrowSchemaSetName(&schema, "payload_ref") != NANOARROW_OK) {
        error = "failed to set blob v2 payload schema name";
        ArrowSchemaRelease(&schema);
        return false;
    }
    schema.flags = 0;
    return true;
}

bool build_epb_table_schema(ArrowSchema& schema, std::string& error) {
    error.clear();
    ArrowSchemaInit(&schema);
    if (!init_child_schema(schema, 2)) {
        error = "failed to allocate epb table schema";
        return false;
    }
    if (ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_UINT64) != NANOARROW_OK ||
        ArrowSchemaSetName(schema.children[0], "packet_id") != NANOARROW_OK) {
        error = "failed to set packet_id schema";
        ArrowSchemaRelease(&schema);
        return false;
    }
    if (!build_blob_v2_payload_schema(*schema.children[1], error)) {
        ArrowSchemaRelease(&schema);
        return false;
    }
    schema.flags = 0;
    return true;
}

bool build_blob_v2_payload_array(const std::vector<BlobV2Row>& rows, ArrowArray& array, std::string& error) {
    error.clear();
    ArrowSchema schema;
    if (!build_blob_v2_payload_schema(schema, error)) {
        return false;
    }
    if (ArrowArrayInitFromSchema(&array, &schema, nullptr) != NANOARROW_OK) {
        error = "failed to init blob v2 payload array";
        ArrowSchemaRelease(&schema);
        return false;
    }
    ArrowSchemaRelease(&schema);

    if (ArrowArrayStartAppending(&array) != NANOARROW_OK) {
        error = "failed to start appending blob v2 payload array";
        ArrowArrayRelease(&array);
        return false;
    }

    auto* data = array.children[0];
    auto* uri = array.children[1];
    auto* position = array.children[2];
    auto* size_field = array.children[3];
    for (const auto& row : rows) {
        if (!validate_row(row, error)) {
            ArrowArrayRelease(&array);
            return false;
        }
        if (row.inline_data.has_value()) {
            if (!append_large_bytes_child(*data, *row.inline_data) || !append_null_bytes(*uri) ||
                !append_uint64_child(*position, row.position) ||
                !append_uint64_child(*size_field, row.size)) {
                error = "failed to append inline blob v2 row";
                ArrowArrayRelease(&array);
                return false;
            }
        } else {
            if (!append_null_bytes(*data) || !append_string_child(*uri, *row.uri) ||
                !append_uint64_child(*position, row.position) || !append_uint64_child(*size_field, row.size)) {
                error = "failed to append external blob v2 row";
                ArrowArrayRelease(&array);
                return false;
            }
        }
        if (ArrowArrayFinishElement(&array) != NANOARROW_OK) {
            error = "failed to finish blob v2 struct element";
            ArrowArrayRelease(&array);
            return false;
        }
    }

    if (ArrowArrayFinishBuildingDefault(&array, nullptr) != NANOARROW_OK) {
        error = "failed to finalize blob v2 payload array";
        ArrowArrayRelease(&array);
        return false;
    }
    return true;
}

bool build_epb_table_array(const std::vector<std::uint64_t>& packet_ids,
                           const std::vector<BlobV2Row>& payload_rows,
                           ArrowArray& array,
                           std::string& error) {
    error.clear();
    if (packet_ids.size() != payload_rows.size()) {
        error = "packet_id and payload_ref row counts must match";
        return false;
    }

    ArrowSchema schema;
    if (!build_epb_table_schema(schema, error)) {
        return false;
    }
    if (ArrowArrayInitFromSchema(&array, &schema, nullptr) != NANOARROW_OK) {
        error = "failed to init epb table array";
        ArrowSchemaRelease(&schema);
        return false;
    }
    ArrowSchemaRelease(&schema);

    if (ArrowArrayStartAppending(&array) != NANOARROW_OK) {
        error = "failed to start appending epb table array";
        ArrowArrayRelease(&array);
        return false;
    }

    auto* packet_id = array.children[0];
    auto* payload = array.children[1];
    auto* data = payload->children[0];
    auto* uri = payload->children[1];
    auto* position = payload->children[2];
    auto* size_field = payload->children[3];

    for (std::size_t row_index = 0; row_index < packet_ids.size(); ++row_index) {
        const auto& row = payload_rows[row_index];
        if (!validate_row(row, error)) {
            ArrowArrayRelease(&array);
            return false;
        }
        if (!append_uint64_child(*packet_id, packet_ids[row_index])) {
            error = "failed to append packet_id value";
            ArrowArrayRelease(&array);
            return false;
        }
        if (row.inline_data.has_value()) {
            if (!append_large_bytes_child(*data, *row.inline_data) || !append_null_bytes(*uri) ||
                !append_uint64_child(*position, row.position) ||
                !append_uint64_child(*size_field, row.size)) {
                error = "failed to append inline blob v2 row";
                ArrowArrayRelease(&array);
                return false;
            }
        } else {
            if (!append_null_bytes(*data) || !append_string_child(*uri, *row.uri) ||
                !append_uint64_child(*position, row.position) || !append_uint64_child(*size_field, row.size)) {
                error = "failed to append external blob v2 row";
                ArrowArrayRelease(&array);
                return false;
            }
        }
        if (ArrowArrayFinishElement(payload) != NANOARROW_OK) {
            error = "failed to finish payload_ref struct element";
            ArrowArrayRelease(&array);
            return false;
        }
        if (ArrowArrayFinishElement(&array) != NANOARROW_OK) {
            error = "failed to finish epb table struct element";
            ArrowArrayRelease(&array);
            return false;
        }
    }

    if (ArrowArrayFinishBuildingDefault(&array, nullptr) != NANOARROW_OK) {
        error = "failed to finalize epb table array";
        ArrowArrayRelease(&array);
        return false;
    }
    return true;
}

}  // namespace nano_lance
