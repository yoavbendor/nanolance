#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct ArrowSchema;

namespace nano_lance::pb {
struct Manifest;
}

namespace nano_lance {

struct LanceField {
    std::string name;
    std::string logical_type;
    std::string arrow_format;
    std::int32_t id = 0;
    std::int32_t parent_id = -1;
    /// Physical Lance column index, or -1 for logical-only fields (struct / extension parents).
    std::int32_t column_index = -1;
    bool nullable = true;
    std::string extension_name;
    std::map<std::string, std::string> metadata;
    bool is_dictionary_index = false;
    std::string dictionary_value_logical_type;
};

struct LanceSchemaMapping {
    std::vector<LanceField> fields;
};

inline bool lance_field_is_physical(const LanceField& field) {
    return field.column_index >= 0;
}

inline bool lance_field_is_variable_width(const std::string& logical_type) {
    return logical_type == "utf8" || logical_type == "large_utf8" || logical_type == "string" ||
           logical_type == "binary" || logical_type == "large_binary";
}

/// Integer logical types (8/16/32/64-bit) eligible for Lance InlineBitpacking. Excludes bool/float.
inline bool lance_logical_type_is_bitpackable_integer(const std::string& logical_type) {
    return logical_type == "int8" || logical_type == "uint8" || logical_type == "int16" ||
           logical_type == "uint16" || logical_type == "int32" || logical_type == "uint32" ||
           logical_type == "int64" || logical_type == "uint64";
}

/// Lance `file.Field.encoding`: 1 = fixed-width, 2 = variable-width.
inline std::int32_t lance_on_disk_field_encoding(const std::string& logical_type) {
    return lance_field_is_variable_width(logical_type) ? 2 : 1;
}

/// Fixed-width byte size for Lance on-disk logical types (writer/reader parity).
inline std::size_t lance_logical_type_value_bytes(const std::string& logical_type) {
    if (logical_type == "bool") {
        return 1U;
    }
    if (logical_type == "int8" || logical_type == "uint8") {
        return 1U;
    }
    if (logical_type == "int16" || logical_type == "uint16") {
        return 2U;
    }
    if (logical_type == "int32" || logical_type == "uint32" || logical_type == "float") {
        return 4U;
    }
    if (logical_type.rfind("fixed_size_binary:", 0) == 0) {
        return static_cast<std::size_t>(std::stoul(logical_type.substr(18)));  // strlen("fixed_size_binary:")
    }
    return 8U;
}

/// Map Arrow-style logical type names to Lance on-disk schema strings.
inline std::string lance_on_disk_logical_type(const std::string& logical_type) {
    if (logical_type == "utf8" || logical_type == "large_utf8") {
        return "string";
    }
    return logical_type;
}

std::vector<const LanceField*> lance_physical_fields(const LanceSchemaMapping& mapping);

bool map_arrow_schema(const ArrowSchema& schema, LanceSchemaMapping& mapping, std::string& error, bool ignore_nullability = false);
bool schema_mappings_equal(const LanceSchemaMapping& left, const LanceSchemaMapping& right);

/// Rebuild `LanceSchemaMapping` from an on-disk manifest (inverse of manifest_writer field mapping).
bool lance_schema_mapping_from_manifest(const pb::Manifest& manifest, LanceSchemaMapping& out, std::string& error);

}  // namespace nano_lance
