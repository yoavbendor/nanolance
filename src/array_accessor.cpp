// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/array_accessor.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstring>

namespace nano_lance {
namespace {

const LanceField* find_field_by_id(const LanceSchemaMapping& mapping, std::int32_t id) {
    for (const auto& field : mapping.fields) {
        if (field.id == id) {
            return &field;
        }
    }
    return nullptr;
}

const ArrowArray* child_by_mapped_name(const ArrowArray& parent,
                                       const LanceSchemaMapping& mapping,
                                       std::int32_t parent_field_id,
                                       const std::string& name) {
    if (parent.n_children <= 0 || parent.children == nullptr) {
        return nullptr;
    }
    std::int64_t child_index = 0;
    for (const auto& field : mapping.fields) {
        if (field.parent_id != parent_field_id) {
            continue;
        }
        if (field.name == name) {
            if (child_index >= parent.n_children || parent.children[child_index] == nullptr) {
                return nullptr;
            }
            return parent.children[child_index];
        }
        ++child_index;
    }
    return nullptr;
}

std::int64_t top_level_batch_child_index(const LanceSchemaMapping& mapping, const LanceField& field) {
    std::int64_t index = 0;
    for (const auto& candidate : mapping.fields) {
        if (candidate.parent_id != -1) {
            continue;
        }
        if (candidate.name == field.name) {
            return index;
        }
        ++index;
    }
    return -1;
}

const ArrowArray* resolve_field_array_impl(const ArrowArray& batch,
                                           const LanceSchemaMapping& mapping,
                                           const LanceField& field) {
    if (field.parent_id < 0) {
        const auto index = top_level_batch_child_index(mapping, field);
        if (index < 0) {
            return nullptr;
        }
        if (batch.n_children > 0 && batch.children != nullptr) {
            if (index >= batch.n_children || batch.children[index] == nullptr) {
                return nullptr;
            }
            return batch.children[index];
        }
        return index == 0 ? &batch : nullptr;
    }

    const auto* parent_field = find_field_by_id(mapping, field.parent_id);
    if (parent_field == nullptr) {
        return nullptr;
    }
    const auto* parent_array = resolve_field_array_impl(batch, mapping, *parent_field);
    if (parent_array == nullptr) {
        return nullptr;
    }
    return child_by_mapped_name(*parent_array, mapping, field.parent_id, field.name);
}

bool append_fixed_width(const ArrowArray& array,
                        const LanceField& field,
                        ColumnValues& out,
                        std::string& error,
                        bool borrow = false) {
    if (array.n_buffers < 2 || array.buffers == nullptr || array.buffers[1] == nullptr) {
        error = "fixed-width array is missing values buffer for ";
        error += field.name;
        return false;
    }
    out.kind = ColumnValues::Kind::FixedWidth;
    // A borrowed column receiving another batch materializes back into the copying path first: copy
    // the borrowed view into `fixed`, drop the view, then append the new batch below as usual.
    if (out.fixed_borrowed != nullptr) {
        out.fixed.assign(out.fixed_borrowed, out.fixed_borrowed + out.fixed_borrowed_size);
        out.fixed_borrowed = nullptr;
        out.fixed_borrowed_size = 0;
    }
    // Arrow stores boolean values bit-packed (1 bit/value, LSB-first, honoring array.offset), but
    // nanolance's on-disk layout is one byte per boolean. Expand here rather than memcpy'ing
    // length bytes out of a length/8-byte buffer (which read far past the buffer and crashed).
    if (field.logical_type == "bool") {
        const auto* bits = static_cast<const std::uint8_t*>(array.buffers[1]);
        const auto base = static_cast<std::size_t>(array.offset);
        out.fixed.reserve(out.fixed.size() + static_cast<std::size_t>(array.length));
        for (std::int64_t i = 0; i < array.length; ++i) {
            const auto bit_index = base + static_cast<std::size_t>(i);
            const auto byte = bits[bit_index >> 3U];
            out.fixed.push_back(static_cast<std::uint8_t>((byte >> (bit_index & 7U)) & 1U));
        }
        return true;
    }
    // Use the shared width table so every fixed-width logical type (incl. fixed_size_binary:N) agrees
    // with the decoder; a local table here previously defaulted to 8 and over-read narrow/byte-array
    // columns.
    const std::size_t width = lance_logical_type_value_bytes(field.logical_type);
    const auto byte_count = static_cast<std::size_t>(array.length) * width;
    const auto* first = static_cast<const std::uint8_t*>(array.buffers[1]) +
                        static_cast<std::size_t>(array.offset) * width;
    // Zero-copy ingest (set_borrow_buffers): the first batch of a column records a view of the
    // caller's buffer instead of copying; the caller guarantees it outlives commit.
    if (borrow && out.fixed.empty()) {
        out.fixed_borrowed = first;
        out.fixed_borrowed_size = byte_count;
        return true;
    }
    out.fixed.insert(out.fixed.end(), first, first + byte_count);
    return true;
}

bool append_variable_width(const ArrowArray& array,
                           const LanceField& field,
                           ColumnValues& out,
                           std::string& error) {
    const bool large = field.logical_type == "large_binary" || field.logical_type == "large_utf8";
    const std::size_t offset_width = large ? 8U : 4U;
    if (array.n_buffers < 3 || array.buffers == nullptr || array.buffers[1] == nullptr ||
        array.buffers[2] == nullptr) {
        error = "variable-width array is missing offsets/data buffers for ";
        error += field.name;
        return false;
    }
    const auto expected_offsets =
        static_cast<std::size_t>(array.length + 1) * offset_width;
    const auto* offsets = static_cast<const std::uint8_t*>(array.buffers[1]);
    const auto* data = static_cast<const std::uint8_t*>(array.buffers[2]);
    std::size_t data_bytes = 0;
    if (array.length > 0) {
        if (large) {
            const auto* last =
                reinterpret_cast<const std::int64_t*>(offsets + static_cast<std::size_t>(array.length) * 8U);
            data_bytes = static_cast<std::size_t>(*last);
        } else {
            const auto* last =
                reinterpret_cast<const std::int32_t*>(offsets + static_cast<std::size_t>(array.length) * 4U);
            data_bytes = static_cast<std::size_t>(*last);
        }
    }
    out.kind = ColumnValues::Kind::VariableWidth;
    out.variable.large = large;

    if (out.variable.offsets.empty()) {
        out.variable.offsets.assign(offsets, offsets + expected_offsets);
        out.variable.data.assign(data, data + data_bytes);
        return true;
    }

    const std::size_t base = out.variable.data.size();
    if (out.variable.large != large) {
        error = "cannot mix offset widths in column ";
        error += field.name;
        return false;
    }
    // Rebase this batch's offsets by `base` writing straight into a single resize()d extension --
    // one vector::insert call PER ROW here previously made multi-batch string ingest per-row-bound
    // (each 4/8-byte insert pays the full call + growth-check machinery).
    const std::size_t add_offsets = expected_offsets - offset_width;  // batch offset 0 is not re-emitted
    const std::size_t existing = out.variable.offsets.size();
    out.variable.offsets.resize(existing + add_offsets);
    std::uint8_t* dst = out.variable.offsets.data() + existing;
    for (std::size_t i = 1; i < expected_offsets / offset_width; ++i) {
        if (large) {
            std::int64_t value = 0;
            std::memcpy(&value, offsets + i * offset_width, offset_width);
            value += static_cast<std::int64_t>(base);
            std::memcpy(dst, &value, offset_width);
        } else {
            std::int32_t value = 0;
            std::memcpy(&value, offsets + i * offset_width, offset_width);
            value += static_cast<std::int32_t>(base);
            std::memcpy(dst, &value, offset_width);
        }
        dst += offset_width;
    }
    out.variable.data.insert(out.variable.data.end(), data, data + data_bytes);
    return true;
}

}  // namespace

const ArrowArray* resolve_field_array(const ArrowArray& batch,
                                      const LanceSchemaMapping& mapping,
                                      const LanceField& field) {
    return resolve_field_array_impl(batch, mapping, field);
}

bool append_batch_column_values(const ArrowArray& batch,
                                const LanceSchemaMapping& mapping,
                                std::vector<ColumnValues>& columns,
                                std::string& error) {
    return append_batch_column_values(batch, mapping, columns, error, -1);
}

bool append_batch_column_values(const ArrowArray& batch,
                                const LanceSchemaMapping& mapping,
                                std::vector<ColumnValues>& columns,
                                std::string& error,
                                std::int32_t skip_blob_parent_id,
                                bool borrow_fixed_buffers) {
    const auto physical = lance_physical_fields(mapping);
    std::vector<const LanceField*> selected;
    selected.reserve(physical.size());
    for (const auto* field : physical) {
        if (skip_blob_parent_id >= 0 && field->parent_id == skip_blob_parent_id) {
            continue;
        }
        selected.push_back(field);
    }
    if (columns.empty()) {
        columns.resize(selected.size());
    }
    if (columns.size() != selected.size()) {
        error = "column value buffers do not match physical field count";
        return false;
    }
    for (std::size_t i = 0; i < selected.size(); ++i) {
        const auto& field = *selected[i];
        const auto* array = resolve_field_array(batch, mapping, field);
        if (array == nullptr) {
            error = "missing ArrowArray for mapped field ";
            error += field.name;
            return false;
        }
        if (lance_field_is_variable_width(field.logical_type)) {
            if (!append_variable_width(*array, field, columns[i], error)) {
                return false;
            }
        } else {
            if (!append_fixed_width(*array, field, columns[i], error, borrow_fixed_buffers)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace nano_lance
