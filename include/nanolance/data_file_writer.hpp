// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/column_values.hpp"
#include "nanolance/schema_mapper.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace nano_lance {

/// What a Lance file can carry besides its columns, as Lance's own index files use it: metadata on
/// the file's schema, and global buffers after the schema's (which is global buffer 0), numbered from
/// 1 in the order given.
struct LanceFileExtras {
    std::map<std::string, std::vector<std::uint8_t>> schema_metadata;
    std::vector<std::vector<std::uint8_t>> global_buffers;
    /// Where to write the file. Empty: <dataset_path>/data/<file_name>, as for a data file.
    std::filesystem::path path;
    /// write_lance_file: bit-pack integer columns (tagged nanolance:packing in their field metadata).
    bool bitpack_integers = false;
};

struct DataFileResult {
    std::filesystem::path relative_path;
    std::uint64_t file_size_bytes = 0;
};

/// With `parallel_columns` and more than one thread (parallel.hpp), columns are encoded side by side
/// into memory and appended in order -- the same bytes, faster, holding the fragment's encoded columns
/// at once. Without it, each column streams to the file as it is encoded (a write memory budget).
bool write_lance_data_file(const std::filesystem::path& dataset_path,
                           const std::string& file_name,
                           const LanceSchemaMapping& mapping,
                           const std::vector<ColumnValues>& column_values,
                           std::uint64_t rows,
                           int compression_level,
                           bool compress,
                           DataFileResult& result,
                           std::string& error,
                           bool parallel_columns = true,
                           const LanceFileExtras* extras = nullptr);

}  // namespace nano_lance
