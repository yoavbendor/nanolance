#pragma once

#include "lance_minimal.pb.hpp"
#include "nanolance/column_values.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace nano_lance {

/// Decode one physical Lance column from a data file (writer encodings only).
bool decode_lance_physical_column(const std::filesystem::path& data_file_path, const pb::Field& on_disk_field,
                                  const pb::ColumnMetadata& column_metadata, ColumnValues& out, std::string& error);

}  // namespace nano_lance
