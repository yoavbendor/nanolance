// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/lance_table_reader.hpp"

#include "nanolance/blob_v2_external.hpp"
#include "nanolance/column_slice.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/deletion_vector.hpp"
#include "nanolance/bool_bitpack.hpp"
#include "nanolance/buffer_pool.hpp"
#include "nanolance/arrow_slice.hpp"
#include "nanolance/expr.hpp"
#include "nanolance/lance_column_decoder.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/page_layout.hpp"
#include "nanolance/parallel.hpp"
#include "nanolance/work_stats.hpp"
#include "nanolance/path_safety.hpp"
#include "nanolance/read_safety.hpp"
#include "nanolance/schema_mapper.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <memory>
#include <mutex>
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
    } else if (lance_logical_type_is_list(field.logical_type)) {
        // One child, the element field Lance's schema names "item".
        const LanceField* child = nullptr;
        for (const auto& candidate : mapping.fields) {
            if (candidate.parent_id == field.id) {
                if (child != nullptr) {
                    error = "list field " + field.name + " has more than one child";
                    return false;
                }
                child = &candidate;
            }
        }
        if (child == nullptr) {
            error = "list field " + field.name + " has no element field";
            return false;
        }
        const char* list_format = field.logical_type == "map"                          ? "+m"
                                  : lance_logical_type_is_large_list(field.logical_type) ? "+L"
                                                                                         : "+l";
        if (ArrowSchemaSetFormat(&schema, list_format) != NANOARROW_OK ||
            ArrowSchemaAllocateChildren(&schema, 1) != NANOARROW_OK) {
            error = "failed to build the list schema for " + field.name;
            return false;
        }
        if (!init_schema_from_field(*child, mapping, *schema.children[0], error)) {
            return false;
        }
        if (field.logical_type == "map") {
            // Arrow's map is a list of non-nullable (key, value) structs with non-nullable keys;
            // nanoarrow refuses the schema otherwise. The data agrees: an entry is never null.
            ArrowSchema* entries = schema.children[0];
            if (entries->n_children != 2) {
                error = "map field " + field.name + " does not hold (key, value) entries";
                return false;
            }
            entries->flags &= ~ARROW_FLAG_NULLABLE;
            entries->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
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
    // The schema says what the manifest says. Every field used to come back nullable, so a table read
    // from a dataset with a non-nullable column could not be appended to it again.
    if (!field.nullable) {
        schema.flags &= ~ARROW_FLAG_NULLABLE;
    }
    for (const auto& kv : field.metadata) {
        // How nanolance encoded a fragment's pages is not part of the schema a reader sees.
        if (kv.first == "nanolance:packing" || kv.first == "nanolance:const-value") {
            continue;
        }
        if (!set_schema_metadata(schema, kv.first, kv.second)) {
            error = "failed to set schema metadata";
            return false;
        }
    }
    return true;
}

/// The dataset's Arrow schema metadata (Manifest.schema_metadata) on the top-level schema.
bool set_dataset_schema_metadata(ArrowSchema& schema, const pb::Manifest& manifest, std::string& error) {
    if (manifest.schema_metadata.empty()) {
        return true;
    }
    ArrowBuffer buffer;
    if (ArrowMetadataBuilderInit(&buffer, schema.metadata) != NANOARROW_OK) {
        error = "failed to build schema metadata";
        return false;
    }
    for (const auto& [key, value] : manifest.schema_metadata) {
        ArrowStringView k{key.data(), static_cast<std::int64_t>(key.size())};
        ArrowStringView v{reinterpret_cast<const char*>(value.data()), static_cast<std::int64_t>(value.size())};
        if (ArrowMetadataBuilderSet(&buffer, k, v) != NANOARROW_OK) {
            ArrowBufferReset(&buffer);
            error = "failed to build schema metadata";
            return false;
        }
    }
    const bool ok = ArrowSchemaSetMetadata(&schema, reinterpret_cast<const char*>(buffer.data)) == NANOARROW_OK;
    ArrowBufferReset(&buffer);
    if (!ok) {
        error = "failed to set schema metadata";
    }
    return ok;
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
    /// The struct and list arrays from the top-level field down to the leaf `array`, outermost
    /// first. When the decoded column carries `layers` (a list above it, or a struct that can be
    /// null), those fill these nodes one to one.
    struct Node {
        enum class Kind { Struct, List, LargeList } kind;
        ArrowArray* array;
    };
    std::vector<Node> nodes;
    bool under_list() const {
        return std::any_of(nodes.begin(), nodes.end(), [](const Node& n) { return n.kind != Node::Kind::Struct; });
    }
};

// Bulk-fill a fixed-width child array's data buffer from the already-decoded column bytes.
/// Attach a decoded validity bitmap to `child`. Arrow buffer 0 is the validity bitmap in exactly the
/// layout the decoder produces (LSB-first, bit set == valid), so this is a straight copy.
/// Deallocator for a buffer whose memory is owned by a heap `std::vector<uint8_t>`.
/// nanoarrow hands `allocator->private_data` straight back to us; it is the vector itself.
void release_adopted_vector(struct ArrowBufferAllocator* allocator, std::uint8_t* /*ptr*/,
                            std::int64_t /*size*/) {
    auto* owned = static_cast<std::vector<std::uint8_t>*>(allocator->private_data);
    buffer_pool::give(std::move(*owned));  // large ones are kept for the next read (buffer_pool.hpp)
    delete owned;
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

/// What a nested node was filled with, kept so every other leaf under it can be checked against it.
struct FilledNode {
    std::vector<std::int64_t> offsets;
    std::vector<std::uint8_t> validity;
    std::uint64_t length = 0;
};

bool same_validity(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, std::uint64_t length) {
    for (std::uint64_t i = 0; i < length; ++i) {
        const bool va = a.empty() || ((a[static_cast<std::size_t>(i >> 3U)] >> (i & 7U)) & 1U) != 0U;
        const bool vb = b.empty() || ((b[static_cast<std::size_t>(i >> 3U)] >> (i & 7U)) & 1U) != 0U;
        if (va != vb) {
            return false;
        }
    }
    return true;
}

/// Give each struct and list array above a leaf its validity (and a list its offsets) from the
/// decoded column's layers. A node shared by several leaves -- a list of structs has one leaf per
/// field -- is filled by the first and must be described identically by every other: they were
/// written together, so a disagreement means a corrupt file, never a choice.
bool fill_nested_nodes(ColumnPlan& plan, std::int64_t rows, std::map<ArrowArray*, FilledNode>& filled,
                       std::string& error) {
    auto& layers = plan.values->layers;
    const auto& name = plan.field->name;
    if (layers.size() != plan.nodes.size() || layers.empty() ||
        layers.front().length != static_cast<std::uint64_t>(rows)) {
        error = "nested column '" + name + "' decoded to a different shape than its schema (" +
                std::to_string(layers.size()) + " layers for " + std::to_string(plan.nodes.size()) + " levels)";
        return false;
    }
    for (std::size_t k = 0; k < layers.size(); ++k) {
        auto& layer = layers[k];
        const auto& node = plan.nodes[k];
        const bool is_list = node.kind != ColumnPlan::Node::Kind::Struct;
        if (layer.is_list != is_list || (is_list && layer.offsets.size() != layer.length + 1U)) {
            error = "nested column '" + name + "': layer " + std::to_string(k) + " does not match its schema";
            return false;
        }
        if (const auto seen = filled.find(node.array); seen != filled.end()) {
            if (seen->second.length != layer.length || seen->second.offsets != layer.offsets ||
                !same_validity(seen->second.validity, layer.validity, layer.length)) {
                error = "nested column '" + name + "' disagrees with its sibling fields about a shared list or "
                        "struct; the file is inconsistent";
                return false;
            }
            continue;
        }
        filled[node.array] = FilledNode{layer.offsets, layer.validity, layer.length};
        ArrowArray* array = node.array;
        array->null_count = 0;
        if (!layer.validity.empty()) {
            layer.validity.resize(static_cast<std::size_t>((layer.length + 7U) / 8U));
            if (!adopt_into_buffer(std::move(layer.validity), ArrowArrayBuffer(array, 0), error)) {
                return false;
            }
            array->null_count = static_cast<std::int64_t>(layer.null_count);
        }
        if (is_list) {
            const bool large = node.kind == ColumnPlan::Node::Kind::LargeList;
            std::vector<std::uint8_t> offsets(layer.offsets.size() * (large ? 8U : 4U));
            for (std::size_t i = 0; i < layer.offsets.size(); ++i) {
                const auto value = layer.offsets[i];
                if (large) {
                    std::memcpy(offsets.data() + i * 8U, &value, 8U);
                } else {
                    if (value > std::numeric_limits<std::int32_t>::max()) {
                        error = "list column '" + name + "' has more than 2^31 elements in one batch; "
                                "read it as large_list";
                        return false;
                    }
                    const auto narrow = static_cast<std::int32_t>(value);
                    std::memcpy(offsets.data() + i * 4U, &narrow, 4U);
                }
            }
            if (!adopt_into_buffer(std::move(offsets), ArrowArrayBuffer(array, 1), error)) {
                return false;
            }
        }
        array->length = static_cast<std::int64_t>(layer.length);
    }
    return true;
}

bool fill_fixed_child(ArrowArray* child, std::vector<std::uint8_t>&& bytes, std::int64_t rows,
                      FixedFmt fmt, std::string& error) {
    ArrowBuffer* data = ArrowArrayBuffer(child, 1);
    // bool is the one fixed type that cannot be adopted: it is a byte per value on disk and a BIT per
    // value in the Arrow buffer, so the bits have to be packed somewhere. They are packed into a fresh
    // vector, which is then adopted -- so this path still copies once (unavoidably) rather than twice.
    if (fmt == FixedFmt::kBool) {
        std::vector<std::uint8_t> packed;
        boolpack::pack_lsb_first(bytes.data(), static_cast<std::size_t>(rows), packed);
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
                             std::int32_t parent_id, std::vector<ColumnPlan::Node> path) -> bool {
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

        if (lance_logical_type_is_list(field->logical_type)) {
            // A list array is not a leaf: its data lives in the element's column, which carries every
            // list layer above it.
            if (node_schema->n_children != 1 || node_array->n_children != 1 || node_schema->children[0] == nullptr) {
                collect_error = "list array has no element child for " + field->name;
                return false;
            }
            path.push_back({lance_logical_type_is_large_list(field->logical_type) ? ColumnPlan::Node::Kind::LargeList
                                                                                  : ColumnPlan::Node::Kind::List,
                            node_array});
            return self(self, node_schema->children[0], node_array->children[0], field->id, std::move(path));
        }

        if (is_struct) {
            const bool under_list = std::any_of(path.begin(), path.end(), [](const ColumnPlan::Node& n) {
                return n.kind != ColumnPlan::Node::Kind::Struct;
            });
            if (!under_list) {
                struct_nodes.push_back(node_array);  // one entry per row; a struct in a list is sized by its layer
            }
            path.push_back({ColumnPlan::Node::Kind::Struct, node_array});
            for (std::int64_t i = 0; i < node_schema->n_children; ++i) {
                if (node_array->children == nullptr || i >= node_array->n_children) {
                    collect_error = "struct array is missing children for ";
                    collect_error += field->name;
                    return false;
                }
                if (!self(self, node_schema->children[i], node_array->children[i], field->id, path)) {
                    return false;
                }
            }
            return true;
        }

        ColumnPlan plan;
        plan.array = node_array;
        plan.field = field;
        plan.nodes = std::move(path);
        if (plan.under_list() && (is_blob || field->column_index < 0)) {
            collect_error = "column '" + field->name + "': a list of " +
                            (is_blob ? std::string("blobs") : field->logical_type) + " is not read yet";
            return false;
        }
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
        if (!collect(collect, batch_schema.children[c], batch.children[c], -1, {})) {
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
        std::map<ArrowArray*, FilledNode> filled;
        for (auto& plan : plans) {
            // A nested column's leaf holds ITEMS: as many as its innermost layer has children.
            std::int64_t leaf_length = length;
            if (!plan.values->layers.empty()) {
                const auto& inner = plan.values->layers.back();
                leaf_length = static_cast<std::int64_t>(inner.is_list ? static_cast<std::uint64_t>(inner.offsets.back())
                                                                      : inner.length);
                if (!fill_nested_nodes(plan, length, filled, error)) {
                    ArrowArrayRelease(&batch);
                    return false;
                }
            } else if (plan.under_list()) {
                error = "column '" + plan.field->name + "' sits under a list but decoded with no list layers";
                ArrowArrayRelease(&batch);
                return false;
            }
            // Validity first: nanoarrow expects buffer 0 filled before the data buffers it sizes
            // against, and both fill_* helpers set child->length/null_count at the end.
            bool ok = fill_validity(plan.array, *plan.values, leaf_length, error);
            if (ok && plan.kind == ColumnPlan::Kind::FixedSizeList) {
                // Row-level validity sits on the list; the child carries every row's N elements,
                // including a null row's, which is what Arrow's fixed_size_list layout requires.
                ArrowArray* child = plan.array->n_children == 1 ? plan.array->children[0] : nullptr;
                std::uint64_t child_length = 0;
                if (child == nullptr ||
                    !checked_mul(static_cast<std::uint64_t>(leaf_length), static_cast<std::uint64_t>(plan.width),
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
                    plan.array->length = leaf_length;
                }
            } else if (ok) {
                ok = plan.kind == ColumnPlan::Kind::Fixed
                         ? fill_fixed_child(plan.array, std::move(plan.values->fixed), leaf_length, plan.fmt, error)
                         : fill_variable_child(plan.array, plan.values->variable, leaf_length, error);
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
            if (filled.find(node) == filled.end()) {
                node->null_count = 0;  // no leaf said it could be null
            }
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
        return plan.kind == ColumnPlan::Kind::FixedSizeList || (plan.values != nullptr && !plan.values->layers.empty());
    });
    if (has_fsl) {
        error =
            "a fixed_size_list or list column cannot be read back in the same batch as a lance.blob.v2 column "
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
    std::filesystem::path data_dir;    // where the data files are; empty: <dataset>/data

    bool partial() const { return skip != 0U || take != rows; }
};

/// One column of one data file, as the fragment's read decodes it.
struct ColumnSource {
    std::filesystem::path path;
    const pb::Field* on_disk = nullptr;
    const pb::ColumnMetadata* metadata = nullptr;
    std::int32_t field_id = 0;
    std::size_t value_bytes = 0;  // as compaction and slicing count it
    std::uint64_t encoded_bytes = 0;
};

/// A parallel read cuts a fragment into row ranges ("morsels") of at least this many encoded bytes,
/// decoded independently and returned as a batch each -- no concatenation afterwards.
/// NANOLANCE_MORSEL_KB overrides it (default 4096 of decode work: below that -- reads of a millisecond
/// or two -- waking threads and building more batches costs about what it saves; under 64 the tests'
/// setting, which cuts every page).
std::uint64_t morsel_min_bytes() {
    static const std::uint64_t bytes = [] {
        const char* env = std::getenv("NANOLANCE_MORSEL_KB");
        const auto kb = env == nullptr || *env == '\0' ? 4096ULL : std::strtoull(env, nullptr, 10);
        return std::max<std::uint64_t>(1U, kb) << 10U;
    }();
    return bytes;
}

/// Physical row ranges covering [first, end): one with a single thread or a small read; otherwise up
/// to two per thread, cut at page boundaries of the column holding the most bytes (so it is split
/// with no page decoded twice) -- or, when its pages can give up row ranges (lance_page_row_addressable),
/// evenly. A page no cut can avoid is decoded by every morsel it overlaps, so a plan that would decode
/// over a quarter more than the whole is halved until it does not: a column of a few large pages
/// is not worth splitting.
std::vector<std::pair<std::uint64_t, std::uint64_t>> plan_morsels(const std::vector<ColumnSource>& columns,
                                                                  std::uint64_t first, std::uint64_t end) {
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> single{{first, end}};
    const auto threads = parallel::threads();
    if (threads <= 1U || end - first < 2U || columns.empty()) {
        return single;
    }
    struct PageInfo {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::uint64_t bytes = 0;
        std::uint64_t items = 0;  // values it decodes to
        std::uint32_t lists = 0;  // list layers to unravel
        bool addressable = false;
    };
    std::vector<std::vector<PageInfo>> pages(columns.size());
    std::uint64_t range_bytes = 0;  // encoded bytes of the pages the range touches
    std::size_t heaviest = 0;
    std::uint64_t heaviest_bytes = 0;
    for (std::size_t c = 0; c < columns.size(); ++c) {
        std::uint64_t row = 0;
        std::uint64_t column_bytes = 0;
        for (const auto& page : columns[c].metadata->pages) {
            PageInfo info;
            info.begin = row;
            info.end = row + page.length;
            row = info.end;
            if (info.end <= first || info.begin >= end) {
                continue;
            }
            for (const auto size : page.buffer_sizes) {
                info.bytes += size;
            }
            info.addressable = lance_page_row_addressable(page);
            info.items = lance_page_items(page);
            info.lists = lance_page_list_depth(page);
            column_bytes += info.bytes;
            pages[c].push_back(info);
        }
        range_bytes += column_bytes;
        if (column_bytes > heaviest_bytes) {
            heaviest_bytes = column_bytes;
            heaviest = c;
        }
    }
    // Work: per page, its encoded bytes or what it decodes to, whichever is more -- bit-packed small
    // integers decode to four or eight times their size, a page of dictionary-coded strings to many
    // times its (value count x the value width; a string counts as 32 bytes -- dictionary lookup or
    // FSST expansion, an offset, the bytes -- which is about what one costs to decode against an int).
    std::uint64_t work = 0;
    for (std::size_t c = 0; c < columns.size(); ++c) {
        // The values' own type (a list column's items), as the decoder sees it.
        const auto& leaf_type = columns[c].on_disk->logical_type;
        const std::uint64_t per_value = lance_field_is_variable_width(leaf_type)
                                            ? 32U
                                            : std::max<std::uint64_t>(1U, lance_logical_type_value_bytes(leaf_type));
        for (const auto& page : pages[c]) {
            const auto rows = std::max<std::uint64_t>(1U, page.end - page.begin);
            const auto rows_here = std::min(end, page.end) - std::max(first, page.begin);
            // A list page also costs its levels to unravel, per list layer, whatever its items'
            // width -- and has a level for every row, empty lists included.
            const auto units = page.lists != 0U ? std::max(page.items, rows) : page.items;
            const auto weight = page.lists != 0U ? std::max<std::uint64_t>(per_value, 32U * page.lists) : per_value;
            work += std::max(page.bytes, units * rows_here / rows * weight);
        }
    }
    auto want = std::min<std::uint64_t>(2U * threads, work / morsel_min_bytes());

    // What a plan decodes: every overlapped page, whole unless it is row-addressable.
    const auto cost = [&](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& plan) {
        double total = 0;
        for (const auto& column : pages) {
            for (const auto& page : column) {
                for (const auto& [a, b] : plan) {
                    const auto lo = std::max(a, page.begin);
                    const auto hi = std::min(b, page.end);
                    if (lo < hi) {
                        total += page.addressable ? static_cast<double>(page.bytes) * static_cast<double>(hi - lo) /
                                                        static_cast<double>(page.end - page.begin)
                                                  : static_cast<double>(page.bytes);
                    }
                }
            }
        }
        return total;
    };
    const auto& spine = pages[heaviest];
    const bool spine_addressable =
        std::all_of(spine.begin(), spine.end(), [](const PageInfo& p) { return p.addressable; });
    const bool test_setting = morsel_min_bytes() < (std::uint64_t{64} << 10U);
    for (; want > 1U; want /= 2U) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> plan;
        std::uint64_t at = first;
        for (std::uint64_t k = 1; k < want; ++k) {
            std::uint64_t cut = first + (end - first) * k / want;
            if (!spine_addressable && !test_setting) {
                // The page boundary nearest the k-th share of the heaviest column's bytes.
                const auto target = heaviest_bytes * k / want;
                std::uint64_t acc = 0;
                cut = 0;
                for (const auto& page : spine) {
                    acc += page.bytes;
                    if (acc >= target) {
                        cut = page.end;
                        break;
                    }
                }
            }
            if (cut > at && cut < end) {
                plan.emplace_back(at, cut);
                at = cut;
            }
        }
        plan.emplace_back(at, end);
        // Morsels under 64 KiB are a test setting: cut evenly, through any page, whatever it costs.
        if (plan.size() > 1U && (test_setting || cost(plan) <= 1.25 * static_cast<double>(range_bytes))) {
            return plan;
        }
    }
    return single;
}

/// One fragment's rows as Arrow batches: one batch, or with several threads one per morsel.

/// Which of Lance's row identity columns a read adds after the data columns.
struct RowIdColumns {
    bool row_id = false;       // `_rowid`
    bool row_address = false;  // `_rowaddr`

    bool any() const { return row_id || row_address; }
};

/// What a scan filters by: `expr` (null for no filter), bound to `schema`, reading the columns `ids`.
/// The fields a read wants that no data file of the fragment holds: columns added to the schema
/// after the fragment was written (pylance's add_columns with a pa.field writes no data). They read
/// as nulls, as in Lance.
std::vector<const LanceField*> fields_missing_from(const PlannedFile& planned, const LanceSchemaMapping& mapping,
                                                   const std::unordered_set<std::int32_t>* allowed_field_ids) {
    std::unordered_set<std::int32_t> held;
    for (const auto& file : planned.files) {
        held.insert(file.fields.begin(), file.fields.end());
    }
    std::vector<const LanceField*> missing;
    for (const auto& f : mapping.fields) {
        if (f.column_index < 0 || held.count(f.id) != 0U ||
            (allowed_field_ids != nullptr && allowed_field_ids->count(f.id) == 0U)) {
            continue;
        }
        missing.push_back(&f);
    }
    return missing;
}

/// `rows` nulls of `field`'s type.
bool null_column_values(const LanceField& field, std::uint64_t rows, ColumnValues& out, std::string& error) {
    out = ColumnValues{};
    out.rows = rows;
    out.validity.assign(static_cast<std::size_t>((rows + 7U) / 8U), 0U);
    out.null_count = rows;
    if (lance_field_is_variable_width(field.logical_type)) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = lance_logical_type_has_large_offsets(field.logical_type);
        out.variable.offsets.assign(static_cast<std::size_t>(rows + 1U) * (out.variable.large ? 8U : 4U), 0U);
        return true;
    }
    const auto width = lance_logical_type_value_bytes(field.logical_type);
    if (width == 0U || field.logical_type == "struct" || field.logical_type.rfind("list", 0) == 0 ||
        field.logical_type.rfind("large_list", 0) == 0) {
        error = "column '" + field.name + "' has no data in this fragment, and nulls of type " +
                field.logical_type + " are not read yet";
        return false;
    }
    out.kind = ColumnValues::Kind::FixedWidth;
    out.fixed.assign(static_cast<std::size_t>(rows) * width, 0U);
    std::string element;
    std::uint64_t items = 0;
    if (lance_fixed_size_list_parts(field.logical_type, element, items)) {
        out.items_per_row = items;
    }
    return true;
}

struct FilterSpec {
    const expr::Expression* expr = nullptr;
    const ArrowSchema* schema = nullptr;
    const std::vector<std::int32_t>* ids = nullptr;
};

/// A row's address: its fragment in the high 32 bits, its offset in the fragment's data files (deleted
/// rows counted) in the low. Without stable row ids it is also the row's id.
std::uint64_t row_address(std::uint64_t fragment_id, std::uint64_t offset) {
    return (fragment_id << 32U) | offset;
}

bool make_u64_array(const std::vector<std::uint64_t>& values, ArrowArray& out, std::string& error) {
    if (ArrowArrayInitFromType(&out, NANOARROW_TYPE_UINT64) != NANOARROW_OK) {
        error = "failed to allocate a row id column";
        return false;
    }
    auto* data = ArrowArrayBuffer(&out, 1);
    if (ArrowBufferAppend(data, values.data(), static_cast<std::int64_t>(values.size() * sizeof(std::uint64_t))) !=
        NANOARROW_OK) {
        ArrowArrayRelease(&out);
        error = "failed to fill a row id column";
        return false;
    }
    out.length = static_cast<std::int64_t>(values.size());
    out.null_count = 0;
    if (ArrowArrayFinishBuildingDefault(&out, nullptr) != NANOARROW_OK) {
        ArrowArrayRelease(&out);
        error = "failed to finish a row id column";
        return false;
    }
    return true;
}

/// Replace `batch` (a struct array) by one with the row identity columns of `physical` (offsets in
/// fragment `fragment_id`) after its own. `batch.release` null means a batch without data columns.
bool add_row_id_columns(ArrowArray& batch, std::int64_t length, std::uint64_t fragment_id,
                        const std::vector<std::uint64_t>& physical, const RowIdColumns& ids, std::string& error) {
    std::vector<std::uint64_t> addresses(physical.size());
    for (std::size_t i = 0; i < physical.size(); ++i) {
        addresses[i] = row_address(fragment_id, physical[i]);
    }
    const auto own = batch.release == nullptr ? 0 : batch.n_children;
    const auto extra = static_cast<std::int64_t>(ids.row_id) + static_cast<std::int64_t>(ids.row_address);
    ArrowArray out{};
    if (ArrowArrayInitFromType(&out, NANOARROW_TYPE_STRUCT) != NANOARROW_OK ||
        ArrowArrayAllocateChildren(&out, own + extra) != NANOARROW_OK) {
        if (out.release != nullptr) {
            ArrowArrayRelease(&out);
        }
        error = "failed to allocate a batch with row ids";
        return false;
    }
    for (std::int64_t c = 0; c < own; ++c) {
        ArrowArrayMove(batch.children[c], out.children[c]);
    }
    auto next = own;
    if (ids.row_id && !make_u64_array(addresses, *out.children[next++], error)) {
        ArrowArrayRelease(&out);
        return false;
    }
    if (ids.row_address && !make_u64_array(addresses, *out.children[next++], error)) {
        ArrowArrayRelease(&out);
        return false;
    }
    out.length = length;
    out.null_count = 0;
    if (batch.release != nullptr) {
        ArrowArrayRelease(&batch);
    }
    batch = out;
    return true;
}

/// `schema` with the row identity columns after its own fields.
bool add_row_id_fields(ArrowSchema& schema, const RowIdColumns& ids, std::string& error) {
    ArrowSchema out{};
    ArrowSchemaInit(&out);
    const auto own = schema.n_children;
    const auto extra = static_cast<std::int64_t>(ids.row_id) + static_cast<std::int64_t>(ids.row_address);
    if (ArrowSchemaSetTypeStruct(&out, own + extra) != NANOARROW_OK ||
        ArrowSchemaSetMetadata(&out, schema.metadata) != NANOARROW_OK) {
        ArrowSchemaRelease(&out);
        error = "failed to allocate a schema with row ids";
        return false;
    }
    for (std::int64_t c = 0; c < own; ++c) {
        ArrowSchemaRelease(out.children[c]);
        ArrowSchemaMove(schema.children[c], out.children[c]);
    }
    auto next = own;
    for (const char* name : {"_rowid", "_rowaddr"}) {
        if ((name[4] == 'i' && !ids.row_id) || (name[4] == 'a' && !ids.row_address)) {
            continue;
        }
        auto* child = out.children[next++];
        if (ArrowSchemaSetType(child, NANOARROW_TYPE_UINT64) != NANOARROW_OK ||
            ArrowSchemaSetName(child, name) != NANOARROW_OK) {
            ArrowSchemaRelease(&out);
            error = "failed to add a row id field";
            return false;
        }
        // Nullable, as pylance declares them, though they never hold a null.
    }
    ArrowSchemaRelease(&schema);
    ArrowSchemaMove(&out, &schema);
    return true;
}

bool read_data_file_batches(const std::filesystem::path& dataset_path, const PlannedFile& planned,
                            const LanceSchemaMapping& mapping, const ArrowSchema& batch_schema,
                            std::vector<ArrowArray>& out, std::string& error,
                            const std::unordered_set<std::int32_t>* allowed_field_ids = nullptr,
                            const RowIdColumns& ids = {}, const FilterSpec& filter = {}) {
    if (planned.files.empty()) {
        error = "fragment has no data files";
        return false;
    }

    // One batch is one read operation, so each data file it touches is validated once here rather
    // than once per page buffer (see DataFileReadScope).
    const DataFileReadScope read_scope;

    // One fragment -- however many files its columns are split across.
    struct OpenFile {
        pb::FileDescriptor descriptor;
        std::vector<pb::ColumnMetadata> columns;
    };
    std::vector<OpenFile> files(planned.files.size());
    std::vector<ColumnSource> columns;
    std::int64_t length = -1;
    for (std::size_t f = 0; f < planned.files.size(); ++f) {
        const auto& data_file = planned.files[f];
        // data_file.path is attacker-controlled (it comes out of the untrusted manifest). Confine it
        // under <dataset>/data/ so a hostile ".."/absolute path can't make the reader open a file
        // outside the dataset. The writer only ever stores a bare filename here, so legitimate
        // datasets are unaffected.
        const auto jailed = safe_join_under(planned.data_dir.empty() ? dataset_path / "data" : planned.data_dir,
                                            data_file.path);
        if (!jailed) {
            error = "data file path escapes the dataset directory";
            return false;
        }
        const auto& path = *jailed;
        auto& open = files[f];
        LanceDataFileFooterLayout layout{};
        if (!read_lance_data_file_footer_and_descriptor(path, open.descriptor, layout, error)) {
            return false;
        }
        if (!read_lance_data_file_column_metadatas(path, layout, open.columns, error)) {
            return false;
        }
        if (data_file.fields.size() != data_file.column_indices.size()) {
            error = "data file field/column index mismatch";
            return false;
        }
        // Every file of a fragment describes the SAME rows. A disagreement means the manifest and the
        // files are out of step, and merging them would silently pad or truncate a column.
        if (length < 0) {
            length = static_cast<std::int64_t>(open.descriptor.length);
        } else if (static_cast<std::uint64_t>(length) != open.descriptor.length) {
            error = "data files within one fragment disagree on their row count";
            return false;
        }

        for (std::size_t i = 0; i < data_file.fields.size(); ++i) {
            const auto field_id = data_file.fields[i];
            // Skip columns not in the projection (if one is set), and columns the schema no longer
            // has: a dropped column's data stays in the files it was written to.
            if (allowed_field_ids && !allowed_field_ids->count(field_id)) continue;
            if (find_mapping_field(mapping, field_id) == nullptr) continue;

            const auto column_index = data_file.column_indices[i];
            if (column_index < 0 || static_cast<std::size_t>(column_index) >= open.columns.size()) {
                error = "data file column index out of range";
                return false;
            }
            const auto* on_disk = find_descriptor_field(open.descriptor, field_id);
            if (on_disk == nullptr) {
                error = "data file references unknown field id";
                return false;
            }
            ColumnSource source;
            source.path = path;
            source.on_disk = on_disk;
            source.metadata = &open.columns[static_cast<std::size_t>(column_index)];
            source.field_id = field_id;
            const auto* field = find_mapping_field(mapping, field_id);
            source.value_bytes = field == nullptr ? 0U : lance_logical_type_value_bytes(field->logical_type);
            for (const auto& page : source.metadata->pages) {
                for (const auto size : page.buffer_sizes) {
                    source.encoded_bytes += size;
                }
            }
            columns.push_back(std::move(source));
        }
    }
    const auto physical = static_cast<std::uint64_t>(length);
    const auto missing = fields_missing_from(planned, mapping, allowed_field_ids);

    // Deletions first, then the row range: a range is expressed in LOGICAL row numbers, which only
    // exist once the deleted rows are gone.
    std::vector<std::uint8_t> keep;  // empty: no deletions
    if (planned.deletion_file.present) {
        if (physical != planned.physical_rows) {
            error = "data file holds " + std::to_string(length) +
                    " rows but the manifest claims " + std::to_string(planned.physical_rows) +
                    "; refusing to apply a deletion vector against rows that do not line up";
            return false;
        }
        std::vector<std::uint32_t> deleted;
        if (!read_deletion_vector(dataset_path, planned.fragment_id, planned.deletion_file, deleted, error)) {
            return false;
        }
        keep.assign(static_cast<std::size_t>(physical), 1U);
        for (const auto row : deleted) {
            if (row >= physical) {
                error = "deletion file names row " + std::to_string(row) + " but the fragment holds " +
                        std::to_string(length);
                return false;
            }
            keep[row] = 0U;
        }
    }
    const auto logical = keep.empty() ? physical
                                      : static_cast<std::uint64_t>(std::count(keep.begin(), keep.end(), 1U));
    // The plan's skip/take came from the MANIFEST's per-fragment row count, while the rows are here in
    // the data file. If the two disagree, the arithmetic that decided which files to skip was wrong,
    // and a silently misaligned row range is exactly the failure this must not have.
    if ((planned.partial() || !keep.empty()) && logical != planned.rows) {
        error = "data file holds " + std::to_string(logical) + " rows but the manifest claims " +
                std::to_string(planned.rows) + "; refusing to guess which rows a range covers";
        return false;
    }
    const auto want_first = planned.partial() ? planned.skip : 0U;
    const auto want_end = planned.partial() ? planned.skip + planned.take : logical;
    // The physical rows holding logical rows [want_first, want_end).
    std::uint64_t phys_first = want_first;
    std::uint64_t phys_end = want_end;
    std::vector<std::uint64_t> logical_before;  // with deletions: logical rows before each physical row
    if (!keep.empty()) {
        logical_before.resize(static_cast<std::size_t>(physical) + 1U, 0U);
        for (std::size_t r = 0; r < keep.size(); ++r) {
            logical_before[r + 1U] = logical_before[r] + keep[r];
        }
        phys_first = static_cast<std::uint64_t>(
            std::upper_bound(logical_before.begin(), logical_before.end(), want_first) - logical_before.begin() - 1);
        phys_end = want_end == 0U ? 0U
                                  : static_cast<std::uint64_t>(std::lower_bound(logical_before.begin(),
                                                                                logical_before.end(), want_end) -
                                                               logical_before.begin());
        phys_end = std::max(phys_end, phys_first);
    }
    const auto morsels = plan_morsels(columns, phys_first, phys_end);
    work_stats::add(work_stats::counters().fragment_reads, 1U);
    work_stats::add(work_stats::counters().read_morsels, morsels.size());
    const bool whole = morsels.size() == 1U && phys_first == 0U && phys_end == physical;

    // Decode: every (morsel, column) is its own task.
    const auto n_columns = columns.size();
    std::vector<ColumnValues> decoded(morsels.size() * n_columns);
    std::vector<std::string> errors(decoded.size());
    parallel::for_each(decoded.size(), [&](std::size_t t) {
        const auto& morsel = morsels[t / n_columns];
        const auto& c = columns[t % n_columns];
        if (whole) {
            decode_lance_physical_column(c.path, *c.on_disk, *c.metadata, decoded[t], errors[t]);
        } else {
            decode_lance_physical_column_range(c.path, *c.on_disk, *c.metadata, morsel.first,
                                               morsel.second - morsel.first, c.value_bytes, decoded[t], errors[t]);
        }
    });
    for (const auto& e : errors) {
        if (!e.empty()) {
            error = e;
            return false;
        }
    }

    // Each morsel: its deletions, its part of the range, its batch.
    std::vector<ArrowArray> batches(morsels.size(), ArrowArray{});
    std::vector<std::uint8_t> built(morsels.size(), 0U);
    std::vector<std::string> batch_errors(morsels.size());
    parallel::for_each(morsels.size(), [&](std::size_t k) {
        auto& why = batch_errors[k];
        const auto [p0, p1] = morsels[k];
        std::uint64_t rows = p1 - p0;
        std::uint64_t logical_first = p0;
        std::unordered_map<std::int32_t, ColumnValues> by_field;
        for (std::size_t c = 0; c < n_columns; ++c) {
            by_field.emplace(columns[c].field_id, std::move(decoded[k * n_columns + c]));
        }
        if (!keep.empty()) {
            const std::vector<std::uint8_t> part(keep.begin() + static_cast<std::ptrdiff_t>(p0),
                                                 keep.begin() + static_cast<std::ptrdiff_t>(p1));
            for (std::size_t c = 0; c < n_columns; ++c) {
                if (!compact_column_values(by_field[columns[c].field_id], part, p1 - p0, columns[c].value_bytes,
                                           why)) {
                    return;
                }
            }
            logical_first = logical_before[p0];
            rows = logical_before[p1] - logical_first;
        }
        const auto lo = std::max(logical_first, want_first);
        const auto hi = std::min(logical_first + rows, want_end);
        if (hi <= lo && !(morsels.size() == 1U)) {
            return;  // nothing of the range here
        }
        const auto take = hi > lo ? hi - lo : 0U;
        if (lo != logical_first || take != rows) {
            for (std::size_t c = 0; c < n_columns; ++c) {
                if (!slice_column_values(by_field[columns[c].field_id], lo - logical_first, take, rows,
                                         columns[c].value_bytes, why)) {
                    return;
                }
            }
        }
        for (const auto* field : missing) {
            if (!null_column_values(*field, take, by_field[field->id], why)) {
                return;
            }
        }
        std::vector<std::uint64_t> physical_rows;
        if (ids.any()) {
            // The physical rows of this batch: the morsel's rows that survive deletion, then the range.
            physical_rows.reserve(static_cast<std::size_t>(take));
            std::uint64_t logical_row = logical_first;
            for (std::uint64_t p = p0; p < p1 && physical_rows.size() < take; ++p) {
                if (!keep.empty() && keep[p] == 0U) {
                    continue;
                }
                if (logical_row++ >= lo) {
                    physical_rows.push_back(p);
                }
            }
        }
        std::uint64_t out_rows = take;
        if (filter.expr != nullptr) {
            // Evaluate on copies of the filter's columns (building a batch consumes its inputs), then
            // drop the rows that do not pass from every column.
            std::unordered_map<std::int32_t, ColumnValues> copies;
            for (const auto id : *filter.ids) {
                const auto it = by_field.find(id);
                if (it != by_field.end()) {
                    copies.emplace(id, it->second);
                }
            }
            std::vector<std::uint8_t> pass;
            if (filter.schema->n_children == 0) {
                ArrowArray empty{};
                if (ArrowArrayInitFromSchema(&empty, filter.schema, nullptr) != NANOARROW_OK) {
                    why = "failed to build the filter batch";
                    return;
                }
                empty.length = static_cast<std::int64_t>(take);
                const bool ok = filter.expr->filter(empty, pass, why);
                ArrowArrayRelease(&empty);
                if (!ok) {
                    return;
                }
            } else {
                ArrowArray probe{};
                if (!build_batch_from_schema(*filter.schema, mapping, copies, static_cast<std::int64_t>(take), probe,
                                             why)) {
                    return;
                }
                const bool ok = filter.expr->filter(probe, pass, why);
                ArrowArrayRelease(&probe);
                if (!ok) {
                    return;
                }
            }
            out_rows = static_cast<std::uint64_t>(std::count(pass.begin(), pass.end(), 1U));
            if (out_rows != take) {
                for (std::size_t c = 0; c < n_columns; ++c) {
                    if (!compact_column_values(by_field[columns[c].field_id], pass, take, columns[c].value_bytes,
                                               why)) {
                        return;
                    }
                }
                for (const auto* field : missing) {
                    if (!null_column_values(*field, out_rows, by_field[field->id], why)) {
                        return;
                    }
                }
                if (ids.any()) {
                    std::vector<std::uint64_t> kept;
                    kept.reserve(static_cast<std::size_t>(out_rows));
                    for (std::size_t r = 0; r < physical_rows.size(); ++r) {
                        if (pass[r] != 0U) {
                            kept.push_back(physical_rows[r]);
                        }
                    }
                    physical_rows = std::move(kept);
                }
            }
            if (out_rows == 0U) {
                return;  // nothing of this morsel passes
            }
        }
        if (batch_schema.n_children != 0 &&
            !build_batch_from_schema(batch_schema, mapping, by_field, static_cast<std::int64_t>(out_rows), batches[k],
                                     why)) {
            return;
        }
        if (ids.any() && !add_row_id_columns(batches[k], static_cast<std::int64_t>(out_rows), planned.fragment_id,
                                             physical_rows, ids, why)) {
            return;
        }
        built[k] = 1U;
    });
    bool ok = true;
    for (std::size_t k = 0; k < morsels.size(); ++k) {
        if (!batch_errors[k].empty() && ok) {
            error = batch_errors[k];
            ok = false;
        }
    }
    for (std::size_t k = 0; k < morsels.size(); ++k) {
        if (built[k] == 0U) {
            continue;
        }
        if (ok) {
            out.push_back(batches[k]);
        } else if (batches[k].release != nullptr) {
            ArrowArrayRelease(&batches[k]);
        }
    }
    return ok;
}

/// A data file's parsed footer, descriptor and column metadata, kept for take(): a shuffled epoch calls
/// take once per mini-batch, and re-reading and re-parsing a file's page table every time (4,890 pages
/// for the Speech Commands waveforms) cost more than decoding the rows. Keyed by path, file size and
/// modification time -- Lance never rewrites a data file in place, a new version writes new files --
/// and bounded, so a long-running loader over many datasets does not grow without limit.
struct CachedFileMetadata {
    pb::FileDescriptor descriptor;
    LanceDataFileFooterLayout layout{};
    std::vector<pb::ColumnMetadata> columns;
};

bool cached_file_metadata(const std::filesystem::path& path, std::shared_ptr<const CachedFileMetadata>& out,
                          std::string& error) {
    struct Entry {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type mtime{};
        std::shared_ptr<const CachedFileMetadata> metadata;
    };
    static std::mutex mutex;
    static std::unordered_map<std::string, Entry> cache;
    constexpr std::size_t kMaxFiles = 256U;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    const auto mtime = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, ec);
    const auto key = path.string();
    if (!ec) {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(key);
        if (it != cache.end() && it->second.size == size && it->second.mtime == mtime) {
            out = it->second.metadata;
            return true;
        }
    }
    auto fresh = std::make_shared<CachedFileMetadata>();
    if (!read_lance_data_file_footer_and_descriptor(path, fresh->descriptor, fresh->layout, error) ||
        !read_lance_data_file_column_metadatas(path, fresh->layout, fresh->columns, error)) {
        return false;
    }
    out = fresh;
    if (!ec) {
        const std::lock_guard<std::mutex> lock(mutex);
        if (cache.size() >= kMaxFiles) {
            cache.clear();
        }
        cache[key] = Entry{size, mtime, fresh};
    }
    return true;
}

/// One fragment's share of a take: `logical` are its requested rows, ascending, fragment-local and
/// counted without the deleted ones. Deleted rows are mapped out first, then every projected column
/// decodes just those physical rows (decode_lance_physical_column_rows).
/// The physical offsets (deleted rows counted) of a fragment's `logical` rows (ascending).
bool physical_rows_of(const std::filesystem::path& dataset_path, const PlannedFile& planned,
                      const std::vector<std::uint64_t>& logical, std::vector<std::uint64_t>& physical,
                      std::string& error) {
    physical.clear();
    physical.reserve(logical.size());
    if (!planned.deletion_file.present) {
        physical = logical;
        return true;
    }
    std::vector<std::uint32_t> deleted;
    if (!read_deletion_vector(dataset_path, planned.fragment_id, planned.deletion_file, deleted, error)) {
        return false;
    }
    std::sort(deleted.begin(), deleted.end());
    std::size_t d = 0;
    std::uint64_t next_logical = 0;
    std::size_t want = 0;
    for (std::uint64_t row = 0; row < planned.physical_rows && want < logical.size(); ++row) {
        while (d < deleted.size() && deleted[d] < row) {
            ++d;
        }
        if (d < deleted.size() && deleted[d] == row) {
            continue;
        }
        if (next_logical == logical[want]) {
            physical.push_back(row);
            ++want;
        }
        ++next_logical;
    }
    if (physical.size() != logical.size()) {
        error = "fragment " + std::to_string(planned.fragment_id) + " has fewer rows than the take asks for";
        return false;
    }
    return true;
}

/// Decode the rows at `physical` (offsets in the fragment's data files, ascending) into one batch.
bool take_from_data_file(const std::filesystem::path& dataset_path, const PlannedFile& planned,
                         const LanceSchemaMapping& mapping, const ArrowSchema& batch_schema,
                         const std::vector<std::uint64_t>& physical, ArrowArray& batch, std::string& error,
                         const std::unordered_set<std::int32_t>* allowed_field_ids) {
    if (planned.files.empty()) {
        error = "fragment has no data files";
        return false;
    }
    const DataFileReadScope read_scope;

    // The columns to take, then taken side by side (each is its own file reads and decode).
    std::vector<std::shared_ptr<const CachedFileMetadata>> files;
    std::vector<ColumnSource> columns;
    for (const auto& data_file : planned.files) {
        const auto jailed = safe_join_under(planned.data_dir.empty() ? dataset_path / "data" : planned.data_dir,
                                            data_file.path);
        if (!jailed) {
            error = "data file path escapes the dataset directory";
            return false;
        }
        const auto& path = *jailed;
        std::shared_ptr<const CachedFileMetadata> file;
        if (!cached_file_metadata(path, file, error)) {
            return false;
        }
        files.push_back(file);
        const auto& descriptor = file->descriptor;
        const auto& column_metadatas = file->columns;
        if (descriptor.length != planned.physical_rows) {
            error = "data file holds " + std::to_string(descriptor.length) + " rows but the manifest claims " +
                    std::to_string(planned.physical_rows);
            return false;
        }
        if (data_file.fields.size() != data_file.column_indices.size()) {
            error = "data file field/column index mismatch";
            return false;
        }
        for (std::size_t i = 0; i < data_file.fields.size(); ++i) {
            const auto field_id = data_file.fields[i];
            if (allowed_field_ids && !allowed_field_ids->count(field_id)) continue;
            if (find_mapping_field(mapping, field_id) == nullptr) continue;
            const auto column_index = data_file.column_indices[i];
            if (column_index < 0 || static_cast<std::size_t>(column_index) >= column_metadatas.size()) {
                error = "data file column index out of range";
                return false;
            }
            const auto* on_disk = find_descriptor_field(descriptor, field_id);
            if (on_disk == nullptr) {
                error = "data file references unknown field id";
                return false;
            }
            const auto* field = find_mapping_field(mapping, field_id);
            ColumnSource source;
            source.path = path;
            source.on_disk = on_disk;
            source.metadata = &column_metadatas[static_cast<std::size_t>(column_index)];
            source.field_id = field_id;
            source.value_bytes = field == nullptr ? 0U : lance_logical_type_value_bytes(field->logical_type);
            columns.push_back(std::move(source));
        }
    }
    std::vector<ColumnValues> decoded(columns.size());
    std::vector<std::string> errors(columns.size());
    parallel::for_each(columns.size(), [&](std::size_t c) {
        const auto& source = columns[c];
        decode_lance_physical_column_rows(source.path, *source.on_disk, *source.metadata, physical,
                                          source.value_bytes, decoded[c], errors[c]);
    });
    std::unordered_map<std::int32_t, ColumnValues> decoded_by_field_id;
    for (std::size_t c = 0; c < columns.size(); ++c) {
        if (!errors[c].empty()) {
            error = errors[c];
            return false;
        }
        decoded_by_field_id.emplace(columns[c].field_id, std::move(decoded[c]));
    }
    for (const auto* field : fields_missing_from(planned, mapping, allowed_field_ids)) {
        if (!null_column_values(*field, physical.size(), decoded_by_field_id[field->id], error)) {
            return false;
        }
    }
    return build_batch_from_schema(batch_schema, mapping, decoded_by_field_id,
                                   static_cast<std::int64_t>(physical.size()), batch, error);
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
    RowIdColumns ids;
    /// With row id columns: the schema of the data columns alone, which batches are built against
    /// before the row ids are added. Null otherwise (the output schema is the data schema).
    std::shared_ptr<ArrowSchema> data_schema;
    /// A filter: the expression (bound to filter_schema, the struct of the columns it reads) and the
    /// field ids of those columns. With a filter, the request's row range applies to the rows that
    /// pass (post_range), not to the dataset's.
    std::shared_ptr<expr::Expression> filter;
    std::shared_ptr<ArrowSchema> filter_schema;
    std::vector<std::int32_t> filter_ids;
    bool has_post_range = false;
    LanceRowRange post_range;

    const std::unordered_set<std::int32_t>* allowed() const { return projected ? &allowed_ids : nullptr; }
    FilterSpec filter_spec() const {
        return FilterSpec{filter.get(), filter_schema.get(), &filter_ids};
    }
    const ArrowSchema& batch_schema(const ArrowSchema& out_schema) const {
        return data_schema ? *data_schema : out_schema;
    }
};

bool load_request_manifest(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                           pb::Manifest& manifest, std::string& error) {
    if (request.has_version) {
        return load_manifest_version(dataset_path, request.version, manifest, error);
    }
    std::uint64_t version = 0;
    return load_latest_manifest(dataset_path, manifest, version, error);
}

bool open_read_plan_from_manifest(const std::filesystem::path& dataset_path, pb::Manifest& manifest,
                                  const LanceScanRequest& request, ReadPlan& plan, ArrowSchema& out_schema,
                                  std::string& error);

/// Parse the manifest and build the Arrow schema. `column_names` null means every column.
bool open_read_plan(const std::filesystem::path& dataset_path, const LanceScanRequest& request, ReadPlan& plan,
                    ArrowSchema& out_schema, std::string& error) {
    pb::Manifest manifest{};
    if (!load_request_manifest(dataset_path, request, manifest, error)) {
        release_schema_if_held(out_schema);
        return false;
    }
    return open_read_plan_from_manifest(dataset_path, manifest, request, plan, out_schema, error);
}

bool open_read_plan_from_manifest(const std::filesystem::path& dataset_path, pb::Manifest& manifest,
                                  const LanceScanRequest& request, ReadPlan& plan, ArrowSchema& out_schema,
                                  std::string& error) {
    plan.dataset_path = dataset_path;
    const auto* column_names = request.columns;
    const auto& range = request.range;
    plan.ids.row_id = request.with_row_id;
    plan.ids.row_address = request.with_row_address;
    if (column_names != nullptr && column_names->empty() && !plan.ids.any()) {
        release_schema_if_held(out_schema);
        error = "a read must name at least one column";
        return false;
    }
    if (plan.ids.row_id && (manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U) {
        release_schema_if_held(out_schema);
        error = "row ids of a dataset with stable row ids are not supported";
        return false;
    }
    LanceSchemaMapping full_mapping;
    if (!lance_schema_mapping_from_manifest(manifest, full_mapping, error)) {
        release_schema_if_held(out_schema);
        return false;
    }

    // A top-level column and ALL its descendants: a struct column is only meaningful together with
    // the children that hold its data.
    auto add_subtree = [&](std::int32_t root, std::unordered_set<std::int32_t>& into) {
        std::vector<std::int32_t> queue = {root};
        while (!queue.empty()) {
            const auto id = queue.back();
            queue.pop_back();
            into.insert(id);
            for (const auto& f : full_mapping.fields) {
                if (f.parent_id == id) {
                    queue.push_back(f.id);
                }
            }
        }
    };
    auto find_root = [&](const std::string& name, bool fold) -> const LanceField* {
        for (const auto& f : full_mapping.fields) {
            if (f.parent_id != -1) {
                continue;
            }
            if (f.name == name) {
                return &f;
            }
        }
        if (fold) {
            const LanceField* found = nullptr;
            for (const auto& f : full_mapping.fields) {
                if (f.parent_id == -1 && f.name.size() == name.size() &&
                    std::equal(f.name.begin(), f.name.end(), name.begin(), [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                    })) {
                    if (found != nullptr) {
                        return nullptr;
                    }
                    found = &f;
                }
            }
            return found;
        }
        return nullptr;
    };

    std::unordered_set<std::int32_t> output_ids;
    if (column_names != nullptr) {
        for (const auto& col_name : *column_names) {
            const LanceField* root = find_root(col_name, false);
            if (root == nullptr) {
                release_schema_if_held(out_schema);
                error = "projected column '" + col_name + "' not found in schema";
                return false;
            }
            add_subtree(root->id, output_ids);
        }
    }

    // The filter, and the columns it reads: decoded too, even when not returned.
    std::unordered_set<std::int32_t> filter_ids;
    if (request.filter != nullptr && !request.filter->empty()) {
        auto parsed = std::make_shared<expr::Expression>();
        if (!expr::Expression::parse(*request.filter, *parsed, error)) {
            release_schema_if_held(out_schema);
            return false;
        }
        for (const auto& name : parsed->columns()) {
            const LanceField* root = find_root(name, true);
            if (root == nullptr && name.find('.') != std::string::npos) {
                root = find_root(name.substr(0, name.find('.')), true);
            }
            if (root == nullptr) {
                release_schema_if_held(out_schema);
                error = "filter column '" + name + "' not found in schema";
                return false;
            }
            add_subtree(root->id, filter_ids);
        }
        plan.filter = std::move(parsed);
    }

    LanceSchemaMapping output_mapping;
    if (column_names == nullptr) {
        output_mapping = full_mapping;
        plan.mapping = std::move(full_mapping);
    } else {
        plan.projected = true;
        plan.allowed_ids = output_ids;
        plan.allowed_ids.insert(filter_ids.begin(), filter_ids.end());
        for (const auto& f : full_mapping.fields) {
            if (plan.allowed_ids.count(f.id) != 0U) {
                plan.mapping.fields.push_back(f);
            }
            if (output_ids.count(f.id) != 0U) {
                output_mapping.fields.push_back(f);
            }
        }
    }
    if (plan.filter) {
        LanceSchemaMapping filter_mapping;
        for (const auto& f : plan.mapping.fields) {
            if (filter_ids.count(f.id) != 0U) {
                filter_mapping.fields.push_back(f);
                plan.filter_ids.push_back(f.id);
            }
        }
        plan.filter_schema = std::shared_ptr<ArrowSchema>(new ArrowSchema{}, [](ArrowSchema* schema) {
            release_schema_if_held(*schema);
            delete schema;
        });
        ArrowSchemaInit(plan.filter_schema.get());
        if (filter_mapping.fields.empty()) {
            if (ArrowSchemaSetTypeStruct(plan.filter_schema.get(), 0) != NANOARROW_OK) {
                release_schema_if_held(out_schema);
                error = "failed to build the filter schema";
                return false;
            }
        } else if (!build_schema_from_mapping(filter_mapping, *plan.filter_schema, error)) {
            release_schema_if_held(out_schema);
            return false;
        }
        if (!plan.filter->bind(*plan.filter_schema, error)) {
            release_schema_if_held(out_schema);
            return false;
        }
    }

    if (output_mapping.fields.empty()) {
        ArrowSchemaInit(&out_schema);
        if (ArrowSchemaSetTypeStruct(&out_schema, 0) != NANOARROW_OK) {
            release_schema_if_held(out_schema);
            error = "failed to build an empty schema";
            return false;
        }
    } else if (!build_schema_from_mapping(output_mapping, out_schema, error)) {
        release_schema_if_held(out_schema);
        return false;
    }
    if (!set_dataset_schema_metadata(out_schema, manifest, error)) {
        release_schema_if_held(out_schema);
        return false;
    }
    if (plan.ids.any()) {
        plan.data_schema = std::shared_ptr<ArrowSchema>(new ArrowSchema{}, [](ArrowSchema* schema) {
            release_schema_if_held(*schema);
            delete schema;
        });
        if (ArrowSchemaDeepCopy(&out_schema, plan.data_schema.get()) != NANOARROW_OK ||
            !add_row_id_fields(out_schema, plan.ids, error)) {
            release_schema_if_held(out_schema);
            if (error.empty()) {
                error = "failed to copy the schema";
            }
            return false;
        }
    }

    std::vector<pb::DataFragment> fragments;
    if (request.fragment_ids != nullptr) {
        // The fragments asked for, in the order asked.
        for (const auto id : *request.fragment_ids) {
            const auto it = std::find_if(manifest.fragments.begin(), manifest.fragments.end(),
                                         [&](const pb::DataFragment& f) { return f.id == id; });
            if (it == manifest.fragments.end()) {
                release_schema_if_held(out_schema);
                error = "fragment " + std::to_string(id) + " not found";
                return false;
            }
            fragments.push_back(*it);
        }
    } else {
        fragments = manifest.fragments;
        std::sort(fragments.begin(), fragments.end(),
                  [](const pb::DataFragment& a, const pb::DataFragment& b) { return a.id < b.id; });
    }

    if (request.include_deleted_rows) {
        for (auto& fragment : fragments) {
            fragment.deletion_file = pb::DeletionFile{};
        }
    }
    // A filtered read decodes whole fragments; its range counts the rows that pass the filter.
    if (plan.filter && !range.is_whole_dataset()) {
        plan.has_post_range = true;
        plan.post_range = range;
    }
    const bool ranged = !plan.filter && !range.is_whole_dataset();
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

/// Decode every data file up front. The eager reads' second half. Fragments are decoded side by
/// side when there are threads to spare (each may itself split into morsels); the batches come back
/// in fragment order either way.
bool read_all_batches(const ReadPlan& plan, ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches,
                      std::string& error) {
    std::vector<std::vector<ArrowArray>> per_file(plan.files.size());
    std::vector<std::string> errors(plan.files.size());
    parallel::for_each(plan.files.size(), [&](std::size_t f) {
        read_data_file_batches(plan.dataset_path, plan.files[f], plan.mapping, plan.batch_schema(out_schema),
                               per_file[f], errors[f], plan.allowed(), plan.ids, plan.filter_spec());
    });
    for (std::size_t f = 0; f < per_file.size(); ++f) {
        if (!errors[f].empty() && error.empty()) {
            error = errors[f];
        }
    }
    for (auto& batches : per_file) {
        for (auto& batch : batches) {
            out_batches.push_back(batch);
        }
    }
    if (!error.empty()) {
        release_partial_read(out_schema, out_batches);
        return false;
    }
    if (plan.has_post_range) {
        const auto& r = plan.post_range;
        slice_batches(out_batches, r.offset,
                      r.length == LanceRowRange::kAllRows ? -1 : static_cast<std::int64_t>(r.length));
    }
    return true;
}

bool read_dataset_eager(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
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
    if (!open_read_plan(dataset_path, request, plan, out_schema, error)) {
        return false;
    }
    return read_all_batches(plan, out_schema, out_batches, error);
}

bool read_dataset_eager(const std::filesystem::path& dataset_path,
                        const std::vector<std::string>* column_names, const LanceRowRange& range,
                        ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches,
                        std::string& error, bool trusted_input) {
    LanceScanRequest request;
    request.columns = column_names;
    request.range = range;
    return read_dataset_eager(dataset_path, request, out_schema, out_batches, error, trusted_input);
}

/// Take rows given as physical offsets per fragment (`by_fragment`, ascending), one batch per fragment.
bool take_physical(const ReadPlan& plan, ArrowSchema& out_schema,
                   const std::vector<std::pair<const PlannedFile*, std::vector<std::uint64_t>>>& by_fragment,
                   std::vector<ArrowArray>& out_batches, std::string& error);

}  // namespace

bool lance_dataset_scan(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                        ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches, std::string& error,
                        bool trusted_input) {
    return read_dataset_eager(dataset_path, request, out_schema, out_batches, error, trusted_input);
}

namespace {

/// A standalone data file, as a one-fragment dataset: its own schema (the file's descriptor) with its
/// leaf fields numbered as the file's columns, and the file's directory as the data directory.
bool open_file_plan(const std::filesystem::path& file_path, const LanceScanRequest& request, ReadPlan& plan,
                    ArrowSchema& out_schema, std::string& error, LanceFileInfo* info = nullptr) {
    pb::FileDescriptor descriptor;
    LanceDataFileFooterLayout layout{};
    if (!read_lance_data_file_footer_and_descriptor(file_path, descriptor, layout, error)) {
        release_schema_if_held(out_schema);
        return false;
    }
    pb::Manifest manifest;
    manifest.fields = descriptor.fields;
    pb::DataFile file;
    file.path = file_path.filename().string();
    std::unordered_set<std::int32_t> parents;
    for (const auto& f : descriptor.fields) {
        if (f.parent_id >= 0) {
            parents.insert(f.parent_id);
        }
    }
    std::int32_t column = 0;
    for (const auto& f : descriptor.fields) {
        if (parents.count(f.id) == 0U) {
            file.fields.push_back(f.id);
            file.column_indices.push_back(column++);
        }
    }
    if (static_cast<std::uint32_t>(column) != layout.num_columns) {
        release_schema_if_held(out_schema);
        error = "the file has " + std::to_string(layout.num_columns) + " columns for " + std::to_string(column) +
                " leaf fields; nanolance reads files whose every leaf field is one column";
        return false;
    }
    pb::DataFragment fragment;
    fragment.physical_rows = descriptor.length;
    fragment.files.push_back(file);
    manifest.fragments.push_back(fragment);
    if (info != nullptr) {
        info->num_rows = descriptor.length;
        info->num_columns = layout.num_columns;
        std::vector<pb::ColumnMetadata> columns;
        if (!read_lance_data_file_column_metadatas(file_path, layout, columns, error)) {
            release_schema_if_held(out_schema);
            return false;
        }
        info->pages.clear();
        for (const auto& c : columns) {
            auto& pages = info->pages.emplace_back();
            for (const auto& p : c.pages) {
                LanceFileInfo::Page page;
                page.rows = p.length;
                page_layout::PageLayout layout_of_page;
                std::string ignored;
                page.encoding = page_layout::decode_page_layout(p.encoding, layout_of_page, ignored)
                                    ? page_layout::describe(layout_of_page)
                                    : std::string("unknown");
                for (std::size_t b = 0; b < p.buffer_offsets.size() && b < p.buffer_sizes.size(); ++b) {
                    page.buffers.emplace_back(p.buffer_offsets[b], p.buffer_sizes[b]);
                }
                pages.push_back(std::move(page));
            }
        }
    }
    if (!open_read_plan_from_manifest(file_path.parent_path(), manifest, request, plan, out_schema, error)) {
        return false;
    }
    // A bare file name ("x.lance") has an empty parent, and an empty data_dir means <dataset>/data:
    // name the current directory instead.
    const auto dir = file_path.parent_path().empty() ? std::filesystem::path(".") : file_path.parent_path();
    for (auto& planned : plan.files) {
        planned.data_dir = dir;
    }
    return true;
}

}  // namespace

bool lance_file_read(const std::filesystem::path& file_path, const LanceScanRequest& request,
                     ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches, std::string& error) {
    error.clear();
    out_batches.clear();
    ArrowSchemaInit(&out_schema);
    ReadPlan plan;
    if (!open_file_plan(file_path, request, plan, out_schema, error)) {
        return false;
    }
    return read_all_batches(plan, out_schema, out_batches, error);
}

bool lance_file_take(const std::filesystem::path& file_path, const LanceScanRequest& request,
                     const std::vector<std::uint64_t>& rows, ArrowSchema& out_schema,
                     std::vector<ArrowArray>& out_batches, std::string& error) {
    error.clear();
    out_batches.clear();
    ArrowSchemaInit(&out_schema);
    ReadPlan plan;
    LanceScanRequest whole = request;
    whole.range = LanceRowRange{};
    if (!open_file_plan(file_path, whole, plan, out_schema, error)) {
        return false;
    }
    std::vector<std::uint64_t> wanted(rows);
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    if (plan.files.empty()) {
        if (!wanted.empty()) {
            release_schema_if_held(out_schema);
            error = "row " + std::to_string(wanted.front()) + " is past the end of the file (0 rows)";
            return false;
        }
        return true;
    }
    if (!wanted.empty() && wanted.back() >= plan.files.front().physical_rows) {
        release_schema_if_held(out_schema);
        error = "row " + std::to_string(wanted.back()) + " is past the end of the file (" +
                std::to_string(plan.files.front().physical_rows) + " rows)";
        return false;
    }
    std::vector<std::pair<const PlannedFile*, std::vector<std::uint64_t>>> by_fragment;
    if (!wanted.empty()) {
        by_fragment.emplace_back(&plan.files.front(), wanted);
    }
    return take_physical(plan, out_schema, by_fragment, out_batches, error);
}

bool lance_file_info(const std::filesystem::path& file_path, LanceFileInfo& info, ArrowSchema& out_schema,
                     std::string& error) {
    error.clear();
    ArrowSchemaInit(&out_schema);
    ReadPlan plan;
    return open_file_plan(file_path, LanceScanRequest{}, plan, out_schema, error, &info);
}

namespace {

bool take_physical(const ReadPlan& plan, ArrowSchema& out_schema,
                   const std::vector<std::pair<const PlannedFile*, std::vector<std::uint64_t>>>& by_fragment,
                   std::vector<ArrowArray>& out_batches, std::string& error) {
    for (const auto& [planned, physical] : by_fragment) {
        ArrowArray batch{};
        if (!plan.mapping.fields.empty() &&
            !take_from_data_file(plan.dataset_path, *planned, plan.mapping, plan.batch_schema(out_schema), physical,
                                 batch, error, plan.allowed())) {
            release_partial_read(out_schema, out_batches);
            return false;
        }
        if (plan.ids.any() && !add_row_id_columns(batch, static_cast<std::int64_t>(physical.size()),
                                                  planned->fragment_id, physical, plan.ids, error)) {
            release_partial_read(out_schema, out_batches);
            return false;
        }
        out_batches.push_back(batch);
    }
    return true;
}

bool open_take(const std::filesystem::path& dataset_path, const LanceScanRequest& request, ReadPlan& plan,
               ArrowSchema& out_schema, std::string& error,
               std::optional<ScopedReadLimits>& trusted_scope, bool trusted_input) {
    error.clear();
    ArrowSchemaInit(&out_schema);
    if (trusted_input) {
        trusted_scope.emplace(trusted_read_limits());
    }
    LanceScanRequest whole = request;
    whole.range = LanceRowRange{};
    return open_read_plan(dataset_path, whole, plan, out_schema, error);
}

}  // namespace

bool lance_dataset_take(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                        const std::vector<std::uint64_t>& indices, ArrowSchema& out_schema,
                        std::vector<ArrowArray>& out_batches, std::string& error, bool trusted_input) {
    out_batches.clear();
    std::optional<ScopedReadLimits> trusted_scope;
    ReadPlan plan;
    if (!open_take(dataset_path, request, plan, out_schema, error, trusted_scope, trusted_input)) {
        return false;
    }
    std::vector<std::uint64_t> wanted(indices);
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    std::uint64_t total = 0;
    for (const auto& planned : plan.files) {
        total += planned.rows;
    }
    if (!wanted.empty() && wanted.back() >= total) {
        release_schema_if_held(out_schema);
        error = "row " + std::to_string(wanted.back()) + " is past the end of the dataset (" + std::to_string(total) +
                " rows)";
        return false;
    }
    std::vector<std::pair<const PlannedFile*, std::vector<std::uint64_t>>> by_fragment;
    std::uint64_t cursor = 0;
    std::size_t w = 0;
    for (const auto& planned : plan.files) {
        const auto first = cursor;
        cursor += planned.rows;
        std::vector<std::uint64_t> logical;
        while (w < wanted.size() && wanted[w] < cursor) {
            logical.push_back(wanted[w++] - first);
        }
        if (logical.empty()) {
            continue;
        }
        std::vector<std::uint64_t> physical;
        if (!physical_rows_of(plan.dataset_path, planned, logical, physical, error)) {
            release_schema_if_held(out_schema);
            return false;
        }
        by_fragment.emplace_back(&planned, std::move(physical));
    }
    return take_physical(plan, out_schema, by_fragment, out_batches, error);
}

bool lance_dataset_take_rows(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                             const std::vector<std::uint64_t>& addresses, ArrowSchema& out_schema,
                             std::vector<ArrowArray>& out_batches, std::string& error, bool trusted_input) {
    out_batches.clear();
    std::optional<ScopedReadLimits> trusted_scope;
    ReadPlan plan;
    if (!open_take(dataset_path, request, plan, out_schema, error, trusted_scope, trusted_input)) {
        return false;
    }
    std::vector<std::uint64_t> wanted(addresses);
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    std::map<std::uint64_t, const PlannedFile*> fragments;
    for (const auto& planned : plan.files) {
        fragments.emplace(planned.fragment_id, &planned);
    }
    std::vector<std::pair<const PlannedFile*, std::vector<std::uint64_t>>> by_fragment;
    for (const auto address : wanted) {
        const auto fragment_id = address >> 32U;
        const auto offset = address & 0xFFFFFFFFULL;
        const auto it = fragments.find(fragment_id);
        if (it == fragments.end() || offset >= it->second->physical_rows) {
            release_schema_if_held(out_schema);
            error = "row address " + std::to_string(address) + " (fragment " + std::to_string(fragment_id) +
                    ", row " + std::to_string(offset) + ") is not in the dataset";
            return false;
        }
        if (by_fragment.empty() || by_fragment.back().first != it->second) {
            by_fragment.emplace_back(it->second, std::vector<std::uint64_t>{});
        }
        by_fragment.back().second.push_back(offset);
    }
    return take_physical(plan, out_schema, by_fragment, out_batches, error);
}

bool lance_table_take(const std::filesystem::path& dataset_path, const std::vector<std::string>* column_names,
                      const std::vector<std::uint64_t>& indices, ArrowSchema& out_schema,
                      std::vector<ArrowArray>& out_batches, std::string& error, bool trusted_input) {
    LanceScanRequest request;
    request.columns = column_names;
    return lance_dataset_take(dataset_path, request, indices, out_schema, out_batches, error, trusted_input);
}

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
    std::deque<ArrowArray> pending;  // the current fragment's batches not yet handed out
    // A filtered read's row range, counted over the rows that pass: rows still to skip, and to return.
    std::uint64_t skip = 0;
    std::uint64_t remaining = LanceRowRange::kAllRows;

    ~Impl() {
        for (auto& batch : pending) {
            if (batch.release != nullptr) {
                ArrowArrayRelease(&batch);
            }
        }
        release_schema_if_held(schema);
    }
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
    LanceScanRequest request;
    request.columns = column_names;
    request.range = range;
    return open_request(dataset_path, request, out_schema, out, error, trusted_input);
}

bool LanceTableStream::open_request(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                                    ArrowSchema& out_schema, LanceTableStream& out, std::string& error,
                                    bool trusted_input) {
    error.clear();
    ArrowSchemaInit(&out_schema);

    std::optional<ScopedReadLimits> trusted_scope;
    if (trusted_input) {
        trusted_scope.emplace(trusted_read_limits());
    }

    auto impl = std::make_unique<Impl>();
    impl->trusted_input = trusted_input;
    if (!open_read_plan(dataset_path, request, impl->plan, out_schema, error)) {
        return false;
    }
    // The stream keeps its own schema: read_data_file_batch builds each batch against one, and the
    // caller owns (and may release) the schema it was handed the moment open() returns.
    if (ArrowSchemaDeepCopy(&impl->plan.batch_schema(out_schema), &impl->schema) != NANOARROW_OK) {
        release_schema_if_held(out_schema);
        error = "failed to copy the dataset schema for the stream";
        return false;
    }
    if (impl->plan.has_post_range) {
        impl->skip = impl->plan.post_range.offset;
        impl->remaining = impl->plan.post_range.length;
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
    // A fragment may come back as several batches (one per morsel of a parallel read).
    while (impl_->pending.empty()) {
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
        std::vector<ArrowArray> batches;
        if (!read_data_file_batches(impl_->plan.dataset_path, planned, impl_->plan.mapping, impl_->schema, batches,
                                    error, impl_->plan.allowed(), impl_->plan.ids, impl_->plan.filter_spec())) {
            out_batch = ArrowArray{};
            return false;
        }
        ++impl_->cursor;
        impl_->pending.insert(impl_->pending.end(), batches.begin(), batches.end());
    }
    out_batch = impl_->pending.front();
    impl_->pending.pop_front();
    if (impl_->plan.has_post_range) {
        // Skip and cut to the range, then stop reading once it is complete.
        auto rows = static_cast<std::uint64_t>(out_batch.length);
        if (impl_->remaining == 0U || impl_->skip >= rows) {
            impl_->skip -= std::min(impl_->skip, rows);
            ArrowArrayRelease(&out_batch);
            out_batch = ArrowArray{};
            if (impl_->remaining == 0U) {
                for (auto& b : impl_->pending) {
                    ArrowArrayRelease(&b);
                }
                impl_->pending.clear();
                impl_->cursor = impl_->plan.files.size();
                return true;
            }
            return next(out_batch, error);
        }
        const auto first = impl_->skip;
        const auto count = std::min(rows - first, impl_->remaining);
        impl_->skip = 0;
        if (impl_->remaining != LanceRowRange::kAllRows) {
            impl_->remaining -= count;
        }
        if (first != 0U || count != rows) {
            auto shared = std::make_shared<SharedBatch>(std::move(out_batch));
            out_batch = slice_batch(shared, static_cast<std::int64_t>(first), static_cast<std::int64_t>(count));
        }
    }
    return true;
}


bool lance_table_read_schema(const std::filesystem::path& dataset_path, ArrowSchema& out_schema,
                             std::string& error) {
    return lance_dataset_schema(dataset_path, LanceScanRequest{}, out_schema, error);
}

bool lance_dataset_schema(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                          ArrowSchema& out_schema, std::string& error) {
    error.clear();
    ArrowSchemaInit(&out_schema);

    pb::Manifest manifest{};
    if (!load_request_manifest(dataset_path, request, manifest, error)) {
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
    if (!set_dataset_schema_metadata(out_schema, manifest, error)) {
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
