// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "table/dataset_operation.hpp"
#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nano_lance {

/// C2: rebase `ours` -- a change whose operation `op` was made from its base, as its transaction file
/// records it -- onto `latest`, the manifest of the version the other writers left, when every change
/// `theirs` committed since is compatible with `op` (Lance's rebase). `next` gets the manifest of that
/// change applied to `latest`: apply_operation, with Lance's finish_create_index pruning. Its version,
/// timestamp and writer fields are left to the caller. False with `error` when the change cannot be
/// rebased (the caller re-runs the operation).
bool rebase_operation(const pb::Manifest& latest, const pb::Manifest& ours, const Operation& op,
                      const std::vector<Operation>& theirs, std::uint64_t new_version, pb::Manifest& next,
                      std::string& error);

}  // namespace nano_lance
