// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/column_values.hpp"
#include "nanolance/schema_mapper.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance {

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
                           bool parallel_columns = true);

}  // namespace nano_lance
