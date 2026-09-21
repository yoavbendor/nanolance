// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/schema_mapper.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace nano_lance {
namespace {

constexpr const char* kArrowExtensionNameKey = "ARROW:extension:name";

struct ParsedFormat {
    std::string logical_type;
    bool supported = false;
    /// Set when the format is understood but deliberately refused, so the caller can report WHY
    /// instead of the generic "unsupported Arrow C format".
    std::string rejection;
};

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

ParsedFormat parse_format(const char* format) {
    ParsedFormat out;
    if (format == nullptr) {
        return out;
    }
    if (std::strcmp(format, "n") == 0) {
        out.logical_type = "null";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "c") == 0) {
        out.logical_type = "int8";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "C") == 0) {
        out.logical_type = "uint8";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "s") == 0) {
        out.logical_type = "int16";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "S") == 0) {
        out.logical_type = "uint16";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "i") == 0) {
        out.logical_type = "int32";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "I") == 0) {
        out.logical_type = "uint32";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "l") == 0) {
        out.logical_type = "int64";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "L") == 0) {
        out.logical_type = "uint64";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "f") == 0) {
        out.logical_type = "float";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "g") == 0) {
        out.logical_type = "double";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "b") == 0) {
        out.logical_type = "bool";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "u") == 0) {
        out.logical_type = "utf8";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "U") == 0) {
        out.logical_type = "large_utf8";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "z") == 0) {
        out.logical_type = "binary";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "Z") == 0) {
        out.logical_type = "large_binary";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "+s") == 0) {
        out.logical_type = "struct";
        out.supported = true;
        return out;
    }
    // ── Temporal and decimal types ───────────────────────────────────────────────────────────────
    //
    // All of these are plain fixed-width integers on the wire, so no encoder work is involved: the
    // only reason they were rejected is that nothing mapped their Arrow format strings. The Lance
    // logical-type names below are not invented -- each was read back out of a manifest written by
    // pylance 12.0.0, so a column nanolance writes is described exactly as stock Lance describes its
    // own. They carry every parameter (unit, timezone, precision, scale), which is why they are used
    // verbatim as the internal logical type too: no lossy translation table to keep in step.
    //
    // Arrow C format reference: timestamp "ts{s,m,u,n}:<tz>", date "tdD"/"tdm",
    // time "tts"/"ttm"/"ttu"/"ttn", decimal "d:<precision>,<scale>[,<bits>]".
    if (starts_with(format, "ts") && std::strlen(format) >= 4U && format[3] == ':') {
        const char* unit = nullptr;
        switch (format[2]) {
            case 's': unit = "s"; break;
            case 'm': unit = "ms"; break;
            case 'u': unit = "us"; break;
            case 'n': unit = "ns"; break;
            default: break;
        }
        if (unit != nullptr) {
            const std::string tz(format + 4);
            // Lance only understands IANA zone NAMES ("UTC", "Europe/Berlin"). Given an offset form
            // like "+05:30" its schema layer raises "Unsupported timestamp type" -- and pylance
            // surfaces that as a Rust panic, on read AND on write, so it cannot produce such a file
            // either. Refuse here rather than emit one no reference reader will open.
            if (!tz.empty() && (tz[0] == '+' || tz[0] == '-')) {
                out.rejection =
                    "timestamp timezone '" + tz +
                    "' is a UTC offset; Lance supports only IANA zone names (e.g. \"UTC\", "
                    "\"Europe/Berlin\") and panics on offsets. Convert the column to a named zone "
                    "or to a naive timestamp first.";
                return out;
            }
            // Lance spells "no timezone" as "-", never as an empty field.
            out.logical_type = std::string("timestamp:") + unit + ":" + (tz.empty() ? "-" : tz);
            out.supported = true;
            return out;
        }
    }
    if (std::strcmp(format, "tdD") == 0) {
        out.logical_type = "date32:day";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "tdm") == 0) {
        out.logical_type = "date64:ms";
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "tts") == 0 || std::strcmp(format, "ttm") == 0) {
        out.logical_type = std::string("time32:") + (format[2] == 's' ? "s" : "ms");
        out.supported = true;
        return out;
    }
    if (std::strcmp(format, "ttu") == 0 || std::strcmp(format, "ttn") == 0) {
        out.logical_type = std::string("time64:") + (format[2] == 'u' ? "us" : "ns");
        out.supported = true;
        return out;
    }
    if (starts_with(format, "d:")) {
        // "d:precision,scale" is 128-bit; "d:precision,scale,bits" names the width explicitly.
        const std::string spec(format + 2);
        const auto comma = spec.find(',');
        if (comma != std::string::npos) {
            const auto precision = spec.substr(0, comma);
            auto rest = spec.substr(comma + 1U);
            std::string bits = "128";
            const auto second = rest.find(',');
            if (second != std::string::npos) {
                bits = rest.substr(second + 1U);
                rest = rest.substr(0, second);
            }
            if (!precision.empty() && !rest.empty() && (bits == "128" || bits == "256")) {
                out.logical_type = "decimal:" + bits + ":" + precision + ":" + rest;
                out.supported = true;
                return out;
            }
        }
        return out;  // unsupported decimal width, or a malformed spec: refuse rather than guess
    }

    if (starts_with(format, "w:")) {
        // Carry the byte width through (Lance's own logical type is "fixed_size_binary:<N>"), so it can
        // be recovered from the manifest on read.
        out.logical_type = std::string("fixed_size_binary:") + (format + 2);
        out.supported = true;
        return out;
    }
    return out;
}

bool is_nullable(const ArrowSchema& schema) {
    return (schema.flags & ARROW_FLAG_NULLABLE) != 0;
}

bool read_metadata_key(const ArrowSchema& schema, const char* key, std::string& value) {
    value.clear();
    if (schema.metadata == nullptr) {
        return false;
    }
    struct ArrowStringView key_view {};
    key_view.data = key;
    key_view.size_bytes = static_cast<int64_t>(std::strlen(key));
    struct ArrowStringView value_view {};
    if (ArrowMetadataGetValue(schema.metadata, key_view, &value_view) != NANOARROW_OK) {
        return false;
    }
    value.assign(value_view.data, value_view.data + value_view.size_bytes);
    return true;
}

bool is_record_batch_schema(const ArrowSchema& schema) {
    if (schema.n_children <= 0 || schema.children == nullptr) {
        return false;
    }
    std::string extension_name;
    if (read_metadata_key(schema, kArrowExtensionNameKey, extension_name)) {
        return false;
    }
    return schema.name == nullptr || schema.name[0] == '\0';
}

void copy_metadata(const ArrowSchema& schema, std::map<std::string, std::string>& out) {
    out.clear();
    if (schema.metadata == nullptr) {
        return;
    }
    struct ArrowMetadataReader reader {};
    if (ArrowMetadataReaderInit(&reader, schema.metadata) != NANOARROW_OK) {
        return;
    }
    struct ArrowStringView key {};
    struct ArrowStringView value {};
    while (ArrowMetadataReaderRead(&reader, &key, &value) == NANOARROW_OK) {
        out.emplace(std::string(key.data, key.data + key.size_bytes),
                    std::string(value.data, value.data + value.size_bytes));
    }
}

bool map_field(const ArrowSchema& field,
               std::int32_t parent_id,
               std::int32_t& next_id,
               std::int32_t& next_column,
               LanceSchemaMapping& mapping,
               std::string& error);

bool map_struct_children(const ArrowSchema& field,
                         std::int32_t parent_id,
                         std::int32_t& next_id,
                         std::int32_t& next_column,
                                   LanceSchemaMapping& mapping,
                         std::string& error) {
    if (field.n_children <= 0 || field.children == nullptr) {
        error = "struct field has no children: ";
        error += field.name == nullptr ? "<unnamed>" : field.name;
        return false;
    }
    for (std::int64_t i = 0; i < field.n_children; ++i) {
        if (field.children[i] == nullptr) {
            error = "struct field has null child";
            return false;
        }
        if (!map_field(*field.children[i], parent_id, next_id, next_column, mapping, error)) {
            return false;
        }
    }
    return true;
}

bool map_field(const ArrowSchema& field,
               std::int32_t parent_id,
               std::int32_t& next_id,
               std::int32_t& next_column,
               LanceSchemaMapping& mapping,
               std::string& error) {
    const char* format = field.format == nullptr ? "" : field.format;
    const auto parsed = parse_format(format);
    if (!parsed.supported) {
        error = "column '";
        error += field.name == nullptr ? "<unnamed>" : field.name;
        error += "': ";
        if (!parsed.rejection.empty()) {
            error += parsed.rejection;
        } else {
            error += "unsupported Arrow C format: ";
            error += format;
        }
        return false;
    }
    std::string extension_name;
    read_metadata_key(field, kArrowExtensionNameKey, extension_name);

    const bool is_struct = parsed.logical_type == "struct";
    const bool is_dictionary = field.dictionary != nullptr;

    // large_utf8 / large_binary produce a file stock Lance rejects as corrupt. nanolance writes the
    // 64-bit Arrow offsets straight into the miniblock chunk, but Lance v2.2 miniblock pages require
    // the u32 chunk grammar ("expected 32-bit offsets but got 64-bit offsets"). Lance keeps u32
    // offsets INSIDE the chunk for large types too and signals the 64-bit Arrow width only in the
    // page layout's Variable{offsets = Flat{bits}} node -- pylance's string and large_string page
    // descriptors are byte-identical apart from that one token (0x20 vs 0x40). Supporting these
    // properly therefore means decoupling the chunk offset width from the declared Arrow width
    // across every variable-width page path (plain, zstd, dict, dict+RLE, constant); until that
    // lands, refuse rather than emit a file no reader accepts.
    //
    // Scoped to columns that go through the generic variable-width page path. A lance.blob.v2
    // struct's `data` child is declared large_binary but is encoded by the blob-v2 packed writer,
    // which never builds a miniblock chunk, so it is unaffected and must keep working.
    const LanceField* mapped_parent = nullptr;
    for (const auto& candidate : mapping.fields) {
        if (parent_id >= 0 && candidate.id == parent_id) {
            mapped_parent = &candidate;
            break;
        }
    }
    const bool under_blob_v2 = mapped_parent != nullptr && mapped_parent->extension_name == "lance.blob.v2";
    // The Arrow null type is an all-null column by definition, so it runs into the same wall as any
    // other null: nanolance writes no validity information and has nothing to store. Say so here
    // rather than letting ingest fail later with "fixed-width array is missing values buffer".
    if (parsed.logical_type == "null") {
        error = "column '";
        error += field.name == nullptr ? "<unnamed>" : field.name;
        error +=
            "' has Arrow's null type, which is all-null by definition; nanolance cannot store nulls "
            "yet (it writes no validity information). Drop the column, or give it a concrete type "
            "and a fill value.";
        return false;
    }

    if (!under_blob_v2 && (parsed.logical_type == "large_utf8" || parsed.logical_type == "large_binary")) {
        error = "column '";
        error += field.name == nullptr ? "<unnamed>" : field.name;
        error += "' has type " + parsed.logical_type +
                 ", which nanolance cannot write yet (it would emit 64-bit offsets inside a Lance "
                 "v2.2 miniblock page, which requires the u32 chunk grammar, and stock Lance "
                 "rejects the result as corrupt). Use utf8 / binary instead (pyarrow: "
                 "col.cast(pa.string()) / col.cast(pa.binary())).";
        return false;
    }

    LanceField out;
    out.name = field.name == nullptr ? "" : field.name;
    out.logical_type = parsed.logical_type;
    out.arrow_format = format;
    out.id = next_id++;
    out.parent_id = parent_id;
    out.nullable = false;
    out.extension_name = extension_name;
    copy_metadata(field, out.metadata);

    // An Arrow dictionary column used to be written as a bare index column with the dictionary
    // VALUES stored nowhere at all: pa.array(["a","b","a"]).dictionary_encode() became an int32
    // column reading back [0, 1, 0], with no record of what 0 and 1 meant. Nothing downstream could
    // detect the loss -- the file is a perfectly valid int32 column to every reader. Refuse it.
    //
    // The remedy costs nothing on disk: nanolance already dictionary-encodes low-cardinality string
    // columns on its own (structural dict / dict+RLE, on by default), so casting to plain utf8 gives
    // the same file size without the Arrow-level dictionary.
    if (is_dictionary) {
        error = "dictionary-encoded column '";
        error += out.name;
        error +=
            "' is not supported: nanolance would store only the integer indices and discard the "
            "dictionary values. Cast it to its value type first (pyarrow: "
            "col.cast(pa.string()), or table.cast(...)); nanolance dictionary-encodes "
            "low-cardinality string columns on disk by itself, so the file stays the same size.";
        return false;
    }

    if (is_struct || !extension_name.empty()) {
        out.column_index = -1;
    } else {
        out.column_index = next_column++;
    }

    mapping.fields.push_back(out);

    if (is_struct) {
        return map_struct_children(field, out.id, next_id, next_column, mapping, error);
    }
    return true;
}

// Physical encoding annotations the writer stamps onto fields at commit (packing, zstd hint, const
// value). They are per-fragment choices, not part of the logical schema, so they must be ignored when
// deciding whether the user changed the schema between batches — otherwise an appended batch (whose
// schema was reloaded from a manifest that carries these tags) would look like a schema change.
bool is_encoding_metadata_key(const std::string& key) {
    return key.rfind("nanolance:", 0) == 0 || key.rfind("lance-encoding:", 0) == 0;
}

bool metadata_equal_ignoring_encoding(const std::map<std::string, std::string>& lhs,
                                      const std::map<std::string, std::string>& rhs) {
    auto logical_only = [](const std::map<std::string, std::string>& m) {
        std::map<std::string, std::string> out;
        for (const auto& [k, v] : m) {
            if (!is_encoding_metadata_key(k)) {
                out.emplace(k, v);
            }
        }
        return out;
    };
    return logical_only(lhs) == logical_only(rhs);
}

bool mappings_field_equal(const LanceField& lhs, const LanceField& rhs) {
    return lhs.name == rhs.name && lhs.logical_type == rhs.logical_type && lhs.arrow_format == rhs.arrow_format &&
           lhs.id == rhs.id && lhs.parent_id == rhs.parent_id && lhs.column_index == rhs.column_index &&
           lhs.nullable == rhs.nullable && lhs.extension_name == rhs.extension_name &&
           metadata_equal_ignoring_encoding(lhs.metadata, rhs.metadata) &&
           lhs.is_dictionary_index == rhs.is_dictionary_index &&
           lhs.dictionary_value_logical_type == rhs.dictionary_value_logical_type;
}

}  // namespace

std::vector<const LanceField*> lance_physical_fields(const LanceSchemaMapping& mapping) {
    std::vector<const LanceField*> out;
    out.reserve(mapping.fields.size());
    for (const auto& field : mapping.fields) {
        if (lance_field_is_physical(field)) {
            out.push_back(&field);
        }
    }
    return out;
}

bool map_arrow_schema(const ArrowSchema& schema, LanceSchemaMapping& mapping, std::string& error,
                      bool /*ignore_nullability*/) {
    mapping.fields.clear();
    error.clear();

    std::int32_t next_id = 0;
    std::int32_t next_column = 0;

    if (is_record_batch_schema(schema)) {
        for (std::int64_t i = 0; i < schema.n_children; ++i) {
            if (schema.children[i] == nullptr) {
                error = "schema has null child";
                return false;
            }
            if (!map_field(*schema.children[i], -1, next_id, next_column, mapping, error)) {
                return false;
            }
        }
        return true;
    }

    return map_field(schema, -1, next_id, next_column, mapping, error);
}

bool schema_mappings_equal(const LanceSchemaMapping& left, const LanceSchemaMapping& right) {
    if (left.fields.size() != right.fields.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.fields.size(); ++i) {
        if (!mappings_field_equal(left.fields[i], right.fields[i])) {
            return false;
        }
    }
    return true;
}

std::string describe_schema_mapping_mismatch(const LanceSchemaMapping& expected,
                                             const LanceSchemaMapping& actual) {
    if (expected.fields.size() != actual.fields.size()) {
        return "column count differs (writer schema has " + std::to_string(expected.fields.size()) +
               " field(s), new batch has " + std::to_string(actual.fields.size()) + ")";
    }
    const auto describe_field = [](const LanceField& f) {
        std::string type = !f.extension_name.empty() ? f.extension_name
                           : !f.logical_type.empty() ? f.logical_type
                                                     : f.arrow_format;
        return "'" + f.name + "' (" + type + ")";
    };
    for (std::size_t i = 0; i < expected.fields.size(); ++i) {
        const auto& e = expected.fields[i];
        const auto& a = actual.fields[i];
        if (mappings_field_equal(e, a)) {
            continue;
        }
        std::string reason;
        if (e.name != a.name) {
            reason = "field name/order";
        } else if (e.logical_type != a.logical_type || e.arrow_format != a.arrow_format ||
                   e.dictionary_value_logical_type != a.dictionary_value_logical_type) {
            reason = "type";
        } else if (e.nullable != a.nullable) {
            reason = "nullability";
        } else if (e.extension_name != a.extension_name) {
            reason = "extension type";
        } else {
            reason = "field definition";
        }
        return reason + " mismatch at column " + std::to_string(i) + ": writer schema has " +
               describe_field(e) + ", new batch has " + describe_field(a);
    }
    return "schemas differ";
}

namespace {

std::string disk_logical_type_to_internal(const std::string& disk) {
    if (disk == "string") {
        return "utf8";
    }
    if (disk == "large_string") {
        return "large_utf8";
    }
    return disk;
}

bool infer_arrow_format_from_internal(const std::string& logical_type, std::string& arrow_format, std::string& error) {
    if (logical_type == "null") {
        arrow_format = "n";
        return true;
    }
    if (logical_type == "int8") {
        arrow_format = "c";
        return true;
    }
    if (logical_type == "uint8") {
        arrow_format = "C";
        return true;
    }
    if (logical_type == "int16") {
        arrow_format = "s";
        return true;
    }
    if (logical_type == "uint16") {
        arrow_format = "S";
        return true;
    }
    if (logical_type == "int32") {
        arrow_format = "i";
        return true;
    }
    if (logical_type == "uint32") {
        arrow_format = "I";
        return true;
    }
    if (logical_type == "int64") {
        arrow_format = "l";
        return true;
    }
    if (logical_type == "uint64") {
        arrow_format = "L";
        return true;
    }
    if (logical_type == "float") {
        arrow_format = "f";
        return true;
    }
    if (logical_type == "double") {
        arrow_format = "g";
        return true;
    }
    if (logical_type == "bool") {
        arrow_format = "b";
        return true;
    }
    if (logical_type == "utf8") {
        arrow_format = "u";
        return true;
    }
    if (logical_type == "large_utf8") {
        arrow_format = "U";
        return true;
    }
    if (logical_type == "binary") {
        arrow_format = "z";
        return true;
    }
    if (logical_type == "large_binary") {
        arrow_format = "Z";
        return true;
    }
    if (logical_type == "struct") {
        arrow_format = "+s";
        return true;
    }
    // Inverse of the temporal/decimal mapping in parse_format. Reconstructed from the Lance logical
    // type alone, which carries every parameter, so a column read back from a manifest gets its unit,
    // timezone, precision and scale -- not just its storage width.
    if (logical_type.rfind("timestamp:", 0) == 0) {
        const std::string rest = logical_type.substr(std::strlen("timestamp:"));
        const auto colon = rest.find(':');
        if (colon == std::string::npos) {
            error = "malformed timestamp logical type: " + logical_type;
            return false;
        }
        const auto unit = rest.substr(0, colon);
        const auto tz = rest.substr(colon + 1U);
        const char* code = unit == "s" ? "s" : unit == "ms" ? "m" : unit == "us" ? "u" : unit == "ns" ? "n" : nullptr;
        if (code == nullptr) {
            error = "unsupported timestamp unit in logical type: " + logical_type;
            return false;
        }
        // Lance's "-" means no timezone; Arrow spells that as an empty field after the colon.
        arrow_format = std::string("ts") + code + ":" + (tz == "-" ? "" : tz);
        return true;
    }
    if (logical_type == "date32:day") {
        arrow_format = "tdD";
        return true;
    }
    if (logical_type == "date64:ms") {
        arrow_format = "tdm";
        return true;
    }
    if (logical_type == "time32:s" || logical_type == "time32:ms") {
        arrow_format = logical_type == "time32:s" ? "tts" : "ttm";
        return true;
    }
    if (logical_type == "time64:us" || logical_type == "time64:ns") {
        arrow_format = logical_type == "time64:us" ? "ttu" : "ttn";
        return true;
    }
    if (logical_type.rfind("decimal:", 0) == 0) {
        // decimal:<bits>:<precision>:<scale> -> Arrow "d:<precision>,<scale>[,256]"
        const std::string rest = logical_type.substr(std::strlen("decimal:"));
        const auto first = rest.find(':');
        const auto second = first == std::string::npos ? std::string::npos : rest.find(':', first + 1U);
        if (second == std::string::npos) {
            error = "malformed decimal logical type: " + logical_type;
            return false;
        }
        const auto bits = rest.substr(0, first);
        const auto precision = rest.substr(first + 1U, second - first - 1U);
        const auto scale = rest.substr(second + 1U);
        if (bits != "128" && bits != "256") {
            error = "unsupported decimal width in logical type: " + logical_type;
            return false;
        }
        arrow_format = "d:" + precision + "," + scale + (bits == "256" ? ",256" : "");
        return true;
    }

    if (logical_type.rfind("fixed_size_binary:", 0) == 0) {
        arrow_format = "w:" + logical_type.substr(std::strlen("fixed_size_binary:"));
        return true;
    }
    if (logical_type == "fixed_size_binary") {
        error = "fixed_size_binary on-disk logical type is missing its width (expected fixed_size_binary:N)";
        return false;
    }
    error = "unsupported on-disk logical type for manifest recovery: " + logical_type;
    return false;
}

const pb::DataFile* pick_latest_data_file(const pb::Manifest& manifest) {
    for (auto it = manifest.fragments.rbegin(); it != manifest.fragments.rend(); ++it) {
        if (!it->files.empty()) {
            return &it->files[0];
        }
    }
    return nullptr;
}

bool dematerialize_blob_v2_for_arrow_append(LanceSchemaMapping& mapping, std::string& error) {
    error.clear();
    std::size_t blob_index = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < mapping.fields.size(); ++i) {
        const auto& f = mapping.fields[i];
        if (f.parent_id == -1 && f.logical_type == "struct" && f.extension_name == "lance.blob.v2") {
            blob_index = i;
            break;
        }
    }
    if (blob_index == static_cast<std::size_t>(-1)) {
        return true;
    }
    const auto& blob_parent = mapping.fields[blob_index];
    for (const auto& f : mapping.fields) {
        if (f.parent_id == blob_parent.id && f.name == "data") {
            return true;
        }
    }

    std::vector<LanceField> rebuilt;
    rebuilt.reserve(mapping.fields.size());
    for (std::size_t i = 0; i < mapping.fields.size();) {
        const auto& f = mapping.fields[i];
        if (i == blob_index) {
            rebuilt.push_back(f);
            ++i;
            while (i < mapping.fields.size() && mapping.fields[i].parent_id == blob_parent.id) {
                ++i;
            }
            const std::int32_t b = blob_parent.id;
            auto push_child = [&](const std::string& name, const std::string& logical, const std::string& format,
                                  std::int32_t fid) {
                LanceField c;
                c.name = name;
                c.logical_type = logical;
                c.arrow_format = format;
                c.id = fid;
                c.parent_id = blob_parent.id;
                c.column_index = -1;
                c.nullable = false;
                c.extension_name.clear();
                rebuilt.push_back(std::move(c));
            };
            push_child("data", "large_binary", "Z", b + 1);
            push_child("uri", "utf8", "u", b + 2);
            push_child("position", "uint64", "L", b + 3);
            push_child("size", "uint64", "L", b + 4);
            continue;
        }
        rebuilt.push_back(f);
        ++i;
    }
    mapping.fields = std::move(rebuilt);
    return true;
}

}  // namespace

bool lance_schema_mapping_from_manifest(const pb::Manifest& manifest, LanceSchemaMapping& out, std::string& error) {
    out.fields.clear();
    error.clear();
    const auto* data_file = pick_latest_data_file(manifest);
    if (data_file == nullptr) {
        error = "manifest has no data files";
        return false;
    }
    if (data_file->fields.size() != data_file->column_indices.size()) {
        error = "manifest data file field id / column index length mismatch";
        return false;
    }
    std::unordered_map<std::int32_t, std::int32_t> id_to_column;
    id_to_column.reserve(data_file->fields.size());
    for (std::size_t i = 0; i < data_file->fields.size(); ++i) {
        id_to_column.emplace(data_file->fields[i], data_file->column_indices[i]);
    }

    for (const auto& pf : manifest.fields) {
        LanceField lf;
        lf.name = pf.name;
        lf.logical_type = disk_logical_type_to_internal(pf.logical_type);
        if (!infer_arrow_format_from_internal(lf.logical_type, lf.arrow_format, error)) {
            return false;
        }
        lf.id = pf.id;
        lf.parent_id = pf.parent_id;
        lf.nullable = pf.nullable;
        lf.is_dictionary_index = false;
        lf.dictionary_value_logical_type.clear();
        for (const auto& kv : pf.metadata) {
            lf.metadata.emplace(kv.first, std::string(kv.second.begin(), kv.second.end()));
        }
        std::string extension_name;
        const auto ext_it = lf.metadata.find("ARROW:extension:name");
        if (ext_it != lf.metadata.end()) {
            extension_name = ext_it->second;
        }
        lf.extension_name = extension_name;
        const auto col_it = id_to_column.find(pf.id);
        lf.column_index = col_it != id_to_column.end() ? col_it->second : -1;
        out.fields.push_back(std::move(lf));
    }
    if (!dematerialize_blob_v2_for_arrow_append(out, error)) {
        return false;
    }
    return true;
}

}  // namespace nano_lance
