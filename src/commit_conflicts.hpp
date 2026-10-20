// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "dataset_operation.hpp"

namespace nano_lance {

/// Lance's verdict on a change (`ours`) against a transaction committed after the version it was made
/// from (`theirs`): compatible, the change is rebased; retryable, the operation runs again on the newer
/// version; incompatible, the commit fails and is not run again.
enum class Verdict { Compatible, Retryable, Incompatible };

/// Lance 12's TransactionRebase::check_txn (io/commit/conflict_resolver.rs) for transactions without
/// affected rows, as nanolance's commits have: a concurrent change to the same fragments is retryable.
Verdict check_txn(const Operation& ours, const Operation& theirs);

}  // namespace nano_lance
