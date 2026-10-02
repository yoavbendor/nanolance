# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.optimize``: compaction results."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class CompactionMetrics:
    fragments_removed: int = 0
    fragments_added: int = 0
    files_removed: int = 0
    files_added: int = 0


def __getattr__(name: str):
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.optimize.{name}")
    globals()[name] = value
    return value
