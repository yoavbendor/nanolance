// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/lance_table_reader.hpp"

#include "nanolance/blob_v2_external.hpp"
#include "nanolance/column_slice.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/deletion_vector.hpp"
#include "nanolance/lance_column_decoder.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/path_safety.hpp"
#include "nanolance/read_safety.hpp"
#include "nanolance/schema_mapper.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace nano_lance {
namespace {

/// Same lookup scoped to one parent, so a struct child named `id` does not resolve to a top-level
/// `id`. `parent_id` is -1 for top-level fields, matching LanceField::parent_id.
const LanceField* find_mapping_field_by_name_under(const LanceSchemaMapping& mapping, const char* name,
                                                   std::int32_t parent_id) {
    if (name == nullptr) {
        return nullptr;
    }
    for (const auto& field : mapping.fields) {
        if (field.parent_id == parent_id && field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

const LanceField* find_mapping_field(const LanceSchemaMapping& mapping, std::int32_t id) {
    for (const auto& field : mapping.fields) {
        if (field.id == id) {
            return &field;
        }
    }
    return nullptr;
}

const pb::Field* find_descriptor_field(const pb::FileDescriptor& descriptor, const std::int32_t id) {
    for (const auto& field : descriptor.fields) {
        if (field.id == id) {
            return &field;
        }
    }
    return nullptr;
}

bool set_schema_metadata(ArrowSchema& schema, const std::string& key, const std::string& value) {
    ArrowBuffer buffer;
    if (ArrowMetadataBuilderInit(&buffer, schema.metadata) != NANOARROW_OK) {
        return false;
    }
    if (ArrowMetadataBuilderSet(&buffer, ArrowCharView(key.c_str()), ArrowCharView(value.c_str())) != NANOARROW_OK) {
        ArrowBufferReset(&buffer);
        return false;
    }
    if (ArrowSchemaSetMetadata(&schema, reinterpret_cast<const char*>(buffer.data)) != NANOARROW_OK) {
        ArrowBufferReset(&buffer);
        return false;
    }
    ArrowBufferReset(&buffer);
    return true;
}

bool init_schema_from_field(const LanceField& field, const LanceSchemaMapping& mapping, ArrowSchema& schema,
                            std::string& error) {
    ArrowSchemaInit(&schema);
    std::uint64_t fsl_items = 0;
    if (field.logical_type == "struct") {
        std::vector<const LanceField*> children;
        for (const auto& candidate : mapping.fields) {
            if (candidate.parent_id == field.id) {
                children.push_back(&candidate);
            }
        }
        if (children.empty()) {
            error = "struct field has no children in mapping";
            return false;
        }
        if (ArrowSchemaAllocateChildren(&schema, static_cast<int64_t>(children.size())) != NANOARROW_OK) {
            error = "failed to allocate struct children";
            return false;
        }
        if (ArrowSchemaSetType(&schema, NANOARROW_TYPE_STRUCT) != NANOARROW_OK) {
            error = "failed to set struct type";
            return false;
        }
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (!init_schema_from_field(*children[i], mapping, *schema.children[i], error)) {
                return false;
            }
        }
    } else if (std::string element; lance_fixed_size_list_parts(field.logical_type, element, fsl_items)) {
        // Lance's schema has no child field for a fixed_size_list -- the element type lives in the
        // logical type string -- so the Arrow child is made up here, named "item" as Arrow and
        // pylance name it.
        std::string element_format;
        if (fsl_items > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
            !lance_arrow_format_for_logical_type(element, element_format, error) ||
            ArrowSchemaSetTypeFixedSize(&schema, NANOARROW_TYPE_FIXED_SIZE_LIST, static_cast<std::int32_t>(fsl_items)) !=
                NANOARROW_OK ||
            ArrowSchemaSetFormat(schema.children[0], element_format.c_str()) != NANOARROW_OK) {
            if (error.empty()) {
                error = "failed to build the fixed_size_list schema for " + field.name;
            }
            return false;
        }
    } else {
        if (ArrowSchemaSetFormat(&schema, field.arrow_format.c_str()) != NANOARROW_OK) {
            error = "failed to set schema format for " + field.name;
            return false;
        }
    }
    if (ArrowSchemaSetName(&schema, field.name.c_str()) != NANOARROW_OK) {
        error = "failed to set schema field name";
        return false;
    }
    for (const auto& kv : field.metadata) {
        if (!set_schema_metadata(schema, kv.first, kv.second)) {
            error = "failed to set schema metadata";
            return false;
        }
    }
    return true;
}

bool build_schema_from_mapping(const LanceSchemaMapping& mapping, ArrowSchema& schema, std::string& error) {
    std::vector<const LanceField*> roots;
    for (const auto& field : mapping.fields) {
        if (field.parent_id == -1) {
            roots.push_back(&field);
        }
    }
    if (roots.empty()) {
        error = "schema mapping has no root fields";
        return false;
    }
    if (roots.size() == 1U) {
        ArrowSchemaInit(&schema);
        if (ArrowSchemaAllocateChildren(&schema, 1) != NANOARROW_OK) {
            error = "failed to allocate root struct children";
            return false;
        }
        if (ArrowSchemaSetType(&schema, NANOARROW_TYPE_STRUCT) != NANOARROW_OK) {
            error = "failed to set root struct type";
            return false;
        }
        return init_schema_from_field(*roots[0], mapping, *schema.children[0], error);
    }
    ArrowSchemaInit(&schema);
    if (ArrowSchemaAllocateChildren(&schema, static_cast<int64_t>(roots.size())) != NANOARROW_OK) {
        error = "failed to allocate root struct children";
        return false;
    }
    if (ArrowSchemaSetType(&schema, NANOARROW_TYPE_STRUCT) != NANOARROW_OK) {
        error = "failed to set root struct type";
        return false;
    }
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (!init_schema_from_field(*roots[i], mapping, *schema.children[i], error)) {
            return false;
        }
    }
    return true;
}

bool append_fixed_raw(ArrowArray& array, const std::uint8_t* data, const std::size_t bytes_per_value,
                      const std::string& arrow_format, std::string& error) {
    const auto append_int = [&](std::int64_t v) { return ArrowArrayAppendInt(&array, v) == NANOARROW_OK; };
    const auto append_uint = [&](std::uint64_t v) {
        return ArrowArrayAppendUInt(&array, static_cast<std::int64_t>(v)) == NANOARROW_OK;
    };
    bool ok = false;
    if (arrow_format == "c") {
        ok = append_int(static_cast<std::int8_t>(data[0]));
    } else if (arrow_format == "C") {
        ok = append_uint(data[0]);
    } else if (arrow_format == "b") {
        ok = append_int(data[0] != 0 ? 1 : 0);
    } else if (arrow_format == "s") {
        std::int16_t v = 0;
        std::memcpy(&v, data, 2);
        ok = append_int(v);
    } else if (arrow_format == "S") {
        std::uint16_t v = 0;
        std::memcpy(&v, data, 2);
        ok = append_uint(v);
    } else if (arrow_format == "i") {
        std::int32_t v = 0;
        std::memcpy(&v, data, 4);
        ok = append_int(v);
    } else if (arrow_format == "I") {
        std::uint32_t v = 0;
        std::memcpy(&v, data, 4);
        ok = append_uint(v);
    } else if (arrow_format == "l") {
        std::int64_t v = 0;
        std::memcpy(&v, data, 8);
        ok = append_int(v);
    } else if (arrow_format == "L") {
        std::uint64_t v = 0;
        std::memcpy(&v, data, 8);
        ok = append_uint(v);
    } else if (arrow_format == "f") {
        float v = 0;
        std::memcpy(&v, data, 4);
        ok = ArrowArrayAppendDouble(&array, static_cast<double>(v)) == NANOARROW_OK;
    } else if (arrow_format == "g") {
        double v = 0;
        std::memcpy(&v, data, 8);
        ok = ArrowArrayAppendDouble(&array, v) == NANOARROW_OK;
    } else if (arrow_format.rfind("w:", 0) == 0) {
        ArrowBufferView view{};
        view.data.data = data;
        view.size_bytes = static_cast<int64_t>(bytes_per_value);
        ok = ArrowArrayAppendBytes(&array, view) == NANOARROW_OK;
    } else {
        error = "unsupported fixed arrow format: " + arrow_format;
        return false;
    }
    if (!ok) {
        error = "failed to append value of arrow format " + arrow_format;
        return false;
    }
    return true;
}

bool append_one_string(ArrowArray& array, std::string_view value, std::string& error) {
    if (ArrowArrayAppendString(&array, {value.data(), static_cast<int64_t>(value.size())}) != NANOARROW_OK) {
        error = "failed to append string value";
        return false;
    }
    return true;
}

bool append_string_at_row(ArrowArray& array, const VariableWidthColumnValues& column, const std::size_t row,
                          std::string& error) {
    const bool large = column.large;
    const auto offset_width = large ? 8U : 4U;
    if (column.offsets.size() < (row + 2U) * offset_width) {
        error = "string row offset out of range";
        return false;
    }
    std::int64_t start = 0;
    std::int64_t end = 0;
    const auto* off = column.offsets.data() + row * offset_width;
    if (large) {
        std::memcpy(&start, off, sizeof(start));
        std::memcpy(&end, off + 8U, sizeof(end));
    } else {
        std::int32_t s = 0;
        std::int32_t e = 0;
        std::memcpy(&s, off, sizeof(s));
        std::memcpy(&e, off + 4U, sizeof(e));
        start = s;
        end = e;
    }
    if (start < 0 || end < start || static_cast<std::size_t>(end) > column.data.size()) {
        error = "string bounds invalid";
        return false;
    }
    return append_one_string(array,
                             {reinterpret_cast<const char*>(column.data.data() + static_cast<std::size_t>(start)),
                              static_cast<std::size_t>(end - start)},
                             error);
}

bool append_null_binary(ArrowArray& array, std::string& error) {
    if (ArrowArrayAppendNull(&array, 1) != NANOARROW_OK) {
        error = "failed to append null binary";
        return false;
    }
    return true;
}

bool append_uint64_value(ArrowArray& array, const std::uint64_t value, std::string& error) {
    if (ArrowArrayAppendUInt(&array, value) != NANOARROW_OK) {
        error = "failed to append uint64";
        return false;
    }
    return true;
}

bool append_blob_v2_row(ArrowArray& struct_array, const std::vector<std::uint8_t>& row_bytes,
                        const std::vector<std::string>* uri_dictionary, std::string& error) {
    BlobV2ExternalDescriptor descriptor{};
    if (!blob_v2_unpack_descriptor_row(row_bytes, descriptor, error)) {
        return false;
    }
    // Dictionary-encoded rows carry an empty inline URI and a blob_id index into the dictionary.
    if (uri_dictionary != nullptr && !uri_dictionary->empty() && descriptor.blob_uri.empty()) {
        if (descriptor.blob_id >= uri_dictionary->size()) {
            error = "blob_id out of range for URI dictionary";
            return false;
        }
        descriptor.blob_uri = (*uri_dictionary)[descriptor.blob_id];
    }
    auto* data = struct_array.children[0];
    auto* uri = struct_array.children[1];
    auto* position = struct_array.children[2];
    auto* size_field = struct_array.children[3];
    if (!append_null_binary(*data, error) || !append_one_string(*uri, descriptor.blob_uri, error) ||
        !append_uint64_value(*position, descriptor.position, error) ||
        !append_uint64_value(*size_field, descriptor.size, error)) {
        return false;
    }
    if (ArrowArrayFinishElement(&struct_array) != NANOARROW_OK) {
        error = "failed to finish blob struct element";
        return false;
    }
    return true;
}

bool append_column_value_at_row(const LanceField& field, const ColumnValues& values, const std::int64_t row,
                                const std::vector<std::string>* uri_dictionary, ArrowArray& array,
                                std::string& error) {
    if (field.extension_name == "lance.blob.v2") {
        if (values.kind != ColumnValues::Kind::BlobV2External) {
            error = "expected blob v2 packed values for " + field.name;
            return false;
        }
        std::size_t offset = 0;
        for (std::int64_t i = 0; i < row; ++i) {
            if (static_cast<std::size_t>(i) >= values.blob_v2.row_packed_sizes.size()) {
                error = "blob row index out of range";
                return false;
            }
            offset += values.blob_v2.row_packed_sizes[static_cast<std::size_t>(i)];
        }
        if (static_cast<std::size_t>(row) >= values.blob_v2.row_packed_sizes.size()) {
            error = "blob row index out of range";
            return false;
        }
        const auto row_size = values.blob_v2.row_packed_sizes[static_cast<std::size_t>(row)];
        if (offset + row_size > values.blob_v2.packed_payload.size()) {
            error = "blob packed row out of range";
            return false;
        }
        const std::vector<std::uint8_t> row_bytes(
            values.blob_v2.packed_payload.begin() + static_cast<std::ptrdiff_t>(offset),
            values.blob_v2.packed_payload.begin() + static_cast<std::ptrdiff_t>(offset + row_size));
        return append_blob_v2_row(array, row_bytes, uri_dictionary, error);
    }
    if (lance_field_is_variable_width(field.logical_type)) {
        return append_string_at_row(array, values.variable, static_cast<std::size_t>(row), error);
    }
    const auto width = lance_logical_type_value_bytes(field.logical_type);
    if (values.fixed.size() < (static_cast<std::size_t>(row) + 1U) * width) {
        error = "fixed column too short for row";
        return false;
    }
    return append_fixed_raw(array, values.fixed.data() + static_cast<std::size_t>(row) * width, width,
                            field.arrow_format, error);
}

// Fixed-width arrow format codes, resolved once per column to avoid per-row string comparisons.
// Covers every fixed-width type the writer can emit (the write/read parity principle): signed/unsigned
// ints 8..64, float/double, bool, and fixed-size-binary (width carried in ColumnPlan::width).
enum class FixedFmt { kI8, kU8, kI16, kU16, kI32, kU32, kI64, kU64, kF32, kF64, kBool, kFixedBinary, kUnsupported };

FixedFmt fixed_fmt_code(const std::string& arrow_format) {
    if (arrow_format == "c") return FixedFmt::kI8;
    if (arrow_format == "C") return FixedFmt::kU8;
    if (arrow_format == "s") return FixedFmt::kI16;
    if (arrow_format == "S") return FixedFmt::kU16;
    if (arrow_format == "i") return FixedFmt::kI32;
    if (arrow_format == "I") return FixedFmt::kU32;
    if (arrow_format == "l") return FixedFmt::kI64;
    if (arrow_format == "L") return FixedFmt::kU64;
    if (arrow_format == "f") return FixedFmt::kF32;
    if (arrow_format == "g") return FixedFmt::kF64;
    if (arrow_format == "b") return FixedFmt::kBool;
    if (arrow_format.rfind("w:", 0) == 0) return FixedFmt::kFixedBinary;
    return FixedFmt::kUnsupported;
}

bool append_fixed_fast(ArrowArray& array, const std::uint8_t* data, FixedFmt fmt, std::size_t width,
                       std::string& error) {
    switch (fmt) {
        case FixedFmt::kI8:
            return ArrowArrayAppendInt(&array, static_cast<std::int8_t>(data[0])) == NANOARROW_OK;
        case FixedFmt::kU8:
            return ArrowArrayAppendUInt(&array, data[0]) == NANOARROW_OK;
        case FixedFmt::kBool:
            return ArrowArrayAppendInt(&array, data[0] != 0 ? 1 : 0) == NANOARROW_OK;
        case FixedFmt::kI16: {
            std::int16_t v = 0;
            std::memcpy(&v, data, 2);
            return ArrowArrayAppendInt(&array, v) == NANOARROW_OK;
        }
        case FixedFmt::kU16: {
            std::uint16_t v = 0;
            std::memcpy(&v, data, 2);
            return ArrowArrayAppendUInt(&array, v) == NANOARROW_OK;
        }
        case FixedFmt::kI32: {
            std::int32_t v = 0;
            std::memcpy(&v, data, 4);
            return ArrowArrayAppendInt(&array, v) == NANOARROW_OK;
        }
        case FixedFmt::kU32: {
            std::uint32_t v = 0;
            std::memcpy(&v, data, 4);
            return ArrowArrayAppendUInt(&array, v) == NANOARROW_OK;
        }
        case FixedFmt::kI64: {
            std::int64_t v = 0;
            std::memcpy(&v, data, 8);
            return ArrowArrayAppendInt(&array, v) == NANOARROW_OK;
        }
        case FixedFmt::kU64: {
            std::uint64_t v = 0;
            std::memcpy(&v, data, 8);
            return ArrowArrayAppendUInt(&array, static_cast<std::int64_t>(v)) == NANOARROW_OK;
        }
        case FixedFmt::kF32: {
            float v = 0;
            std::memcpy(&v, data, 4);
            return ArrowArrayAppendDouble(&array, static_cast<double>(v)) == NANOARROW_OK;
        }
        case FixedFmt::kF64: {
            double v = 0;
            std::memcpy(&v, data, 8);
            return ArrowArrayAppendDouble(&array, v) == NANOARROW_OK;
        }
        case FixedFmt::kFixedBinary: {
            ArrowBufferView view{};
            view.data.data = data;
            view.size_bytes = static_cast<int64_t>(width);
            return ArrowArrayAppendBytes(&array, view) == NANOARROW_OK;
        }
        default:
            error = "unsupported fixed arrow format in fast path";
            return false;
    }
}

// One column's decode plan, resolved once before the row loop (no per-row metadata/string work).
struct ColumnPlan {
    enum class Kind { Skip, Blob, Variable, Fixed, FixedSizeList } kind = Kind::Skip;
    ArrowArray* array = nullptr;
    const LanceField* field = nullptr;      // Blob path needs the full field
    ColumnValues* values = nullptr;
    const std::vector<std::string>* dict = nullptr;
    std::size_t width = 0;                   // Fixed
    FixedFmt fmt = FixedFmt::kUnsupported;   // Fixed
};

// Bulk-fill a fixed-width child array's data buffer from the already-decoded column bytes.
/// Attach a decoded validity bitmap to `child`. Arrow buffer 0 is the validity bitmap in exactly the
/// layout the decoder produces (LSB-first, bit set == valid), so this is a straight copy.
/// Deallocator for a buffer whose memory is owned by a heap `std::vector<uint8_t>`.
/// nanoarrow hands `allocator->private_data` straight back to us; it is the vector itself.
void release_adopted_vector(struct ArrowBufferAllocator* allocator, std::uint8_t* /*ptr*/,
                            std::int64_t /*size*/) {
    delete static_cast<std::vector<std::uint8_t>*>(allocator->private_data);
}

/// Hand a decoded vector's memory to `out` WITHOUT copying it.
///
/// This is the whole of plan item 4.1. The decoder produces each column into a std::vector, and every
/// fill_* helper below used to ArrowBufferAppend it into the Arrow buffer -- so the decoded column and
/// its Arrow copy were both live, and a single-fragment read peaked at 2.01x the dataset (measured; see
/// docs/PROGRESS.md). Adopting the vector's storage instead makes that a move.
///
/// nanoarrow documents ArrowBufferDeallocator for exactly this ("avoid copying an existing buffer that
/// was not allocated using the infrastructure provided here"). The vector moves to the heap and the
/// ArrowBuffer owns it from here on.
///
/// An EMPTY vector is deliberately not adopted: ArrowBufferReset only calls the deallocator when
/// `data != NULL`, so a zero-length vector would leak the heap object it was moved into. An empty
/// Arrow buffer is already the correct representation, so there is nothing to do.
bool adopt_into_buffer(std::vector<std::uint8_t>&& bytes, ArrowBuffer* out, std::string& error) {
    if (bytes.empty()) {
        return true;
    }
    auto* owned = new std::vector<std::uint8_t>(std::move(bytes));
    if (ArrowBufferSetAllocator(out, ArrowBufferDeallocator(&release_adopted_vector, owned)) !=
        NANOARROW_OK) {
        // Only possible if the buffer already holds data, which would mean this column was filled
        // twice. Refuse rather than leak or double-own.
        delete owned;
        error = "cannot adopt a decoded buffer into an Arrow buffer that already holds data";
        return false;
    }
    out->data = owned->data();
    out->size_bytes = static_cast<std::int64_t>(owned->size());
    out->capacity_bytes = static_cast<std::int64_t>(owned->capacity());
    return true;
}

bool fill_validity(ArrowArray* child, ColumnValues& values, std::int64_t rows, std::string& error) {
    if (values.validity.empty()) {
        child->null_count = 0;
        return true;
    }
    const auto expected = static_cast<std::size_t>((rows + 7) / 8);
    if (values.validity.size() < expected) {
        error = "validity bitmap covers fewer rows than the column has";
        return false;
    }
    // The decoder sizes the bitmap to whole bytes over the rows it appended, which can exceed
    // `expected` only by trailing padding bits Arrow ignores. Trim before adopting so the buffer's
    // length is exactly what Arrow expects.
    values.validity.resize(expected);
    if (!adopt_into_buffer(std::move(values.validity), ArrowArrayBuffer(child, 0), error)) {
        return false;
    }
    child->null_count = static_cast<std::int64_t>(values.null_count);
    return true;
}

bool fill_fixed_child(ArrowArray* child, std::vector<std::uint8_t>&& bytes, std::int64_t rows,
                      FixedFmt fmt, std::string& error) {
    ArrowBuffer* data = ArrowArrayBuffer(child, 1);
    // bool is the one fixed type that cannot be adopted: it is a byte per value on disk and a BIT per
    // value in the Arrow buffer, so the bits have to be packed somewhere. They are packed into a fresh
    // vector, which is then adopted -- so this path still copies once (unavoidably) rather than twice.
    if (fmt == FixedFmt::kBool) {
        const auto packed_bytes = static_cast<std::size_t>((rows + 7) / 8);
        std::vector<std::uint8_t> packed(packed_bytes, 0U);
        for (std::int64_t i = 0; i < rows; ++i) {
            if (bytes[static_cast<std::size_t>(i)] != 0U) {
                packed[static_cast<std::size_t>(i) >> 3U] |=
                    static_cast<std::uint8_t>(1U << (static_cast<std::size_t>(i) & 7U));
            }
        }
        if (!adopt_into_buffer(std::move(packed), data, error)) {
            return false;
        }
        child->length = rows;
        return true;
    }
    if (!adopt_into_buffer(std::move(bytes), data, error)) {
        return false;
    }
    child->length = rows;
    return true;
}

// Bulk-fill a variable-width child array's offsets+data buffers from the decoded column.
bool fill_variable_child(ArrowArray* child, VariableWidthColumnValues& v, std::int64_t rows,
                         std::string& error) {
    if (!adopt_into_buffer(std::move(v.offsets), ArrowArrayBuffer(child, 1), error) ||
        !adopt_into_buffer(std::move(v.data), ArrowArrayBuffer(child, 2), error)) {
        return false;
    }
    child->length = rows;
    return true;
}

bool build_batch_from_schema(const ArrowSchema& batch_schema, const LanceSchemaMapping& mapping,
                             std::unordered_map<std::int32_t, ColumnValues>& decoded_by_field_id,
                             const std::int64_t length, ArrowArray& batch, std::string& error) {
    if (ArrowArrayInitFromSchema(&batch, &batch_schema, nullptr) != NANOARROW_OK) {
        error = "failed to init batch array from schema";
        return false;
    }
    // NB: ArrowArrayStartAppending is only for the per-row fallback below; the bulk path fills buffers
    // directly and must NOT call it (StartAppending pre-seeds the leading 0 offset on variable arrays).
    if (batch_schema.n_children <= 0) {
        error = "batch schema has no children";
        ArrowArrayRelease(&batch);
        return false;
    }

    // Precompute URI dictionaries for any dictionary-encoded blob columns (nanolance extension).
    std::unordered_map<std::int32_t, std::vector<std::string>> blob_uri_dicts;
    for (const auto& f : mapping.fields) {
        if (f.extension_name == "lance.blob.v2") {
            const auto it = f.metadata.find(kBlobV2UriDictMetadataKey);
            if (it != f.metadata.end()) {
                blob_uri_dicts.emplace(f.id, blob_v2_parse_uri_dictionary(it->second));
            }
        }
    }
    auto dict_for = [&](std::int32_t id) -> const std::vector<std::string>* {
        const auto it = blob_uri_dicts.find(id);
        return it == blob_uri_dicts.end() ? nullptr : &it->second;
    };

    // Resolve every column's decode plan ONCE (field lookup, kind, width, format, dict) so the row
    // loop does zero per-row metadata/string work — this was ~27% of read instructions (callgrind).
    //
    // `plans` is flat over LEAF columns wherever they sit in the schema. A plain struct column
    // (anything but the lance.blob.v2 extension, which has its own packed representation) is not a
    // leaf: its data lives in its children's columns, exactly as the writer laid it out, so it
    // contributes its descendants here and is recorded in `struct_nodes` for its length to be set
    // once the leaves are filled. The writer already produces a correct, stock-Lance-readable file
    // for these -- pylance reads a nanolance struct column fine -- so refusing them here (the old
    // "nested struct children are not supported in this reader build") made nanolance unable to read
    // back a file it had just written correctly, for a feature README.md advertises.
    std::vector<ColumnPlan> plans;
    std::vector<ArrowArray*> struct_nodes;
    plans.reserve(static_cast<std::size_t>(batch_schema.n_children));

    std::string collect_error;
    const auto collect = [&](auto&& self, const ArrowSchema* node_schema, ArrowArray* node_array,
                             std::int32_t parent_id) -> bool {
        if (node_schema == nullptr || node_schema->name == nullptr || node_array == nullptr) {
            collect_error = "batch schema child is missing";
            return false;
        }
        const auto* field = find_mapping_field_by_name_under(mapping, node_schema->name, parent_id);
        if (field == nullptr) {
            collect_error = "mapping field not found for schema child ";
            collect_error += node_schema->name;
            return false;
        }
        const bool is_blob = field->extension_name == "lance.blob.v2";
        std::string fsl_element;
        std::uint64_t fsl_items = 0;
        const bool is_fsl = lance_fixed_size_list_parts(field->logical_type, fsl_element, fsl_items);
        const bool is_struct =
            !is_blob && !is_fsl && node_schema->format != nullptr && node_schema->format[0] == '+';

        if (is_struct) {
            struct_nodes.push_back(node_array);
            for (std::int64_t i = 0; i < node_schema->n_children; ++i) {
                if (node_array->children == nullptr || i >= node_array->n_children) {
                    collect_error = "struct array is missing children for ";
                    collect_error += field->name;
                    return false;
                }
                if (!self(self, node_schema->children[i], node_array->children[i], field->id)) {
                    return false;
                }
            }
            return true;
        }

        ColumnPlan plan;
        plan.array = node_array;
        plan.field = field;
        if (!is_blob && field->column_index < 0) {
            plan.kind = ColumnPlan::Kind::Skip;
            plans.push_back(plan);
            return true;
        }
        const auto col_it = decoded_by_field_id.find(field->id);
        if (col_it == decoded_by_field_id.end()) {
            collect_error = "missing decoded column for ";
            collect_error += field->name;
            return false;
        }
        plan.values = &col_it->second;
        if (is_blob) {
            plan.kind = ColumnPlan::Kind::Blob;
            plan.dict = dict_for(field->id);
        } else if (is_fsl) {
            // One physical column of N-element rows. The decoded bytes ARE the child's values buffer:
            // N elements per row, back to back.
            plan.kind = ColumnPlan::Kind::FixedSizeList;
            plan.width = static_cast<std::size_t>(fsl_items);
            plan.fmt = fixed_fmt_code(node_schema->children != nullptr && node_schema->n_children == 1
                                          ? node_schema->children[0]->format
                                          : "");
        } else if (lance_field_is_variable_width(field->logical_type)) {
            plan.kind = ColumnPlan::Kind::Variable;
        } else {
            plan.kind = ColumnPlan::Kind::Fixed;
            plan.width = lance_logical_type_value_bytes(field->logical_type);
            plan.fmt = fixed_fmt_code(field->arrow_format);
        }
        plans.push_back(plan);
        return true;
    };

    for (int64_t c = 0; c < batch_schema.n_children; ++c) {
        if (!collect(collect, batch_schema.children[c], batch.children[c], -1)) {
            error = collect_error;
            ArrowArrayRelease(&batch);
            return false;
        }
    }

    // Fast path: when every column is a plain fixed/variable leaf (no blob struct, no skipped logical
    // field), build each child's Arrow buffers in one bulk copy from the decoded column and skip the
    // per-row append/FinishElement entirely.
    bool bulk_ok = true;
    for (const auto& plan : plans) {
        if (plan.kind != ColumnPlan::Kind::Fixed && plan.kind != ColumnPlan::Kind::Variable &&
            plan.kind != ColumnPlan::Kind::FixedSizeList) {
            bulk_ok = false;
            break;
        }
    }
    if (bulk_ok) {
        for (auto& plan : plans) {
            // Validity first: nanoarrow expects buffer 0 filled before the data buffers it sizes
            // against, and both fill_* helpers set child->length/null_count at the end.
            bool ok = fill_validity(plan.array, *plan.values, length, error);
            if (ok && plan.kind == ColumnPlan::Kind::FixedSizeList) {
                // Row-level validity sits on the list; the child carries every row's N elements,
                // including a null row's, which is what Arrow's fixed_size_list layout requires.
                ArrowArray* child = plan.array->n_children == 1 ? plan.array->children[0] : nullptr;
                std::uint64_t child_length = 0;
                if (child == nullptr ||
                    !checked_mul(static_cast<std::uint64_t>(length), static_cast<std::uint64_t>(plan.width),
                                 child_length) ||
                    child_length > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    error = "fixed_size_list column has no child array or too many elements";
                    ok = false;
                } else {
                    child->null_count = 0;
                    if (!plan.values->item_validity.empty()) {
                        auto& bits = plan.values->item_validity;
                        const auto expected = static_cast<std::size_t>((child_length + 7U) / 8U);
                        if (bits.size() < expected) {
                            error = "fixed_size_list element validity covers fewer elements than the column has";
                            ok = false;
                        } else {
                            bits.resize(expected);
                            ok = adopt_into_buffer(std::move(bits), ArrowArrayBuffer(child, 0), error);
                            child->null_count = static_cast<std::int64_t>(plan.values->item_null_count);
                        }
                    }
                    ok = ok && fill_fixed_child(child, std::move(plan.values->fixed),
                                                static_cast<std::int64_t>(child_length), plan.fmt, error);
                    plan.array->length = length;
                }
            } else if (ok) {
                ok = plan.kind == ColumnPlan::Kind::Fixed
                         ? fill_fixed_child(plan.array, std::move(plan.values->fixed), length, plan.fmt, error)
                         : fill_variable_child(plan.array, plan.values->variable, length, error);
            }
            if (!ok) {
                ArrowArrayRelease(&batch);
                return false;
            }
            plan.array->null_count = static_cast<std::int64_t>(plan.values->null_count);
        }
        // A struct node owns no buffers of its own -- its leaves were just filled above -- but Arrow
        // still needs its length, and validation checks it against every child's.
        for (auto* node : struct_nodes) {
            node->length = length;
            node->null_count = 0;
        }
        batch.length = length;
        batch.null_count = 0;
        ArrowError arrow_error;
        if (ArrowArrayFinishBuildingDefault(&batch, &arrow_error) != NANOARROW_OK) {
            error = "failed to finish bulk-built batch: ";
            error += arrow_error.message;
            ArrowArrayRelease(&batch);
            return false;
        }
        return true;
    }

    // Per-row fallback (blob columns or skipped logical fields): needs the append machinery, which
    // drives nesting through ArrowArrayFinishElement on the ROOT only. A plain struct column mixed
    // into such a batch would therefore have its own FinishElement skipped, so refuse that
    // combination explicitly rather than emit a subtly malformed array. Struct-only batches take the
    // bulk path above and are fine.
    const bool has_fsl = std::any_of(plans.begin(), plans.end(), [](const ColumnPlan& plan) {
        return plan.kind == ColumnPlan::Kind::FixedSizeList;
    });
    if (has_fsl) {
        error =
            "a fixed_size_list column cannot be read back in the same batch as a lance.blob.v2 column "
            "or a skipped logical field (the per-row append path does not build nested arrays)";
        ArrowArrayRelease(&batch);
        return false;
    }
    if (!struct_nodes.empty()) {
        error =
            "a plain struct column cannot be read back in the same batch as a lance.blob.v2 column "
            "or a skipped logical field (the per-row append path drives nesting from the root only)";
        ArrowArrayRelease(&batch);
        return false;
    }
    if (ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
        error = "failed to start batch append";
        ArrowArrayRelease(&batch);
        return false;
    }
    for (std::int64_t row = 0; row < length; ++row) {
        for (auto& plan : plans) {
            switch (plan.kind) {
                case ColumnPlan::Kind::Skip:
                case ColumnPlan::Kind::FixedSizeList:  // refused above: this path builds no nested arrays
                    break;
                case ColumnPlan::Kind::Variable:
                    if (!append_string_at_row(*plan.array, plan.values->variable, static_cast<std::size_t>(row),
                                              error)) {
                        ArrowArrayRelease(&batch);
                        return false;
                    }
                    break;
                case ColumnPlan::Kind::Fixed:
                    if (plan.values->fixed.size() < (static_cast<std::size_t>(row) + 1U) * plan.width ||
                        !append_fixed_fast(*plan.array,
                                           plan.values->fixed.data() + static_cast<std::size_t>(row) * plan.width,
                                           plan.fmt, plan.width, error)) {
                        error += " (fixed column decode)";
                        ArrowArrayRelease(&batch);
                        return false;
                    }
                    break;
                case ColumnPlan::Kind::Blob:
                    if (!append_column_value_at_row(*plan.field, *plan.values, row, plan.dict, *plan.array, error)) {
                        ArrowArrayRelease(&batch);
                        return false;
                    }
                    break;
            }
        }
        if (ArrowArrayFinishElement(&batch) != NANOARROW_OK) {
            error = "failed to finish batch struct row";
            ArrowArrayRelease(&batch);
            return false;
        }
    }
    if (ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
        error = "failed to finish building batch";
        ArrowArrayRelease(&batch);
        return false;
    }
    batch.length = length;
    return true;
}

/// One fragment, and which of its rows this read wants.
///
/// `skip`/`take` are how a row range reaches the decoder. A fragment the range does not touch at all
/// is never put in the plan, so it is never opened -- that, not the trimming, is where the saving is.
///
/// A fragment can hold SEVERAL data files, and they are not more rows: they are more COLUMNS of the
/// same rows. `add_columns` produces exactly that -- the computed column lands in a second file
/// beside the original. Treating each file as its own batch (which this used to do) silently dropped
/// every column after the first file's.
struct PlannedFile {
    std::vector<pb::DataFile> files;
    std::uint64_t fragment_id = 0;
    pb::DeletionFile deletion_file;    // `present` false when the fragment has no deletions
    std::uint64_t physical_rows = 0;   // rows on disk, before deletions
    std::uint64_t rows = 0;            // LOGICAL rows: physical minus deleted. What a read returns.
    std::uint64_t skip = 0;            // logical rows to drop from the front
    std::uint64_t take = 0;            // logical rows to keep

    bool partial() const { return skip != 0U || take != rows; }
};

bool read_data_file_batch(const std::filesystem::path& dataset_path, const PlannedFile& planned,
                          const LanceSchemaMapping& mapping, const ArrowSchema& batch_schema, ArrowArray& batch,
                          std::string& error,
                          const std::unordered_set<std::int32_t>* allowed_field_ids = nullptr) {
    if (planned.files.empty()) {
        error = "fragment has no data files";
        return false;
    }

    // One batch is one read operation, so each data file it touches is validated once here rather
    // than once per page buffer (see DataFileReadScope).
    const DataFileReadScope read_scope;

    // One fragment, one batch -- however many files its columns are split across.
    std::unordered_map<std::int32_t, ColumnValues> decoded_by_field_id;
    std::int64_t length = -1;
    for (const auto& data_file : planned.files) {
        // data_file.path is attacker-controlled (it comes out of the untrusted manifest). Confine it
        // under <dataset>/data/ so a hostile ".."/absolute path can't make the reader open a file
        // outside the dataset. The writer only ever stores a bare filename here, so legitimate
        // datasets are unaffected.
        const auto jailed = safe_join_under(dataset_path / "data", data_file.path);
        if (!jailed) {
            error = "data file path escapes the dataset directory";
            return false;
        }
        const auto& path = *jailed;
        pb::FileDescriptor descriptor{};
        LanceDataFileFooterLayout layout{};
        if (!read_lance_data_file_footer_and_descriptor(path, descriptor, layout, error)) {
            return false;
        }
        std::vector<pb::ColumnMetadata> column_metadatas;
        if (!read_lance_data_file_column_metadatas(path, layout, column_metadatas, error)) {
            return false;
        }
        if (data_file.fields.size() != data_file.column_indices.size()) {
            error = "data file field/column index mismatch";
            return false;
        }
        // Every file of a fragment describes the SAME rows. A disagreement means the manifest and the
        // files are out of step, and merging them would silently pad or truncate a column.
        if (length < 0) {
            length = static_cast<std::int64_t>(descriptor.length);
        } else if (static_cast<std::uint64_t>(length) != descriptor.length) {
            error = "data files within one fragment disagree on their row count";
            return false;
        }

        for (std::size_t i = 0; i < data_file.fields.size(); ++i) {
            const auto field_id = data_file.fields[i];
            // Skip columns not in the projection (if one is set).
            if (allowed_field_ids && !allowed_field_ids->count(field_id)) continue;

            const auto column_index = data_file.column_indices[i];
            if (column_index < 0 ||
                static_cast<std::size_t>(column_index) >= column_metadatas.size()) {
                error = "data file column index out of range";
                return false;
            }
            const auto* on_disk = find_descriptor_field(descriptor, field_id);
            if (on_disk == nullptr) {
                error = "data file references unknown field id";
                return false;
            }
            ColumnValues values;
            if (!decode_lance_physical_column(path, *on_disk,
                                              column_metadatas[static_cast<std::size_t>(column_index)],
                                              values, error)) {
                return false;
            }
            decoded_by_field_id.emplace(field_id, std::move(values));
        }
    }

    // Deletions first, then the row range: a range is expressed in LOGICAL row numbers, which only
    // exist once the deleted rows are gone.
    if (planned.deletion_file.present) {
        if (static_cast<std::uint64_t>(length) != planned.physical_rows) {
            error = "data file holds " + std::to_string(length) +
                    " rows but the manifest claims " + std::to_string(planned.physical_rows) +
                    "; refusing to apply a deletion vector against rows that do not line up";
            return false;
        }
        std::vector<std::uint32_t> deleted;
        if (!read_deletion_vector(dataset_path, planned.fragment_id, planned.deletion_file, deleted, error)) {
            return false;
        }
        std::vector<std::uint8_t> keep(static_cast<std::size_t>(length), 1U);
        for (const auto row : deleted) {
            if (row >= static_cast<std::uint64_t>(length)) {
                error = "deletion file names row " + std::to_string(row) + " but the fragment holds " +
                        std::to_string(length);
                return false;
            }
            keep[row] = 0U;
        }
        for (auto& [field_id, values] : decoded_by_field_id) {
            const auto* field = find_mapping_field(mapping, field_id);
            const std::size_t value_bytes =
                field == nullptr ? 0U : lance_logical_type_value_bytes(field->logical_type);
            if (!compact_column_values(values, keep, static_cast<std::uint64_t>(length), value_bytes,
                                       error)) {
                return false;
            }
        }
        length = static_cast<std::int64_t>(planned.rows);
    }

    if (planned.partial()) {
        // The plan's skip/take came from the MANIFEST's per-fragment row count, while the rows are
        // here in the data file. If the two disagree, the arithmetic that decided which files to skip
        // was wrong, and a silently misaligned row range is exactly the failure this must not have.
        if (static_cast<std::uint64_t>(length) != planned.rows) {
            error = "data file holds " + std::to_string(length) +
                    " rows but the manifest claims " + std::to_string(planned.rows) +
                    "; refusing to guess which rows a range covers";
            return false;
        }
        for (auto& [field_id, values] : decoded_by_field_id) {
            const auto* field = find_mapping_field(mapping, field_id);
            const std::size_t value_bytes =
                field == nullptr ? 0U : lance_logical_type_value_bytes(field->logical_type);
            if (!slice_column_values(values, planned.skip, planned.take,
                                     static_cast<std::uint64_t>(length), value_bytes, error)) {
                return false;
            }
        }
        length = static_cast<std::int64_t>(planned.take);
    }

    return build_batch_from_schema(batch_schema, mapping, decoded_by_field_id, length, batch, error);
}

/// ArrowSchemaRelease dereferences `release` unconditionally, and releasing sets it to null, so
/// calling it twice on the same schema jumps through a null pointer. Every failure path below leaves
/// the schema released exactly once by going through here.
void release_schema_if_held(ArrowSchema& schema) {
    if (schema.release != nullptr) {
        ArrowSchemaRelease(&schema);
    }
}

// Release out_schema and every ArrowArray already pushed into out_batches, then clear out_batches. Used
// on every failure path after the loop has started building batches, so a mid-read error (a later
// fragment/data-file fails to decode) can't leak the batches successfully built before it.
void release_partial_read(ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches) {
    release_schema_if_held(out_schema);
    for (auto& batch : out_batches) {
        ArrowArrayRelease(&batch);
    }
    out_batches.clear();
}

}  // namespace

namespace {

/// Everything a read needs after the manifest has been parsed: where the data files are, how the
/// on-disk fields map to Arrow, and (for a projected read) which field ids survive.
///
/// This exists because the eager read, the projected eager read and the stream below are all the same
/// two steps -- open, then walk the data files -- and were three copies of the first step. The stream
/// needs to keep that state alive between `next()` calls, which is what made the duplication worth
/// removing rather than adding a fourth copy.
struct ReadPlan {
    std::filesystem::path dataset_path;
    LanceSchemaMapping mapping;
    std::vector<PlannedFile> files;  // flattened in fragment-id order, range-filtered
    std::unordered_set<std::int32_t> allowed_ids;
    bool projected = false;

    const std::unordered_set<std::int32_t>* allowed() const { return projected ? &allowed_ids : nullptr; }
};

/// Parse the manifest and build the Arrow schema. `column_names` null means every column.
bool open_read_plan(const std::filesystem::path& dataset_path,
                    const std::vector<std::string>* column_names, const LanceRowRange& range,
                    ReadPlan& plan, ArrowSchema& out_schema, std::string& error) {
    plan.dataset_path = dataset_path;

    pb::Manifest manifest{};
    std::uint64_t version = 0;
    if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        release_schema_if_held(out_schema);
        return false;
    }
    LanceSchemaMapping full_mapping;
    if (!lance_schema_mapping_from_manifest(manifest, full_mapping, error)) {
        release_schema_if_held(out_schema);
        return false;
    }

    if (column_names == nullptr) {
        plan.mapping = std::move(full_mapping);
    } else {
        // Collect the requested top-level columns and ALL their descendants: a projected struct
        // column is only meaningful together with the children that hold its data.
        for (const auto& col_name : *column_names) {
            const LanceField* root = nullptr;
            for (const auto& f : full_mapping.fields) {
                if (f.parent_id == -1 && f.name == col_name) {
                    root = &f;
                    break;
                }
            }
            if (root == nullptr) {
                release_schema_if_held(out_schema);
                error = "projected column '" + col_name + "' not found in schema";
                return false;
            }
            std::vector<std::int32_t> queue = {root->id};
            while (!queue.empty()) {
                const auto id = queue.back();
                queue.pop_back();
                plan.allowed_ids.insert(id);
                for (const auto& f : full_mapping.fields) {
                    if (f.parent_id == id) {
                        queue.push_back(f.id);
                    }
                }
            }
        }
        plan.projected = true;
        for (const auto& f : full_mapping.fields) {
            if (plan.allowed_ids.count(f.id) != 0U) {
                plan.mapping.fields.push_back(f);
            }
        }
    }

    if (!build_schema_from_mapping(plan.mapping, out_schema, error)) {
        release_schema_if_held(out_schema);
        return false;
    }

    std::vector<pb::DataFragment> fragments = manifest.fragments;
    std::sort(fragments.begin(), fragments.end(),
              [](const pb::DataFragment& a, const pb::DataFragment& b) { return a.id < b.id; });

    const bool ranged = !range.is_whole_dataset();
    std::uint64_t cursor = 0;  // absolute row index of the next fragment's first row
    for (auto& fragment : fragments) {
        if (fragment.files.empty()) {
            continue;
        }
        // Deleted rows are not rows any more: a range, a count and a full read all have to agree,
        // and a full read does not return them. So the plan counts in LOGICAL rows throughout and
        // the deletion filter runs before the range is applied.
        if (fragment.deletion_file.present &&
            fragment.deletion_file.num_deleted_rows > fragment.physical_rows) {
            release_schema_if_held(out_schema);
            error = "fragment claims more deleted rows than it holds";
            return false;
        }
        const auto rows = fragment.physical_rows - (fragment.deletion_file.present
                                                        ? fragment.deletion_file.num_deleted_rows
                                                        : 0U);
        const auto first_row = cursor;
        if (rows > std::numeric_limits<std::uint64_t>::max() - cursor) {
            release_schema_if_held(out_schema);
            error = "dataset row count overflows a 64-bit integer";
            return false;
        }
        cursor += rows;

        PlannedFile planned;
        planned.fragment_id = fragment.id;
        planned.deletion_file = fragment.deletion_file;
        planned.physical_rows = fragment.physical_rows;
        planned.rows = rows;

        if (!ranged) {
            planned.files = std::move(fragment.files);
            planned.skip = 0U;
            planned.take = rows;
            plan.files.push_back(std::move(planned));
            continue;
        }

        // Half-open intersection of [first_row, first_row + rows) with the requested range. An empty
        // intersection means the file is dropped from the plan entirely and never opened.
        const auto want_begin = range.offset;
        const auto want_end = range.length == LanceRowRange::kAllRows
                                  ? std::numeric_limits<std::uint64_t>::max()
                                  : (range.offset > std::numeric_limits<std::uint64_t>::max() - range.length
                                         ? std::numeric_limits<std::uint64_t>::max()
                                         : range.offset + range.length);
        const auto file_end = first_row + rows;
        const auto begin = std::max(want_begin, first_row);
        const auto end = std::min(want_end, file_end);
        if (begin >= end) {
            continue;
        }
        planned.files = std::move(fragment.files);
        planned.skip = begin - first_row;
        planned.take = end - begin;
        plan.files.push_back(std::move(planned));
    }

    if (ranged && range.offset > cursor) {
        release_schema_if_held(out_schema);
        error = "row range starts past the end of the dataset (" + std::to_string(cursor) + " rows)";
        return false;
    }
    return true;
}

/// Decode every data file up front. The eager reads' second half.
bool read_all_batches(const ReadPlan& plan, ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches,
                      std::string& error) {
    for (const auto& planned : plan.files) {
        ArrowArray batch{};
        if (!read_data_file_batch(plan.dataset_path, planned, plan.mapping, out_schema, batch, error,
                                  plan.allowed())) {
            release_partial_read(out_schema, out_batches);
            return false;
        }
        out_batches.push_back(batch);
    }
    return true;
}

bool read_dataset_eager(const std::filesystem::path& dataset_path,
                        const std::vector<std::string>* column_names, const LanceRowRange& range,
                        ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches,
                        std::string& error, bool trusted_input) {
    error.clear();
    out_batches.clear();
    ArrowSchemaInit(&out_schema);

    std::optional<ScopedReadLimits> trusted_scope;
    if (trusted_input) {
        trusted_scope.emplace(trusted_read_limits());
    }
    ReadPlan plan;
    if (!open_read_plan(dataset_path, column_names, range, plan, out_schema, error)) {
        return false;
    }
    return read_all_batches(plan, out_schema, out_batches, error);
}

}  // namespace

bool lance_table_read_dataset(const std::filesystem::path& dataset_path, ArrowSchema& out_schema,
                              std::vector<ArrowArray>& out_batches, std::string& error,
                              bool trusted_input) {
    return read_dataset_eager(dataset_path, /*column_names=*/nullptr, LanceRowRange{}, out_schema,
                              out_batches, error, trusted_input);
}

bool lance_table_read_dataset_projected(const std::filesystem::path& dataset_path,
                                        const std::vector<std::string>& column_names,
                                        ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches,
                                        std::string& error, bool trusted_input) {
    return read_dataset_eager(dataset_path, &column_names, LanceRowRange{}, out_schema, out_batches,
                              error, trusted_input);
}

bool lance_table_read_dataset_range(const std::filesystem::path& dataset_path,
                                    const std::vector<std::string>* column_names,
                                    const LanceRowRange& range, ArrowSchema& out_schema,
                                    std::vector<ArrowArray>& out_batches, std::string& error,
                                    bool trusted_input) {
    return read_dataset_eager(dataset_path, column_names, range, out_schema, out_batches, error,
                              trusted_input);
}

/// The stream's state. Held by pointer so the public header stays free of the manifest and schema
/// mapping types.
struct LanceTableStream::Impl {
    ReadPlan plan;
    ArrowSchema schema{};   // the stream's own copy; the caller got a deep copy at open()
    std::size_t cursor = 0;
    bool trusted_input = false;

    ~Impl() { release_schema_if_held(schema); }
};

LanceTableStream::LanceTableStream() = default;
LanceTableStream::~LanceTableStream() = default;
LanceTableStream::LanceTableStream(LanceTableStream&&) noexcept = default;
LanceTableStream& LanceTableStream::operator=(LanceTableStream&&) noexcept = default;

bool LanceTableStream::open(const std::filesystem::path& dataset_path,
                            const std::vector<std::string>* column_names, ArrowSchema& out_schema,
                            LanceTableStream& out, std::string& error, bool trusted_input) {
    return open_range(dataset_path, column_names, LanceRowRange{}, out_schema, out, error, trusted_input);
}

bool LanceTableStream::open_range(const std::filesystem::path& dataset_path,
                                  const std::vector<std::string>* column_names,
                                  const LanceRowRange& range, ArrowSchema& out_schema,
                                  LanceTableStream& out, std::string& error, bool trusted_input) {
    error.clear();
    ArrowSchemaInit(&out_schema);

    std::optional<ScopedReadLimits> trusted_scope;
    if (trusted_input) {
        trusted_scope.emplace(trusted_read_limits());
    }

    auto impl = std::make_unique<Impl>();
    impl->trusted_input = trusted_input;
    if (!open_read_plan(dataset_path, column_names, range, impl->plan, out_schema, error)) {
        return false;
    }
    // The stream keeps its own schema: read_data_file_batch builds each batch against one, and the
    // caller owns (and may release) the schema it was handed the moment open() returns.
    if (ArrowSchemaDeepCopy(&out_schema, &impl->schema) != NANOARROW_OK) {
        release_schema_if_held(out_schema);
        error = "failed to copy the dataset schema for the stream";
        return false;
    }
    out.impl_ = std::move(impl);
    return true;
}

bool LanceTableStream::next(ArrowArray& out_batch, std::string& error) {
    error.clear();
    out_batch = ArrowArray{};
    if (impl_ == nullptr) {
        error = "stream is not open";
        return false;
    }
    if (impl_->cursor >= impl_->plan.files.size()) {
        return true;  // end of stream: out_batch.release stays null
    }

    // The limits are per-thread and scoped, so a trusted stream has to re-establish them on every
    // next() -- open()'s scope ended when open() returned.
    std::optional<ScopedReadLimits> trusted_scope;
    if (impl_->trusted_input) {
        trusted_scope.emplace(trusted_read_limits());
    }
    const auto& planned = impl_->plan.files[impl_->cursor];
    if (!read_data_file_batch(impl_->plan.dataset_path, planned, impl_->plan.mapping, impl_->schema,
                              out_batch, error, impl_->plan.allowed())) {
        out_batch = ArrowArray{};
        return false;
    }
    ++impl_->cursor;
    return true;
}


bool lance_table_read_schema(const std::filesystem::path& dataset_path, ArrowSchema& out_schema,
                             std::string& error) {
    error.clear();
    ArrowSchemaInit(&out_schema);

    pb::Manifest manifest{};
    std::uint64_t version = 0;
    if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        release_schema_if_held(out_schema);
        return false;
    }
    LanceSchemaMapping mapping;
    if (!lance_schema_mapping_from_manifest(manifest, mapping, error)) {
        release_schema_if_held(out_schema);
        return false;
    }
    if (!build_schema_from_mapping(mapping, out_schema, error)) {
        release_schema_if_held(out_schema);
        return false;
    }
    return true;
}

bool lance_table_count_rows(const std::filesystem::path& dataset_path, std::uint64_t& out_rows,
                            std::string& error) {
    error.clear();
    out_rows = 0;

    pb::Manifest manifest{};
    std::uint64_t version = 0;
    if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        return false;
    }
    // Summed from the fragments rather than taken from a total field: the count has to agree with
    // what a read actually returns, and a read walks these same fragments. That is also why deleted
    // rows are subtracted -- a read does not return them, so counting them would make count_rows
    // disagree with len(read_table(...)), which is the one thing it must never do.
    for (const auto& fragment : manifest.fragments) {
        const auto deleted =
            fragment.deletion_file.present ? fragment.deletion_file.num_deleted_rows : 0U;
        if (deleted > fragment.physical_rows) {
            error = "fragment claims more deleted rows than it holds";
            out_rows = 0;
            return false;
        }
        const auto live = fragment.physical_rows - deleted;
        if (live > UINT64_MAX - out_rows) {
            error = "dataset row count overflows a 64-bit integer";
            out_rows = 0;
            return false;
        }
        out_rows += live;
    }
    return true;
}

}  // namespace nano_lance
