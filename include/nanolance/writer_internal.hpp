// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/manifest_writer.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/schema_mapper.hpp"

#include <cstdint>
#include <string>
#include <vector>

/// Hooks into a staged writer (NanoLanceWriteOptions::stage_fragments) for operations that commit
/// their own manifest: an update or merge writes new fragments and changes old ones in one version;
/// adding columns writes one new file per existing fragment.
namespace nano_lance {

/// Number the field ids of the schema the first batch sets from `first_id` (after a dataset's own).
bool writer_set_field_id_base(NanoLanceWriter* writer, std::int32_t first_id, std::string& error);

/// Commit what is pending (a batch of no rows too, with `keep_empty`) and hand over every staged data
/// file, with the schema they were written with, instead of publishing them.
bool writer_take_staged(NanoLanceWriter* writer, std::vector<NewFragment>& out, LanceSchemaMapping& mapping,
                        std::string& error, bool keep_empty = false);

}  // namespace nano_lance
