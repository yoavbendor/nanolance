#pragma once

#include "nanolance/data_file_writer.hpp"
#include "nanolance/schema_mapper.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace nano_lance {

bool write_dataset_manifest(const std::filesystem::path& dataset_path,
                            const LanceSchemaMapping& mapping,
                            const DataFileResult& data_file,
                            std::uint64_t rows,
                            bool is_append,
                            std::uint64_t& version,
                            std::string& error);

}  // namespace nano_lance
