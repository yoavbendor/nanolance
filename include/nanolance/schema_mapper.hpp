// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

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

/// An Arrow extension type Lance itself defines (lance.blob.v2, ...), which nanolance lays out on its
/// own terms. Any other extension type (arrow.fixed_shape_tensor, arrow.uuid, a user's) is stored as
/// its storage type, with the extension recorded in the field's metadata.
inline bool lance_extension_is_lance_owned(const std::string& extension_name) {
    return extension_name.rfind("lance.", 0) == 0;
}

inline bool lance_field_is_physical(const LanceField& field) {
    return field.column_index >= 0;
}

/// A list field, as Lance's schema spells it. A list of structs is `list.struct`; a map is a list of
/// `entries` structs (key, value) and is stored exactly like one.
inline bool lance_logical_type_is_list(const std::string& logical_type) {
    return logical_type == "list" || logical_type == "large_list" || logical_type == "list.struct" ||
           logical_type == "large_list.struct" || logical_type == "map";
}

inline bool lance_logical_type_is_large_list(const std::string& logical_type) {
    return logical_type.rfind("large_list", 0) == 0;
}

inline bool lance_field_is_variable_width(const std::string& logical_type) {
    return logical_type == "utf8" || logical_type == "large_utf8" || logical_type == "string" ||
           logical_type == "large_string" || logical_type == "binary" || logical_type == "large_binary";
}

/// Does this logical type use 64-bit (rather than 32-bit) offsets? Accepts both the in-memory names
/// (`large_utf8`) and the on-disk ones (`large_string`), since the same predicate runs on both sides.
inline bool lance_logical_type_has_large_offsets(const std::string& logical_type) {
    return logical_type == "large_utf8" || logical_type == "large_string" ||
           logical_type == "large_binary";
}

/// Temporal types backed by a 32-bit integer on the wire (`date32:day`, `time32:s`, `time32:ms`).
/// Everything else temporal -- timestamps, `date64:ms`, `time64:*` -- is 64-bit.
inline bool lance_logical_type_is_32bit_temporal(const std::string& logical_type) {
    return logical_type.rfind("date32:", 0) == 0 || logical_type.rfind("time32:", 0) == 0;
}

/// Is this a temporal type? They are integers on the wire, so every fixed-width encoding applies.
/// `duration` is always 64-bit, like a timestamp, and stock Lance bitpacks it the same way.
inline bool lance_logical_type_is_temporal(const std::string& logical_type) {
    return logical_type.rfind("timestamp:", 0) == 0 || logical_type.rfind("date32:", 0) == 0 ||
           logical_type.rfind("date64:", 0) == 0 || logical_type.rfind("time32:", 0) == 0 ||
           logical_type.rfind("time64:", 0) == 0 || logical_type.rfind("duration:", 0) == 0;
}

/// Integer logical types (8/16/32/64-bit) eligible for Lance InlineBitpacking. Excludes bool/float.
inline bool lance_logical_type_is_bitpackable_integer(const std::string& logical_type) {
    // Temporal types are included deliberately: they are integers on the wire, they are usually
    // monotonic (so they bitpack extremely well), and stock Lance bitpacks them too -- a pylance
    // timestamp column's page descriptor reads InlineBitpacking(64). Decimals are excluded: they are
    // 16/32 bytes wide, beyond the FastLanes kernel's 8/16/32/64-bit widths.
    return logical_type == "int8" || logical_type == "uint8" || logical_type == "int16" ||
           logical_type == "uint16" || logical_type == "int32" || logical_type == "uint32" ||
           logical_type == "int64" || logical_type == "uint64" ||
           lance_logical_type_is_temporal(logical_type);
}

/// The widest value Lance lets a ConstantLayout carry inline in its descriptor
/// ("MUST be <= 32 bytes if present", encodings_v2_1.proto). A wider fixed-width constant is not
/// written as a ConstantLayout at all.
inline constexpr std::size_t kMaxInlineConstantBytes = 32U;

/// Lance `file.Field.encoding`: 1 = fixed-width, 2 = variable-width, 0 = none -- which is what
/// pylance writes for a null-typed field, since it has no values to encode.
inline std::int32_t lance_on_disk_field_encoding(const std::string& logical_type) {
    if (logical_type == "null") {
        return 0;
    }
    return lance_field_is_variable_width(logical_type) ? 2 : 1;
}

/// Split Lance's `fixed_size_list:<element type>:<N>` into its parts. The element type can itself
/// contain colons (`timestamp:us:-`), so N is taken from the LAST colon. Returns false for anything
/// else, including N = 0.
inline bool lance_fixed_size_list_parts(const std::string& logical_type, std::string& element,
                                        std::uint64_t& items) {
    static const std::string prefix = "fixed_size_list:";
    if (logical_type.rfind(prefix, 0) != 0) {
        return false;
    }
    const auto last = logical_type.rfind(':');
    if (last == std::string::npos || last <= prefix.size() || last + 1U >= logical_type.size()) {
        return false;
    }
    std::uint64_t n = 0;
    for (std::size_t i = last + 1U; i < logical_type.size(); ++i) {
        const char c = logical_type[i];
        if (c < '0' || c > '9' || n > (1ULL << 32U)) {
            return false;
        }
        n = n * 10U + static_cast<std::uint64_t>(c - '0');
    }
    if (n == 0U) {
        return false;
    }
    element = logical_type.substr(prefix.size(), last - prefix.size());
    items = n;
    return true;
}

inline std::size_t lance_logical_type_value_bytes(const std::string& logical_type);

/// Fixed-width byte size for Lance on-disk logical types (writer/reader parity).
inline std::size_t lance_logical_type_value_bytes(const std::string& logical_type) {
    // A fixed_size_list row is N elements back to back -- which is also exactly how Lance stores it,
    // so the whole row decodes as one fixed-width value.
    {
        std::string element;
        std::uint64_t items = 0;
        if (lance_fixed_size_list_parts(logical_type, element, items)) {
            return static_cast<std::size_t>(items) * lance_logical_type_value_bytes(element);
        }
    }
    if (logical_type == "bool") {
        return 1U;
    }
    if (logical_type == "int8" || logical_type == "uint8") {
        return 1U;
    }
    // halffloat is 2 bytes. Without this line it fell through to the 8-byte default below, which
    // would have read four rows as one.
    if (logical_type == "int16" || logical_type == "uint16" || logical_type == "halffloat") {
        return 2U;
    }
    if (logical_type == "int32" || logical_type == "uint32" || logical_type == "float") {
        return 4U;
    }
    if (lance_logical_type_is_32bit_temporal(logical_type)) {
        return 4U;
    }
    // "decimal:<bits>:<precision>:<scale>" -- the storage width is the bit width, not the precision.
    if (logical_type.rfind("decimal:256:", 0) == 0) {
        return 32U;
    }
    if (logical_type.rfind("decimal:128:", 0) == 0) {
        return 16U;
    }
    if (logical_type.rfind("fixed_size_binary:", 0) == 0) {
        // The logical type comes from the file. std::stoul threw on "fixed_size_binary:x" and the
        // exception escaped the reader (found by fuzz_column_decode); 0 means "no usable width",
        // which every caller already refuses.
        std::size_t width = 0;
        const auto digits = logical_type.substr(18);  // strlen("fixed_size_binary:")
        if (digits.empty()) {
            return 0U;
        }
        for (const char c : digits) {
            if (c < '0' || c > '9' || width > (std::size_t{1} << 31U)) {
                return 0U;
            }
            width = width * 10U + static_cast<std::size_t>(c - '0');
        }
        return width;
    }
    return 8U;
}

/// Map Arrow-style logical type names to Lance on-disk schema strings.
///
/// `large_utf8` must NOT collapse to "string": the data file writes 64-bit offsets for it, and a
/// field declared "string" tells every reader to parse 32-bit ones. That produced a genuinely corrupt
/// file -- stock Lance rejected it outright ("expected 32-bit offsets but got 64-bit offsets") and
/// nanolance's own reader failed with "terminal offset out of range". Lance's on-disk name is
/// "large_string", which is what pylance writes for a pa.large_string() column.
inline std::string lance_on_disk_logical_type(const std::string& logical_type) {
    if (logical_type == "utf8") {
        return "string";
    }
    if (logical_type == "large_utf8") {
        return "large_string";
    }
    return logical_type;
}

std::vector<const LanceField*> lance_physical_fields(const LanceSchemaMapping& mapping);

/// \p ignore_nullability is accepted and ignored; it is kept so existing callers still compile.
/// Nullable-flagged fields are always accepted now -- pyarrow marks essentially every field nullable,
/// so gating on the flag rejected almost every real table while protecting nothing. What actually
/// needed guarding is a null *value*, and that is refused unconditionally during ingest
/// (see append_batch_column_values), where the data is.
bool map_arrow_schema(const ArrowSchema& schema, LanceSchemaMapping& mapping, std::string& error,
                      bool ignore_nullability = false);
/// The same schema, field for field -- names, types, nullability, nesting -- whatever the field ids
/// and column positions. What an append checks a batch against: a dataset's field ids need not run
/// 0..n (a dropped or re-typed column leaves a gap), a new batch's always do.
bool schema_mappings_equivalent(const LanceSchemaMapping& left, const LanceSchemaMapping& right);

/// Number the physical fields' columns 0..n in field order: the layout of one new data file holding
/// every column. A mapping read from a manifest carries each field's position in whichever file of
/// the latest fragment holds it, and those collide once a fragment has several files.
void renumber_columns_for_one_file(LanceSchemaMapping& mapping);

bool schema_mappings_equal(const LanceSchemaMapping& left, const LanceSchemaMapping& right);
/// Human-readable description of the first way `actual` differs from `expected` (column count, or the
/// first field whose name/order, type, nullability, or extension differs). Returns a generic string if
/// they compare equal. Used to produce an informative error when a batch's schema changes mid-write.
std::string describe_schema_mapping_mismatch(const LanceSchemaMapping& expected,
                                             const LanceSchemaMapping& actual);

/// Rebuild `LanceSchemaMapping` from an on-disk manifest (inverse of manifest_writer field mapping).
bool lance_schema_mapping_from_manifest(const pb::Manifest& manifest, LanceSchemaMapping& out, std::string& error);

/// The Arrow C format string for a Lance logical type, as read back from a manifest. Exposed for the
/// one place that needs it outside manifest recovery: building a fixed_size_list's child, whose
/// element type Lance keeps only inside the list's logical type string.
bool lance_arrow_format_for_logical_type(const std::string& logical_type, std::string& arrow_format,
                                         std::string& error);

}  // namespace nano_lance
