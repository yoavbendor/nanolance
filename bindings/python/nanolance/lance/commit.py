# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.commit``: the error a commit raises when another writer got there first."""

from __future__ import annotations

from contextlib import AbstractContextManager
from typing import Callable

CommitLock = Callable[[int], AbstractContextManager]


class CommitConflictError(OSError):
    """A commit conflicted with a concurrent transaction.

    An ``OSError``, so ``except OSError`` still catches it. ``retryable`` is True when the
    transaction was preempted and can be retried against the newer version, False when the two
    are incompatible and retrying will not help.
    """

    def __init__(self, message: str = "", retryable: bool = True):
        super().__init__(message)
        self.retryable = retryable
