#pragma once

#include "nano_lance_writer/column_values.hpp"
#include "nano_lance_writer/schema_mapper.hpp"

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
                           DataFileResult& result,
                           std::string& error);

}  // namespace nano_lance
