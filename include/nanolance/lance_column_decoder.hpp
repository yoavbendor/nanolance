// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"
#include "nanolance/column_values.hpp"
#include "nanolance/encodings_io.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance {

/// Decode one physical Lance column from a data file (writer encodings only).
bool decode_lance_physical_column(const EncodingsIo& io, const pb::Field& on_disk_field,
                                  const pb::ColumnMetadata& column_metadata, ColumnValues& out, std::string& error);

/// Decode only `rows` of the column: physical row numbers within this data file, strictly ascending.
/// `out` then holds those rows, in that order.
///
/// Only the pages holding a requested row are read. A FullZip page of variable-width values -- how
/// Lance and nanolance store images, audio and other large values -- is read row by row through its
/// repetition index: a random batch of 64 images reads 64 images. Any other page is decoded whole and
/// the rows picked out of it (`value_bytes` is the column's fixed value width, as for
/// compact_column_values; 0 for variable width).
bool decode_lance_physical_column_rows(const EncodingsIo& io, const pb::Field& on_disk_field,
                                       const pb::ColumnMetadata& column_metadata,
                                       const std::vector<std::uint64_t>& rows, std::size_t value_bytes,
                                       ColumnValues& out, std::string& error);

/// Can a page give up a row range without being decoded whole? A list page with a repetition index
/// (only the chunks holding the rows), or a FullZip page of large values (only the rows).
bool lance_page_row_addressable(const pb::ColumnPage& page);

/// The values a page decodes to -- its rows, or for a list page its items -- as a measure of decode
/// work: a dictionary-encoded page of strings is small on disk and large once decoded.
std::uint64_t lance_page_items(const pb::ColumnPage& page);

/// The bytes a constant page expands to (its one value once per item), 0 for any other page. A
/// constant page is a few bytes on disk and can be hundreds of megabytes decoded (one 300 KB value
/// in each of 1,009 rows), so its stored size says nothing of what decoding it costs.
std::uint64_t lance_page_expanded_bytes(const pb::ColumnPage& page);

/// How many list layers the page's levels describe (0: none). Unravelling them is decode work of its
/// own, per layer.
std::uint32_t lance_page_list_depth(const pb::ColumnPage& page);

/// Rows [first, first + count) of one column: the pages the range touches, decoded and trimmed --
/// for a list page with a repetition index, only the chunks holding those rows. How a parallel read
/// splits a fragment into row ranges it decodes independently (lance_table_reader.cpp).
bool decode_lance_physical_column_range(const EncodingsIo& io, const pb::Field& on_disk_field,
                                        const pb::ColumnMetadata& column_metadata, std::uint64_t first,
                                        std::uint64_t count, std::size_t value_bytes, ColumnValues& out,
                                        std::string& error);

/// The dictionary entries of a column's dictionary pages (each entry's bytes), in page order, each
/// value once: what an Arrow dictionary column was written with. Empty when no page has one.
bool lance_column_dictionary_entries(const EncodingsIo& io, const pb::Field& on_disk_field,
                                     const pb::ColumnMetadata& column_metadata, std::vector<std::string>& out,
                                     std::string& error);

/// Lance 2.1 packed struct (`lance-encoding:packed`): a struct whose fixed-width children are one column, their
/// bytes interleaved row by row. `struct_field` is the struct's on-disk field and `column` its one column;
/// `child_bytes` is every child's width in bytes, in struct order, from the file's descriptor. `child` is the
/// struct's child index; the child's values come out as a fixed-width column of its own width, which the caller
/// passes as `value_bytes` as for the other decoders. Each of these reads the whole packed column for that one
/// child, as the 2.0 packed readers do; the rows, row range and row list are the same as the other decoders'.
/// `packed_struct_column` says whether a column is such a packed struct: its pages are constant rows of the
/// children's total width, or mini-block / full-zip rows whose PackedStruct gives those widths.
bool packed_struct_column(const EncodingsIo& io, const pb::ColumnMetadata& column,
                          const std::vector<std::uint64_t>& child_bytes);
bool decode_lance_packed_struct_child(const EncodingsIo& io, const pb::Field& struct_field,
                                      const pb::ColumnMetadata& column, const std::vector<std::uint64_t>& child_bytes,
                                      std::size_t child, ColumnValues& out, std::string& error);
bool decode_lance_packed_struct_child_range(const EncodingsIo& io, const pb::Field& struct_field,
                                            const pb::ColumnMetadata& column,
                                            const std::vector<std::uint64_t>& child_bytes, std::size_t child,
                                            std::uint64_t first, std::uint64_t count, std::size_t value_bytes,
                                            ColumnValues& out, std::string& error);
bool decode_lance_packed_struct_child_rows(const EncodingsIo& io, const pb::Field& struct_field,
                                           const pb::ColumnMetadata& column,
                                           const std::vector<std::uint64_t>& child_bytes, std::size_t child,
                                           const std::vector<std::uint64_t>& rows, std::size_t value_bytes,
                                           ColumnValues& out, std::string& error);

}  // namespace nano_lance
