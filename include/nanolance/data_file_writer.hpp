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

bool write_lance_data_file(const std::filesystem::path& dataset_path,
                           const std::string& file_name,
                           const LanceSchemaMapping& mapping,
                           const std::vector<ColumnValues>& column_values,
                           std::uint64_t rows,
                           int compression_level,
                           bool compress,
                           DataFileResult& result,
                           std::string& error);

}  // namespace nano_lance
