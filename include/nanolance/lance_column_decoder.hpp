// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"
#include "nanolance/column_values.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance {

/// Decode one physical Lance column from a data file (writer encodings only).
bool decode_lance_physical_column(const std::filesystem::path& data_file_path, const pb::Field& on_disk_field,
                                  const pb::ColumnMetadata& column_metadata, ColumnValues& out, std::string& error);

/// Decode only `rows` of the column: physical row numbers within this data file, strictly ascending.
/// `out` then holds those rows, in that order.
///
/// Only the pages holding a requested row are read. A FullZip page of variable-width values -- how
/// Lance and nanolance store images, audio and other large values -- is read row by row through its
/// repetition index: a random batch of 64 images reads 64 images. Any other page is decoded whole and
/// the rows picked out of it (`value_bytes` is the column's fixed value width, as for
/// compact_column_values; 0 for variable width).
bool decode_lance_physical_column_rows(const std::filesystem::path& data_file_path, const pb::Field& on_disk_field,
                                       const pb::ColumnMetadata& column_metadata,
                                       const std::vector<std::uint64_t>& rows, std::size_t value_bytes,
                                       ColumnValues& out, std::string& error);

/// Can a page give up a row range without being decoded whole? A list page with a repetition index
/// (only the chunks holding the rows), or a FullZip page of large values (only the rows).
bool lance_page_row_addressable(const pb::ColumnPage& page);

/// Rows [first, first + count) of one column: the pages the range touches, decoded and trimmed --
/// for a list page with a repetition index, only the chunks holding those rows. How a parallel read
/// splits a fragment into row ranges it decodes independently (lance_table_reader.cpp).
bool decode_lance_physical_column_range(const std::filesystem::path& data_file_path, const pb::Field& on_disk_field,
                                        const pb::ColumnMetadata& column_metadata, std::uint64_t first,
                                        std::uint64_t count, std::size_t value_bytes, ColumnValues& out,
                                        std::string& error);

}  // namespace nano_lance
