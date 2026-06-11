#pragma once

// Host-only glue: realize the reflected column list as a nanoarrow struct schema + struct array
// (a Lance record batch). Maps the backend-neutral `arrow_kind` to nanoarrow types and appends each
// SoA column. The reflection core headers stay nanoarrow-free; only this glue pulls it in.

#include "soatins/reflect.hpp"

#include <nanoarrow/nanoarrow.h>

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>

namespace soatins {

inline ArrowType map_arrow_type(arrow_kind k) {
    switch (k) {
        case arrow_kind::i8: return NANOARROW_TYPE_INT8;
        case arrow_kind::u8: return NANOARROW_TYPE_UINT8;
        case arrow_kind::i16: return NANOARROW_TYPE_INT16;
        case arrow_kind::u16: return NANOARROW_TYPE_UINT16;
        case arrow_kind::i32: return NANOARROW_TYPE_INT32;
        case arrow_kind::u32: return NANOARROW_TYPE_UINT32;
        case arrow_kind::i64: return NANOARROW_TYPE_INT64;
        case arrow_kind::u64: return NANOARROW_TYPE_UINT64;
        case arrow_kind::f32: return NANOARROW_TYPE_FLOAT;
        case arrow_kind::f64: return NANOARROW_TYPE_DOUBLE;
        case arrow_kind::boolean: return NANOARROW_TYPE_BOOL;
        case arrow_kind::string: return NANOARROW_TYPE_STRING;
        case arrow_kind::large_binary: return NANOARROW_TYPE_LARGE_BINARY;
        case arrow_kind::fixed_binary: return NANOARROW_TYPE_FIXED_SIZE_BINARY;
    }
    return NANOARROW_TYPE_UNINITIALIZED;
}

// Configure one already-allocated child schema for a column.
inline bool nt_set_column_schema(ArrowSchema* child, arrow_kind kind, int fixed_width, const char* name,
                                 std::string& error) {
    int rc = NANOARROW_OK;
    if (kind == arrow_kind::fixed_binary) {
        rc = ArrowSchemaSetTypeFixedSize(child, NANOARROW_TYPE_FIXED_SIZE_BINARY, fixed_width);
    } else {
        rc = ArrowSchemaSetType(child, map_arrow_type(kind));
    }
    if (rc != NANOARROW_OK) {
        error = std::string("failed to set column type for ") + name;
        return false;
    }
    if (ArrowSchemaSetName(child, name) != NANOARROW_OK) {
        error = std::string("failed to set column name ") + name;
        return false;
    }
    // Reflected columns are always populated; the writer requires non-nullable fixed-width fields.
    child->flags &= ~static_cast<int64_t>(ARROW_FLAG_NULLABLE);
    return true;
}

// Append one host-order value to its child array (dispatch by element type).
template <class Elem>
bool nt_append_value(ArrowArray* child, const Elem& v) {
    if constexpr (std::is_same_v<Elem, bool>) {
        return ArrowArrayAppendInt(child, v ? 1 : 0) == NANOARROW_OK;
    } else if constexpr (std::is_floating_point_v<Elem>) {
        return ArrowArrayAppendDouble(child, static_cast<double>(v)) == NANOARROW_OK;
    } else if constexpr (std::is_signed_v<Elem>) {
        return ArrowArrayAppendInt(child, static_cast<int64_t>(v)) == NANOARROW_OK;
    } else if constexpr (std::is_unsigned_v<Elem>) {
        return ArrowArrayAppendUInt(child, static_cast<uint64_t>(v)) == NANOARROW_OK;
    } else {
        // std::array<uint8_t,N> (fixed-size binary)
        ArrowBufferView view{};
        view.data.data = v.data();
        view.size_bytes = static_cast<int64_t>(v.size());
        return ArrowArrayAppendBytes(child, view) == NANOARROW_OK;
    }
}

// Set children [0, ncols) of an already-struct schema from T's columns. `offset` lets a caller place
// the scalar columns ahead of extra columns (e.g. a blob.v2 payload_ref) in a combined batch.
template <class T>
bool nt_fill_struct_schema(ArrowSchema* parent, std::size_t offset, std::string& error) {
    bool ok = true;
    std::string err;
    for_each_column<T>([&]<std::size_t I, class Col>() {
        if (!ok) {
            return;
        }
        ok = nt_set_column_schema(parent->children[offset + I], Col::kind, Col::fixed_width, Col::name(), err);
    });
    if (!ok) {
        error = err;
    }
    return ok;
}

// Append row `i`'s scalar columns into children [offset, offset+ncols) (no parent FinishElement).
template <class T>
bool nt_append_scalar_row(ArrowArray* parent, std::size_t offset, const soa<T>& s, std::size_t i) {
    bool ok = true;
    for_each_column<T>([&]<std::size_t I, class Col>() {
        if (!ok) {
            return;
        }
        ok = nt_append_value(parent->children[offset + I], s.template column<I>()[i]);
    });
    return ok;
}

// Standalone struct schema for T (scalar columns only).
template <class T>
bool arrow_schema(ArrowSchema& out, std::string& error) {
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetTypeStruct(&out, static_cast<int64_t>(column_count<T>)) != NANOARROW_OK) {
        error = "failed to allocate struct schema";
        return false;
    }
    if (!nt_fill_struct_schema<T>(&out, 0, error)) {
        ArrowSchemaRelease(&out);
        return false;
    }
    out.flags = 0;
    return true;
}

// Standalone record batch for T from its SoA (scalar columns only).
template <class T>
bool to_arrow(const soa<T>& s, ArrowArray& out, std::string& error) {
    ArrowSchema schema;
    if (!arrow_schema<T>(schema, error)) {
        return false;
    }
    if (ArrowArrayInitFromSchema(&out, &schema, nullptr) != NANOARROW_OK) {
        error = "failed to init struct array";
        ArrowSchemaRelease(&schema);
        return false;
    }
    ArrowSchemaRelease(&schema);
    if (ArrowArrayStartAppending(&out) != NANOARROW_OK) {
        error = "failed to start appending";
        ArrowArrayRelease(&out);
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (!nt_append_scalar_row<T>(&out, 0, s, i)) {
            error = "failed to append scalar column value";
            ArrowArrayRelease(&out);
            return false;
        }
        if (ArrowArrayFinishElement(&out) != NANOARROW_OK) {
            error = "failed to finish struct element";
            ArrowArrayRelease(&out);
            return false;
        }
    }
    if (ArrowArrayFinishBuildingDefault(&out, nullptr) != NANOARROW_OK) {
        error = "failed to finalize struct array";
        ArrowArrayRelease(&out);
        return false;
    }
    return true;
}

}  // namespace soatins
