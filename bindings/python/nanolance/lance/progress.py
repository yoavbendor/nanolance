# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.progress``: the write progress callback interface (accepted; nanolance does not call it)."""

from __future__ import annotations

from abc import ABC


class FragmentWriteProgress(ABC):
    def begin(self, fragment, **kwargs) -> None:  # pragma: no cover - an interface
        pass

    def complete(self, fragment) -> None:  # pragma: no cover - an interface
        pass


class NoopFragmentWriteProgress(FragmentWriteProgress):
    pass


class IndexProgress:  # pragma: no cover - indexes are not supported
    pass


def __getattr__(name: str):
    """pylance names this module does not implement import as placeholders (see _stubs.py)."""
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.progress.{name}")
    globals()[name] = value
    return value
