// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <vector>

/// Keeping a dataset's indices across the commits nanolance makes. nanolance builds only scalar
/// indices (scalar_index.hpp), but a dataset pylance indexed must keep all its indices when nanolance appends to it, deletes from it or
/// changes its columns -- dropping them silently turns every indexed query into a full scan.
///
/// An index claims a set of fields and of fragments (its fragment bitmap). These are Lance's own
/// rules for keeping that claim honest (lance-table's transaction/index_maintenance.rs), for the
/// operations nanolance commits:
///
///   append, delete, update, merge_insert   kept as they are: new fragments are simply not covered,
///                                          and deleted rows are masked by their deletion files
///   add columns                            kept (no indexed field's data changes)
///   drop / retype a column                 an index on a field that is gone is dropped
///   compaction                             the rewritten fragments leave every index's coverage
///                                          (their rows moved; the index points at the old ones)
///   overwrite                              every index is dropped
///
/// Lance scans whatever an index does not cover, so a narrower claim costs speed, never rows.
namespace nano_lance {

/// Lance's own bookkeeping indices (fragment reuse, MemWAL), kept whatever their fields.
bool is_system_index(const pb::IndexMetadata& index);

/// Lance's retain_relevant_indices: drop each index on a field the schema no longer has; of several
/// segments of one index, drop those covering no fragment that still exists (unless all are such,
/// when the oldest stays: an index with nothing to cover is still the dataset's declared index).
void retain_relevant_indices(std::vector<pb::IndexMetadata>& indices, const std::vector<pb::Field>& fields,
                             const std::vector<pb::DataFragment>& fragments);

/// Take `fragment_ids` out of every index's coverage: their rows are no longer where it says.
void drop_fragments_from_indices(std::vector<pb::IndexMetadata>& indices,
                                 const std::vector<std::uint64_t>& fragment_ids);

/// One past the highest fragment id any index covers. A new fragment must not take an id an index
/// already claims -- it would be taken as indexed when it is not.
std::uint64_t first_fragment_id_after_indices(const std::vector<pb::IndexMetadata>& indices);

}  // namespace nano_lance
