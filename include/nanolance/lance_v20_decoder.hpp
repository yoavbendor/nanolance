// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"
#include "nanolance/column_values.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/// Lance file format 2.0 (footer version 0.3), read into the same ColumnValues the 2.1 decoder
/// produces, so everything above it -- Arrow assembly, row ranges, take, deletions, filters -- is
/// shared.
///
/// 2.0 describes each page with an `ArrayEncoding` tree (encodings_v2_0.proto): Nullable around
/// Flat values or bitmaps, Binary (end offsets + bytes), Dictionary, Fsst, FixedSizeList,
/// FixedSizeBinary and two bit-packing schemes. Where 2.1 keeps a whole nested field in its leaf
/// column's repetition and definition levels, 2.0 gives EVERY field a column: a struct's (which holds
/// nothing -- 2.0 structs are never null), a list's (its offsets, pages of their own), then the
/// child's. A leaf below a list therefore needs the list columns above it: `LeafContext`.
namespace nano_lance::v20 {

/// Does this column hold format 2.0 pages (`/lance.encodings.ArrayEncoding`)?
bool is_v20_column(const pb::ColumnMetadata& column);

/// Are this 2.0 column's pages List pages (offsets)? True for a list's column, and for a string an
/// older writer stored as list<uint8> -- whose bytes are then the next column.
bool column_is_list_encoded(const pb::ColumnMetadata& column);

/// Are this 2.0 column's pages PackedStruct pages? A struct its writer packed (field metadata
/// `packed`): its fields -- fixed-width, never null -- have no columns; each row's values sit back
/// to back in the struct's.
bool column_is_packed_struct(const pb::ColumnMetadata& column);

/// A list or struct above a leaf, outermost first.
struct Ancestor {
    bool is_list = false;
    std::string name;
    /// A list's offsets column. Structs have none worth reading: they are never null in 2.0.
    const pb::ColumnMetadata* column = nullptr;
};

/// What a leaf column needs from the rest of its 2.0 file.
struct LeafContext {
    std::vector<Ancestor> ancestors;  // outermost first; empty for a top-level leaf
    /// A string or binary field that an older writer stored as list<uint8>: its own column holds
    /// the list offsets and this, the next column, the bytes.
    const pb::ColumnMetadata* binary_items = nullptr;
    /// A field of a packed struct, read from the struct's column: which of its fields. -1 otherwise.
    int packed_child = -1;
    bool has_lists() const;
};

/// The whole column. `context` may be null for a leaf with no list above it.
bool decode_column(const std::filesystem::path& path, const pb::Field& field, const pb::ColumnMetadata& column,
                   const LeafContext* context, ColumnValues& out, std::string& error);

/// Rows [first, first + count) of the column's top-level rows: the pages they touch, then sliced.
bool decode_column_range(const std::filesystem::path& path, const pb::Field& field, const pb::ColumnMetadata& column,
                         const LeafContext* context, std::uint64_t first, std::uint64_t count,
                         std::size_t value_bytes, ColumnValues& out, std::string& error);

/// The rows listed (strictly ascending): the pages holding them, then compacted to them.
bool decode_column_rows(const std::filesystem::path& path, const pb::Field& field, const pb::ColumnMetadata& column,
                        const LeafContext* context, const std::vector<std::uint64_t>& rows,
                        std::size_t value_bytes, ColumnValues& out, std::string& error);

}  // namespace nano_lance::v20
