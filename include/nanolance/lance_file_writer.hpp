// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/data_file_writer.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance {

/// Arrow batches of one schema as one standalone Lance file (format 2.2) at `extras.path`, with the
/// schema metadata and global buffers `extras` carries -- what Lance's index files are. `file_size`
/// is the size written.
bool write_lance_file(const ArrowSchema& schema, const std::vector<const ArrowArray*>& batches,
                      const LanceFileExtras& extras, std::uint64_t& file_size, std::string& error);

}  // namespace nano_lance
