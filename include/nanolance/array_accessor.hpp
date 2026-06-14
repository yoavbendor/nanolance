// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/column_values.hpp"
#include "nanolance/schema_mapper.hpp"

struct ArrowArray;

namespace nano_lance {

const ArrowArray* resolve_field_array(const ArrowArray& batch,
                                      const LanceSchemaMapping& mapping,
                                      const LanceField& field);

bool append_batch_column_values(const ArrowArray& batch,
                                const LanceSchemaMapping& mapping,
                                std::vector<ColumnValues>& columns,
                                std::string& error);

/// Append physical columns, skipping write-side children under `blob_parent_id` when set.
bool append_batch_column_values(const ArrowArray& batch,
                                const LanceSchemaMapping& mapping,
                                std::vector<ColumnValues>& columns,
                                std::string& error,
                                std::int32_t skip_blob_parent_id);

}  // namespace nano_lance
