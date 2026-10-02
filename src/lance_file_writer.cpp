// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/lance_file_writer.hpp"

#include "nanolance/array_accessor.hpp"
#include "nanolance/schema_mapper.hpp"

namespace nano_lance {

bool write_lance_file(const ArrowSchema& schema, const std::vector<const ArrowArray*>& batches,
                      const LanceFileExtras& extras, std::uint64_t& file_size, std::string& error) {
    error.clear();
    file_size = 0;
    if (extras.path.empty()) {
        error = "write_lance_file needs a path";
        return false;
    }
    LanceSchemaMapping mapping;
    if (!map_arrow_schema(schema, mapping, error, true)) {
        return false;
    }
    std::vector<ColumnValues> columns(lance_physical_fields(mapping).size());
    std::uint64_t rows = 0;
    for (const auto* batch : batches) {
        if (batch == nullptr || batch->length == 0) {
            continue;
        }
        if (!append_batch_column_values(*batch, mapping, columns, error)) {
            return false;
        }
        rows += static_cast<std::uint64_t>(batch->length);
    }
    DataFileResult result;
    if (!write_lance_data_file(extras.path.parent_path(), extras.path.filename().string(), mapping, columns, rows, 0,
                               false, result, error, true, &extras)) {
        return false;
    }
    file_size = result.file_size_bytes;
    return true;
}

}  // namespace nano_lance
