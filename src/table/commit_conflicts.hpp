// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "table/dataset_operation.hpp"

namespace nano_lance {

/// Lance's verdict on a change (`ours`) against a transaction committed after the version it was made
/// from (`theirs`): compatible, the change is rebased; retryable, the operation runs again on the newer
/// version; incompatible, the commit fails and is not run again.
enum class Verdict { Compatible, Retryable, Incompatible };

/// Lance 12's TransactionRebase::check_txn (io/commit/conflict_resolver.rs) for transactions without
/// affected rows, as nanolance's commits have: a concurrent change to the same fragments is retryable.
Verdict check_txn(const Operation& ours, const Operation& theirs);

/// check_txn for a change that has affected rows (a Delete or Update whose deleted rows are known): where
/// check_txn is retryable for an overlap of Delete / Update fragments, Lance's check_delete_txn /
/// check_update_txn is compatible when the other change keeps the data files of those fragments and removes
/// none of them -- the rows overlap is then checked at rebase, against the latest deletion files. `base` is
/// the manifest the change was made from.
Verdict check_txn_with_affected_rows(const Operation& ours, const Operation& theirs, const pb::Manifest& base);

}  // namespace nano_lance
