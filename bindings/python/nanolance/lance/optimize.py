# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.optimize``: compaction results."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, List


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


def __getattr__(name: str):
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.optimize.{name}")
    globals()[name] = value
    return value
