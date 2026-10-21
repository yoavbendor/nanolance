// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The text of an FTS document read from a column: a string, or a list of strings (Lance's row
// granularity). A list's document is its non-null elements joined by one space, as Lance's
// InvertedIndexBuilder joins them (builder.rs, materialize_string_list), so the positions continue
// across elements with no gap and the tokens are Lance's to the byte.

#pragma once

#include <nanoarrow/nanoarrow.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace nano_lance::fts {

/// The kind of an FTS column: a string column, a list (or large list) of strings, or neither.
enum class TextColumn { kNone, kString, kStringList };

namespace detail {

inline bool is_string_format(const char* format) {
    return format != nullptr && (std::strcmp(format, "u") == 0 || std::strcmp(format, "U") == 0 ||
                                 std::strcmp(format, "vu") == 0);
}

inline bool is_string_storage(ArrowType type) {
    return type == NANOARROW_TYPE_STRING || type == NANOARROW_TYPE_LARGE_STRING ||
           type == NANOARROW_TYPE_STRING_VIEW;
}

}  // namespace detail

/// The kind of a column given its schema (the column's own ArrowSchema, not its parent's).
inline TextColumn text_schema_kind(const ArrowSchema* column) {
    if (column == nullptr || column->format == nullptr) {
        return TextColumn::kNone;
    }
    if (detail::is_string_format(column->format)) {
        return TextColumn::kString;
    }
    const bool list = std::strcmp(column->format, "+l") == 0 || std::strcmp(column->format, "+L") == 0;
    if (list && column->n_children == 1 && column->children[0] != nullptr &&
        detail::is_string_format(column->children[0]->format)) {
        return TextColumn::kStringList;
    }
    return TextColumn::kNone;
}

/// The kind of a column given its array view.
inline TextColumn text_column_kind(const ArrowArrayView* column) {
    if (column == nullptr) {
        return TextColumn::kNone;
    }
    if (detail::is_string_storage(column->storage_type)) {
        return TextColumn::kString;
    }
    const bool list = column->storage_type == NANOARROW_TYPE_LIST || column->storage_type == NANOARROW_TYPE_LARGE_LIST;
    if (list && column->n_children == 1 && column->children[0] != nullptr &&
        detail::is_string_storage(column->children[0]->storage_type)) {
        return TextColumn::kStringList;
    }
    return TextColumn::kNone;
}

/// The document of `row` (as the column's own accessors take it) of a column of `kind`: false when the
/// row is null. For a list, the text is built in `joined` (which `text` then views).
inline bool row_text(const ArrowArrayView* column, TextColumn kind, std::int64_t row, std::string& joined,
                     std::string_view& text) {
    if (ArrowArrayViewIsNull(column, row)) {
        return false;
    }
    if (kind == TextColumn::kString) {
        const ArrowStringView s = ArrowArrayViewGetStringUnsafe(column, row);
        text = std::string_view(s.data, static_cast<std::size_t>(s.size_bytes));
        return true;
    }
    // ArrowArrayViewListChildOffset takes positions in the raw offsets buffer, so the row is made absolute.
    const ArrowArrayView* elements = column->children[0];
    const std::int64_t first = ArrowArrayViewListChildOffset(column, column->offset + row);
    const std::int64_t last = ArrowArrayViewListChildOffset(column, column->offset + row + 1);
    joined.clear();
    for (std::int64_t k = first; k < last; ++k) {
        if (ArrowArrayViewIsNull(elements, k)) {
            continue;
        }
        const ArrowStringView s = ArrowArrayViewGetStringUnsafe(elements, k);
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        if (s.size_bytes > 0) {
            joined.append(s.data, static_cast<std::size_t>(s.size_bytes));
        }
    }
    text = joined;
    return true;
}

/// A text column below struct fields (`meta.body.text`): the struct children from the top-level column's
/// schema down to the leaf, by name. `names` are the path's parts after the top-level column.
inline bool text_child_path(const ArrowSchema* column, const std::vector<std::string>& names,
                            std::vector<std::size_t>& index) {
    index.clear();
    const ArrowSchema* at = column;
    for (const auto& name : names) {
        std::size_t found = 0;
        bool any = false;
        for (std::int64_t c = 0; at != nullptr && c < at->n_children; ++c) {
            if (at->children[c] != nullptr && at->children[c]->name != nullptr && name == at->children[c]->name) {
                found = static_cast<std::size_t>(c);
                any = true;
                break;
            }
        }
        if (!any) {
            return false;
        }
        index.push_back(found);
        at = at->children[found];
    }
    return true;
}

/// The schema of the leaf a path names (null when the path does not fit the schema).
inline const ArrowSchema* text_schema_at(const ArrowSchema* column, const std::vector<std::size_t>& index) {
    const ArrowSchema* at = column;
    for (const auto c : index) {
        if (at == nullptr || static_cast<std::int64_t>(c) >= at->n_children) {
            return nullptr;
        }
        at = at->children[c];
    }
    return at;
}

/// The view of the leaf a path names. `above` gets the struct views from the top-level column down to
/// the leaf's parent: a document is null where any of them is null, as a null struct has no text.
inline const ArrowArrayView* text_view_at(const ArrowArrayView* column, const std::vector<std::size_t>& index,
                                          std::vector<const ArrowArrayView*>& above) {
    above.clear();
    const ArrowArrayView* at = column;
    for (const auto c : index) {
        if (at == nullptr || static_cast<std::int64_t>(c) >= at->n_children) {
            return nullptr;
        }
        above.push_back(at);
        at = at->children[c];
    }
    return at;
}

/// True when some struct above the leaf is null at `row` (the leaf's own nulls are row_text's).
inline bool null_above(const std::vector<const ArrowArrayView*>& above, std::int64_t row) {
    for (const auto* parent : above) {
        if (ArrowArrayViewIsNull(parent, row)) {
            return true;
        }
    }
    return false;
}

}  // namespace nano_lance::fts
