# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.progress``: the write progress callback interface (accepted; nanolance does not call it),
and the index progress events (``IndexProgress``) index merges report."""

from __future__ import annotations

from abc import ABC
from dataclasses import dataclass
from typing import Literal, Optional


class FragmentWriteProgress(ABC):
    def begin(self, fragment, **kwargs) -> None:  # pragma: no cover - an interface
        pass

    def complete(self, fragment) -> None:  # pragma: no cover - an interface
        pass


class NoopFragmentWriteProgress(FragmentWriteProgress):
    pass


@dataclass(frozen=True)
class IndexProgress:
    """A progress event while an index is built or merged: ``event`` "start", "progress" or
    "complete" of ``stage``, with ``completed`` of ``total`` ``unit`` when known."""

    event: Literal["start", "progress", "complete"]
    stage: str
    completed: Optional[int] = None
    total: Optional[int] = None
    unit: str = ""

    @property
    def fraction(self) -> Optional[float]:
        if self.completed is None or self.total in (None, 0):
            return None
        return min(self.completed / self.total, 1.0)


def __getattr__(name: str):
    """pylance names this module does not implement import as placeholders (see _stubs.py)."""
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.progress.{name}")
    globals()[name] = value
    return value
