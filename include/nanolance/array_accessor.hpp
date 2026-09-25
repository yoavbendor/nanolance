// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/column_values.hpp"
#include "nanolance/schema_mapper.hpp"

struct ArrowArray;

namespace nano_lance {

/// Resolve `field`'s Arrow array within `batch` as a REBASED, BORROWED view.
///
/// `out` is a shallow copy whose `offset`/`length` already account for every enclosing array's own
/// slice, so it can be read with the ordinary `array.offset + row` arithmetic. Arrow does not slice
/// a struct's children when the struct is sliced -- the parent carries the offset and the children
/// keep their full extent -- so using a child pointer directly returns the wrong rows and the wrong
/// count for any sliced batch, which `Table.to_batches()` produces by default.
///
/// The view borrows the batch's buffers and children and must NOT be released; its `release` is
/// blanked so an accidental release fails loudly. Returns false when the field has no array.
bool resolve_field_array(const ArrowArray& batch, const LanceSchemaMapping& mapping,
                         const LanceField& field, ArrowArray& out);

bool append_batch_column_values(const ArrowArray& batch,
                                const LanceSchemaMapping& mapping,
                                std::vector<ColumnValues>& columns,
                                std::string& error);

/// Append physical columns, skipping write-side children under `blob_parent_id` when set. With
/// `borrow_fixed_buffers`, a fixed-width (non-bool) column written in a single batch records a view of
/// the caller's Arrow buffer in ColumnValues::fixed_borrowed instead of copying -- the caller must keep
/// the buffer alive and unmodified until the dataset is committed. A second batch for a borrowed column
/// silently materializes it back into the copying path.
bool append_batch_column_values(const ArrowArray& batch,
                                const LanceSchemaMapping& mapping,
                                std::vector<ColumnValues>& columns,
                                std::string& error,
                                std::int32_t skip_blob_parent_id,
                                bool borrow_fixed_buffers = false);

}  // namespace nano_lance
