// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <filesystem>
#include <string>

namespace nano_lance {

/// Write the transaction of committing `next` (on top of version `next.version - 1`) to
/// `_transactions/{read_version}-{uuid}.txn`, Lance's name, and return that name.
bool write_transaction_file(const std::filesystem::path& dataset_path, const pb::Manifest& next,
                            std::string& file_name, std::string& error);

/// Remove a transaction file whose commit did not land.
void remove_transaction_file(const std::filesystem::path& dataset_path, const std::string& file_name);

}  // namespace nano_lance
