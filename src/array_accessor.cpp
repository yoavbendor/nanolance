// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/array_accessor.hpp"

#include "nanolance/bool_bitpack.hpp"
#include "nanolance/read_safety.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

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

/// Narrow `child` to the rows an enclosing array's `[enclosing_offset, +enclosing_length)` window
/// covers, as a borrowed shallow view.
///
/// Arrow does NOT slice a struct's children when the struct is sliced: the parent carries the
/// offset and the children keep their full extent. So a child's own `offset`/`length` describe the
/// whole column, and reading them directly returns the wrong rows AND the wrong count. Composing
/// the windows here is what makes `field.parent_id` chains slice-correct.
bool narrow_to(const ArrowArray& child, std::int64_t enclosing_offset, std::int64_t enclosing_length,
               ArrowArray& out) {
    const auto offset = child.offset + enclosing_offset;
    if (offset < child.offset || enclosing_length < 0 ||
        offset + enclosing_length > child.offset + child.length) {
        return false;  // the window is not inside the child; a malformed batch, not a slice
    }
    out = child;
    out.offset = offset;
    out.length = enclosing_length;
    // `null_count` described the whole child, not this window. -1 is the interface's "not computed"
    // and makes callers scan the bitmap over the window instead. Zero stays zero: a null-free column
    // has null-free windows, and keeping it preserves the fast path.
    if (out.null_count != 0) {
        out.null_count = -1;
    }
    // A view borrows everything it points at. Blanking `release` makes an accidental release a loud
    // null dereference rather than a silent double free of the real batch.
    out.release = nullptr;
    return true;
}

bool resolve_field_array_impl(const ArrowArray& batch, const LanceSchemaMapping& mapping,
                              const LanceField& field, ArrowArray& out) {
    if (field.parent_id < 0) {
        const auto index = top_level_batch_child_index(mapping, field);
        if (index < 0) {
            return false;
        }
        if (batch.n_children > 0 && batch.children != nullptr) {
            if (index >= batch.n_children || batch.children[index] == nullptr) {
                return false;
            }
            // Both conventions have to work: pyarrow slices each COLUMN (offset on the child, root
            // at 0), while a producer that slices the record batch itself puts the offset on the
            // ROOT and leaves children whole. Composing them adds zero in each case.
            return narrow_to(*batch.children[index], batch.offset, batch.length, out);
        }
        if (index != 0) {
            return false;
        }
        out = batch;
        out.release = nullptr;
        return true;
    }

    const auto* parent_field = find_field_by_id(mapping, field.parent_id);
    if (parent_field == nullptr) {
        return false;
    }
    ArrowArray parent_view{};
    if (!resolve_field_array_impl(batch, mapping, *parent_field, parent_view)) {
        return false;
    }
    const auto* child = child_by_mapped_name(parent_view, mapping, field.parent_id, field.name);
    if (child == nullptr) {
        return false;
    }
    return narrow_to(*child, parent_view.offset, parent_view.length, out);
}

// --- Null detection -------------------------------------------------------------------------
//
// Historically `ignore_nullability` meant "copy the null slot's raw bytes as-is", which turned
// [10, null, 30] into [10, 0, 30] and ["x", null] into ["x", ""] with no error and no warning --
// silent data loss, and the worst kind, because stock Lance reads the result back happily and just
// reports wrong values. Ingest now CAPTURES a validity bitmap per column, which the writer turns
// into Lance's definition-level layer; the few shapes that layer cannot express (a null struct, a
// null in a lance.blob.v2 column) are refused by name in data_file_writer.cpp rather than dropped.
// `ignore_nullability` keeps only its job of accepting a *nullable-flagged* field (pyarrow marks
// essentially everything nullable), and can no longer cost you data.

/// First row index in [0, length) that is null, or -1 when the array has no nulls.
/// Arrow's `null_count` is authoritative when non-negative; -1 means "not computed" and we scan the
/// validity bitmap ourselves. Absent validity buffer == every slot valid.
std::int64_t first_null_row(const ArrowArray& array) {
    if (array.length <= 0 || array.null_count == 0) {
        return -1;
    }
    if (array.n_buffers < 1 || array.buffers == nullptr || array.buffers[0] == nullptr) {
        // No validity buffer. A positive null_count here would be malformed; treat the buffer as the
        // source of truth and report the array as all-valid rather than inventing a row index.
        return -1;
    }
    const auto* validity = static_cast<const std::uint8_t*>(array.buffers[0]);
    const auto base = static_cast<std::uint64_t>(array.offset);
    const auto length = static_cast<std::uint64_t>(array.length);
    // Scan whole bytes where the window is byte-aligned (the overwhelmingly common case, and what
    // makes this O(n/8) rather than O(n) for the all-valid-but-null_count-unknown path); the
    // unaligned head/tail fall back to bit tests.
    std::uint64_t i = 0;
    while (i < length && ((base + i) & 7U) != 0U) {
        const auto bit = base + i;
        if (((validity[bit >> 3U] >> (bit & 7U)) & 1U) == 0U) {
            return static_cast<std::int64_t>(i);
        }
        ++i;
    }
    while (i + 8U <= length) {
        const auto byte = validity[(base + i) >> 3U];
        if (byte != 0xFFU) {
            for (std::uint64_t b = 0; b < 8U; ++b) {
                if (((byte >> b) & 1U) == 0U) {
                    return static_cast<std::int64_t>(i + b);
                }
            }
        }
        i += 8U;
    }
    while (i < length) {
        const auto bit = base + i;
        if (((validity[bit >> 3U] >> (bit & 7U)) & 1U) == 0U) {
            return static_cast<std::int64_t>(i);
        }
        ++i;
    }
    return -1;
}

/// Is row `i` of `array` null? Callers have already established that a validity buffer exists.
bool row_is_null(const ArrowArray& array, std::int64_t i) {
    const auto* validity = static_cast<const std::uint8_t*>(array.buffers[0]);
    const auto bit = static_cast<std::uint64_t>(array.offset + i);
    return ((validity[bit >> 3U] >> (bit & 7U)) & 1U) == 0U;
}

/// Accumulate `field`'s validity for this batch into `out`, at the rows following the ones already
/// appended. A null on an ANCESTOR struct makes every one of its descendant rows null without any
/// bit being clear on the child itself, so the whole chain is folded together here.
///
/// The bitmap follows Arrow's convention (bit SET == valid) and is left EMPTY while every row so far
/// is valid, so a column with no nulls -- the overwhelmingly common case -- carries no bitmap and
/// costs nothing. It materializes lazily the first time a null shows up, back-filling the rows
/// already seen as valid.
bool append_validity(const ArrowArray& batch,
                     const LanceSchemaMapping& mapping,
                     const LanceField& field,
                     std::int64_t length,
                     ColumnValues& out,
                     std::string& error) {
    // Collect the arrays along the chain that actually carry nulls; usually none do.
    //
    // BY VALUE, not by pointer. These are rebased views built per iteration, so a vector of pointers
    // into them dangles the moment the loop body ends -- which is what it did, and what ASan caught
    // as a stack-use-after-scope. An ArrowArray is a small POD header; copying it is free and the
    // buffers it points at belong to the batch either way.
    std::vector<ArrowArray> nullable_levels;
    for (const LanceField* f = &field; f != nullptr;
         f = f->parent_id < 0 ? nullptr : find_field_by_id(mapping, f->parent_id)) {
        ArrowArray view{};
        if (!resolve_field_array_impl(batch, mapping, *f, view)) {
            continue;
        }
        const ArrowArray* array = &view;
        if (first_null_row(*array) >= 0) {
            if (array->length < length) {
                error = "column '" + f->name + "' is shorter than the batch it belongs to";
                return false;
            }
            // A null on an ANCESTOR means the struct itself is null, which is not the same thing as
            // a struct whose every field is null -- Lance distinguishes them with a deeper
            // definition level, and nanolance only writes the one level. Folding the parent's nulls
            // into the child would silently turn "no struct here" into "a struct with nothing in
            // it", so refuse instead.
            if (f != &field) {
                error = "column '" + f->name + "' is a struct with a null at row " +
                        std::to_string(first_null_row(*array)) +
                        "; nanolance can store nulls on a field but not on the struct that contains "
                        "it (that needs a second definition level). Flatten the struct, or fill it "
                        "in and null its fields instead.";
                return false;
            }
            nullable_levels.push_back(view);
        }
    }

    const auto base = out.rows;
    if (nullable_levels.empty()) {
        // Nothing null in this batch. Only extend an existing bitmap; do not create one.
        if (!out.validity.empty()) {
            out.validity.resize(static_cast<std::size_t>((base + static_cast<std::uint64_t>(length) + 7U) / 8U), 0U);
            for (std::int64_t i = 0; i < length; ++i) {
                const auto row = base + static_cast<std::uint64_t>(i);
                out.validity[static_cast<std::size_t>(row >> 3U)] |=
                    static_cast<std::uint8_t>(1U << (row & 7U));
            }
        }
        return true;
    }

    // First null ever seen for this column: materialize the bitmap and mark every earlier row valid.
    if (out.validity.empty() && base != 0U) {
        out.validity.assign(static_cast<std::size_t>((base + 7U) / 8U), 0xFFU);
        // Clear any padding bits above `base` so they cannot be mistaken for real rows.
        for (std::uint64_t row = base; row < ((base + 7U) / 8U) * 8U; ++row) {
            out.validity[static_cast<std::size_t>(row >> 3U)] &=
                static_cast<std::uint8_t>(~(1U << (row & 7U)));
        }
    }
    out.validity.resize(static_cast<std::size_t>((base + static_cast<std::uint64_t>(length) + 7U) / 8U), 0U);
    for (std::int64_t i = 0; i < length; ++i) {
        bool is_null = false;
        for (const auto& level : nullable_levels) {
            is_null = is_null || row_is_null(level, i);
        }
        const auto row = base + static_cast<std::uint64_t>(i);
        if (is_null) {
            ++out.null_count;
        } else {
            out.validity[static_cast<std::size_t>(row >> 3U)] |= static_cast<std::uint8_t>(1U << (row & 7U));
        }
    }
    return true;
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
        const auto at = out.fixed.size();
        reserve_more(out.fixed, static_cast<std::size_t>(array.length));
        out.fixed.resize(at + static_cast<std::size_t>(array.length));
        boolpack::unpack_lsb_first(bits, static_cast<std::size_t>(array.offset),
                                   static_cast<std::size_t>(array.length), out.fixed.data() + at);
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

/// A fixed_size_list's rows are its child's elements, N per row: row r of a list at offset `o` is
/// child elements [(o + r) * N, (o + r + 1) * N), further shifted by the child's own offset. Copied as
/// one run -- that byte layout is exactly what Lance stores. Row-level nulls were taken by
/// append_validity from the list's own bitmap; a null ELEMENT inside a row is refused, because
/// writing it needs FixedSizeList.has_validity, which this writer does not emit.
bool append_fixed_size_list(const ArrowArray& array, const LanceField& field, const std::string& element,
                            std::uint64_t items, ColumnValues& out, std::string& error) {
    const ArrowArray* child = array.n_children == 1 && array.children != nullptr ? array.children[0] : nullptr;
    if (child == nullptr || child->n_buffers < 2 || child->buffers == nullptr || child->buffers[1] == nullptr) {
        error = "fixed_size_list column '" + field.name + "' has no values buffer";
        return false;
    }
    const std::size_t element_bytes = lance_logical_type_value_bytes(element);
    const auto first = static_cast<std::uint64_t>(child->offset) + static_cast<std::uint64_t>(array.offset) * items;
    const auto count = static_cast<std::uint64_t>(array.length) * items;
    if (child->null_count != 0 && child->buffers[0] != nullptr) {
        // Only elements of VALID rows count. pyarrow marks every element of a null row null as well
        // (pa.array([None, [1, 2]], pa.list_(t, 2)) does), and those are masked by the row anyway --
        // refusing them refused every ordinary nullable vector column.
        const auto* bits = static_cast<const std::uint8_t*>(child->buffers[0]);
        const auto* rows = static_cast<const std::uint8_t*>(array.buffers != nullptr ? array.buffers[0] : nullptr);
        for (std::uint64_t k = first; k < first + count; ++k) {
            const auto row = static_cast<std::uint64_t>(array.offset) + (k - first) / items;
            if (rows != nullptr && ((rows[row >> 3U] >> (row & 7U)) & 1U) == 0U) {
                continue;
            }
            if (((bits[k >> 3U] >> (k & 7U)) & 1U) == 0U) {
                error = "fixed_size_list column '" + field.name +
                        "' has a null element inside a row; whole-row nulls are supported, null elements "
                        "are not yet";
                return false;
            }
        }
    }
    out.kind = ColumnValues::Kind::FixedWidth;
    const auto* bytes = static_cast<const std::uint8_t*>(child->buffers[1]) + first * element_bytes;
    out.fixed.insert(out.fixed.end(), bytes, bytes + count * element_bytes);
    return true;
}

bool append_variable_width(const ArrowArray& array,
                           const LanceField& field,
                           ColumnValues& out,
                           std::string& error) {
    const bool large = lance_logical_type_has_large_offsets(field.logical_type);
    const std::size_t offset_width = large ? 8U : 4U;
    if (array.n_buffers < 3 || array.buffers == nullptr || array.buffers[1] == nullptr ||
        array.buffers[2] == nullptr) {
        error = "variable-width array is missing offsets/data buffers for ";
        error += field.name;
        return false;
    }
    out.kind = ColumnValues::Kind::VariableWidth;
    if (!out.variable.offsets.empty() && out.variable.large != large) {
        error = "cannot mix offset widths in column ";
        error += field.name;
        return false;
    }
    out.variable.large = large;

    // A sliced batch shares its parent's buffers and addresses them through array.offset -- both the
    // offsets buffer AND, transitively, the data buffer, whose live region starts at offsets[offset].
    // Ignoring that made every batch after the first re-ingest the FIRST batch's values: to_batches()
    // hands out slices of one contiguous array, so a multi-batch utf8/binary column was silently
    // corrupted from row `chunksize` on (fixed-width columns were always correct -- they apply
    // array.offset above).
    const auto* offsets = static_cast<const std::uint8_t*>(array.buffers[1]) +
                          static_cast<std::size_t>(array.offset) * offset_width;
    const auto* data = static_cast<const std::uint8_t*>(array.buffers[2]);
    const auto count = static_cast<std::size_t>(array.length);

    const auto raw_offset = [&](std::size_t i) -> std::uint64_t {
        const std::uint8_t* p = offsets + i * offset_width;
        if (large) {
            std::int64_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<std::uint64_t>(value);
        }
        std::int32_t value = 0;
        std::memcpy(&value, p, sizeof(value));
        return static_cast<std::uint64_t>(value);
    };
    // Only touch offsets[0]/offsets[count] when there is a value to bound; a zero-length batch is
    // allowed to carry an empty offsets buffer.
    std::uint64_t data_begin = 0;
    std::uint64_t data_end = 0;
    if (count > 0) {
        data_begin = raw_offset(0);
        data_end = raw_offset(count);
    }

    const std::size_t base = out.variable.data.size();
    const bool first = out.variable.offsets.empty();
    // Rebase this batch's offsets onto the accumulated buffer (subtract the slice's own start, add
    // what we already hold) writing straight into a single resize()d extension -- one vector::insert
    // call PER ROW here previously made multi-batch string ingest per-row-bound.
    const std::size_t add = (first ? 1U : 0U) + count;
    const std::size_t existing = out.variable.offsets.size();
    out.variable.offsets.resize(existing + add * offset_width);
    std::uint8_t* dst = out.variable.offsets.data() + existing;
    const auto write_offset = [&](std::uint64_t value) {
        if (large) {
            const auto narrowed = static_cast<std::int64_t>(value);
            std::memcpy(dst, &narrowed, sizeof(narrowed));
        } else {
            const auto narrowed = static_cast<std::int32_t>(value);
            std::memcpy(dst, &narrowed, sizeof(narrowed));
        }
        dst += offset_width;
    };
    if (first) {
        write_offset(base);  // 0, but spelled as the invariant the rebasing keeps
    }
    for (std::size_t i = 1; i <= count; ++i) {
        write_offset(raw_offset(i) - data_begin + base);
    }
    out.variable.data.insert(out.variable.data.end(), data + data_begin, data + data_end);
    return true;
}

/// Append `count` validity bits of `array` from physical index `at` to a lazily materialized bitmap
/// that already holds `have` entries (empty = all valid so far).
void append_bits(std::vector<std::uint8_t>& bits, std::uint64_t& nulls, std::uint64_t have, const ArrowArray& array,
                 std::int64_t at, std::int64_t count) {
    const auto* src = array.n_buffers >= 1 && array.buffers != nullptr && array.null_count != 0
                          ? static_cast<const std::uint8_t*>(array.buffers[0])
                          : nullptr;
    for (std::int64_t i = 0; i < count; ++i) {
        const auto b = static_cast<std::uint64_t>(at + i);
        const bool valid = src == nullptr || ((src[b >> 3U] >> (b & 7U)) & 1U) != 0U;
        const auto dst = have + static_cast<std::uint64_t>(i);
        if (!valid && bits.empty()) {
            bits.assign(static_cast<std::size_t>((dst + 8U) / 8U), 0U);
            for (std::uint64_t j = 0; j < dst; ++j) {
                bits[static_cast<std::size_t>(j >> 3U)] |= static_cast<std::uint8_t>(1U << (j & 7U));
            }
        }
        if (bits.empty()) {
            continue;
        }
        bits.resize(static_cast<std::size_t>((dst + 8U) / 8U), 0U);
        if (valid) {
            bits[static_cast<std::size_t>(dst >> 3U)] |= static_cast<std::uint8_t>(1U << (dst & 7U));
        } else {
            ++nulls;
        }
    }
}

/// A leaf under one or more lists (or a map). Walks its path from the top-level column down, one
/// layer per list or struct, recording each layer's offsets and validity for this batch's rows and
/// narrowing to the children those rows cover; then appends the leaf values those children span.
/// Physical indices compose the Arrow way: a list's offsets are logical indices into its child (plus
/// the child's own offset), and a struct's children are NOT sliced with it (child physical = child
/// offset + the struct's physical index).
bool append_nested(const ArrowArray& batch, const LanceSchemaMapping& mapping, const LanceField& field,
                   ColumnValues& out, std::string& error) {
    std::vector<const LanceField*> path;
    for (const LanceField* f = &field; f != nullptr;
         f = f->parent_id < 0 ? nullptr : find_field_by_id(mapping, f->parent_id)) {
        path.push_back(f);
    }
    std::reverse(path.begin(), path.end());
    ArrowArray top{};
    if (!resolve_field_array_impl(batch, mapping, *path.front(), top)) {
        error = "missing ArrowArray for mapped field " + path.front()->name;
        return false;
    }
    if (out.layers.empty()) {
        out.layers.resize(path.size() - 1U);
        for (std::size_t k = 0; k + 1U < path.size(); ++k) {
            out.layers[k].is_list = lance_logical_type_is_list(path[k]->logical_type);
            if (out.layers[k].is_list) {
                out.layers[k].offsets.push_back(0);
            }
        }
    }
    if (out.layers.size() + 1U != path.size()) {
        error = "column '" + field.name + "' changed nesting between batches";
        return false;
    }
    const ArrowArray* node = &top;
    std::int64_t at = top.offset;
    std::int64_t count = top.length;
    for (std::size_t k = 0; k + 1U < path.size(); ++k) {
        auto& layer = out.layers[k];
        append_bits(layer.validity, layer.null_count, layer.length, *node, at, count);
        layer.length += static_cast<std::uint64_t>(count);
        if (layer.is_list) {
            if (node->n_buffers < 2 || node->buffers == nullptr || node->buffers[1] == nullptr ||
                node->n_children != 1 || node->children == nullptr || node->children[0] == nullptr) {
                error = "list array for '" + path[k]->name + "' is missing its offsets or child";
                return false;
            }
            const bool large = lance_logical_type_is_large_list(path[k]->logical_type);
            const auto offset_at = [&](std::int64_t i) -> std::int64_t {
                if (large) {
                    return static_cast<const std::int64_t*>(node->buffers[1])[i];
                }
                return static_cast<const std::int32_t*>(node->buffers[1])[i];
            };
            const auto first = offset_at(at);
            const auto last = offset_at(at + count);
            if (last < first) {
                error = "list array for '" + path[k]->name + "' has decreasing offsets";
                return false;
            }
            const auto base = layer.offsets.back();
            for (std::int64_t i = 1; i <= count; ++i) {
                layer.offsets.push_back(base + offset_at(at + i) - first);
            }
            node = node->children[0];
            at = node->offset + first;
            count = last - first;
        } else {
            const auto* child = child_by_mapped_name(*node, mapping, path[k]->id, path[k + 1U]->name);
            if (child == nullptr) {
                error = "struct array for '" + path[k]->name + "' is missing child " + path[k + 1U]->name;
                return false;
            }
            at = child->offset + at;
            node = child;
        }
        if (at < 0 || count < 0 || at + count > node->offset + node->length) {
            error = "column '" + field.name + "': a list points past the end of its child array";
            return false;
        }
    }

    std::string element;
    std::uint64_t items = 0;
    if (field.logical_type == "null" || !field.extension_name.empty() ||
        lance_fixed_size_list_parts(field.logical_type, element, items)) {
        error = "column '" + field.name + "': a list of " + field.logical_type + " cannot be written yet";
        return false;
    }
    ArrowArray leaf = *node;
    leaf.offset = at;
    leaf.length = count;
    if (leaf.null_count != 0) {
        leaf.null_count = -1;
    }
    leaf.release = nullptr;
    // Items already held, for the validity bitmap's position. Taken before the values are appended.
    std::uint64_t items_before = 0;
    if (out.kind == ColumnValues::Kind::VariableWidth && !out.variable.offsets.empty()) {
        items_before = out.variable.offsets.size() / (out.variable.large ? 8U : 4U) - 1U;
    } else if (out.kind == ColumnValues::Kind::FixedWidth) {
        const auto width = lance_logical_type_value_bytes(field.logical_type);
        items_before = width == 0U ? 0U : out.fixed.size() / width;
    }
    append_bits(out.validity, out.null_count, items_before, leaf, at, count);
    if (lance_field_is_variable_width(field.logical_type)) {
        return append_variable_width(leaf, field, out, error);
    }
    return append_fixed_width(leaf, field, out, error);
}

/// A leaf under a list, or under plain structs: both are walked as nested columns, so every struct's
/// nulls and every list's offsets are recorded per layer. (Whether the pages are written nested is
/// decided later, per fragment: see ColumnValues::needs_nested_pages.) A leaf under an extension type
/// -- lance.blob.v2 -- keeps its own path.
bool is_nested_leaf(const LanceSchemaMapping& mapping, const LanceField& field) {
    bool any = false;
    for (const LanceField* f = field.parent_id < 0 ? nullptr : find_field_by_id(mapping, field.parent_id); f != nullptr;
         f = f->parent_id < 0 ? nullptr : find_field_by_id(mapping, f->parent_id)) {
        if (!f->extension_name.empty()) {
            return false;
        }
        any = true;
    }
    return any;
}

}  // namespace

bool resolve_field_array(const ArrowArray& batch, const LanceSchemaMapping& mapping,
                         const LanceField& field, ArrowArray& out) {
    out = ArrowArray{};
    return resolve_field_array_impl(batch, mapping, field, out);
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
        // A leaf under a list is not row-aligned with the batch (a list's child has as many entries
        // as its lists have items), so it is reached by walking down from its top-level column.
        if (is_nested_leaf(mapping, field)) {
            const LanceField* top = &field;
            while (top->parent_id >= 0) {
                top = find_field_by_id(mapping, top->parent_id);
            }
            ArrowArray top_view{};
            if (!resolve_field_array(batch, mapping, *top, top_view)) {
                error = "missing ArrowArray for mapped field " + top->name;
                return false;
            }
            if (!append_nested(batch, mapping, field, columns[i], error)) {
                return false;
            }
            columns[i].rows += static_cast<std::uint64_t>(top_view.length);
            continue;
        }
        ArrowArray view{};
        if (!resolve_field_array(batch, mapping, field, view)) {
            error = "missing ArrowArray for mapped field ";
            error += field.name;
            return false;
        }
        const ArrowArray* array = &view;
        // Arrow's null type has NO buffers -- not even a validity bitmap; every row is null by
        // definition. Neither path below can take it (both start from a buffer that is not there), and
        // there is nothing to encode beyond the row count: the writer emits Lance's all-null spelling.
        if (field.logical_type == "null") {
            auto& column = columns[i];
            const auto base = column.rows;
            const auto rows = base + static_cast<std::uint64_t>(array->length);
            if (column.validity.empty() && base != 0U) {
                error = "column '";
                error += field.name;
                error += "' is null-typed but earlier rows were recorded as valid";
                return false;
            }
            column.kind = ColumnValues::Kind::FixedWidth;
            column.validity.resize(static_cast<std::size_t>((rows + 7U) / 8U), 0U);  // every bit clear
            column.null_count += static_cast<std::uint64_t>(array->length);
            column.rows = rows;
            continue;
        }
        // Validity first: it is recorded against the rows already appended, so it has to be taken
        // before the value append advances them.
        if (!append_validity(batch, mapping, field, array->length, columns[i], error)) {
            return false;
        }
        std::string fsl_element;
        std::uint64_t fsl_items = 0;
        if (lance_fixed_size_list_parts(field.logical_type, fsl_element, fsl_items)) {
            if (!append_fixed_size_list(*array, field, fsl_element, fsl_items, columns[i], error)) {
                return false;
            }
        } else if (lance_field_is_variable_width(field.logical_type)) {
            if (!append_variable_width(*array, field, columns[i], error)) {
                return false;
            }
        } else {
            if (!append_fixed_width(*array, field, columns[i], error, borrow_fixed_buffers)) {
                return false;
            }
        }
        columns[i].rows += static_cast<std::uint64_t>(array->length);
    }
    return true;
}

}  // namespace nano_lance
