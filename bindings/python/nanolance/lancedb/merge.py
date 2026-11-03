# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lancedb.merge``: ``Table.merge_insert(on)`` -- an upsert, its clauses collected and run on ``execute``."""

from __future__ import annotations

from datetime import timedelta
from typing import List, Optional

__all__ = ["LanceMergeInsertBuilder", "AsyncMergeInsertBuilder"]


class LanceMergeInsertBuilder:
    def __init__(self, table, on: List[str]):
        self._table = table
        self._on = on
        self._when_matched_update_all = False
        self._when_matched_update_all_condition: Optional[str] = None
        self._when_not_matched_insert_all = False
        self._when_not_matched_by_source_delete = False
        self._when_not_matched_by_source_condition: Optional[str] = None

    def when_matched_update_all(self, *, where: Optional[str] = None) -> "LanceMergeInsertBuilder":
        self._when_matched_update_all = True
        self._when_matched_update_all_condition = where
        return self

    def when_not_matched_insert_all(self) -> "LanceMergeInsertBuilder":
        self._when_not_matched_insert_all = True
        return self

    def when_not_matched_by_source_delete(self, condition: Optional[str] = None) -> "LanceMergeInsertBuilder":
        self._when_not_matched_by_source_delete = True
        self._when_not_matched_by_source_condition = condition
        return self

    def use_index(self, use_index: bool) -> "LanceMergeInsertBuilder":
        return self

    def execute(self, new_data, on_bad_vectors: str = "error", fill_value: float = 0.0,
                timeout: Optional[timedelta] = None):
        return self._table._do_merge(self, new_data, on_bad_vectors, fill_value, timeout)


class AsyncMergeInsertBuilder(LanceMergeInsertBuilder):
    async def execute(self, new_data, on_bad_vectors: str = "error", fill_value: float = 0.0,
                      timeout: Optional[timedelta] = None):
        import asyncio

        return await asyncio.to_thread(self._table._do_merge, self, new_data, on_bad_vectors, fill_value, timeout)
