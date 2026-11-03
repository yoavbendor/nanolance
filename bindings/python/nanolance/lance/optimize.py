# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.optimize``: compaction results."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, List, Optional, TypedDict


@dataclass
class CompactionMetrics:
    fragments_removed: int = 0
    fragments_added: int = 0
    files_removed: int = 0
    files_added: int = 0


@dataclass(repr=False)
class RewriteResult:
    """One compaction task rewritten (``CompactionTask.execute``): the fragments it removed and the new ones,
    written but not committed. ``Compaction.commit`` commits a list of them."""

    read_version: int
    new_fragments: List[Any]
    old_fragments: List[Any]
    metrics: CompactionMetrics = field(default_factory=CompactionMetrics)
    _messages: List[bytes] = field(default_factory=list)  # the new fragments as lance.table.DataFragment

    def __repr__(self) -> str:
        return (f"RewriteResult(read_version={self.read_version}, new_fragments={self.new_fragments!r}, "
                f"old_fragments={self.old_fragments!r})")


class CompactionOptions(TypedDict, total=False):
    """Options for ``Compaction.plan`` / ``DatasetOptimizer.compact_files``, as pylance names them."""

    target_rows_per_fragment: Optional[int]
    max_rows_per_group: Optional[int]
    max_bytes_per_file: Optional[int]
    materialize_deletions: Optional[bool]
    materialize_deletions_threshold: Optional[float]
    num_threads: Optional[int]
    batch_size: Optional[int]
    io_buffer_size: Optional[int]
    defer_index_remap: Optional[bool]
    max_source_fragments: Optional[int]
    max_source_rows: Optional[int]
    max_source_bytes: Optional[int]
    excluded_fragment_ids: Optional[List[int]]


# pylance re-exports the step-by-step compaction API here (`from lance.optimize import Compaction`); lance.lance
# imports this module only inside functions, so the import below makes no cycle.
from nanolance.lance.lance import Compaction, CompactionPlan, CompactionTask  # noqa: E402


def __getattr__(name: str):
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.optimize.{name}")
    globals()[name] = value
    return value
