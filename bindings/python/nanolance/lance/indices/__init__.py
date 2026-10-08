# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor

"""``lance.indices``: the enums pylance defines here; the index builders are not implemented."""

from __future__ import annotations

from enum import Enum


class IndexFileVersion(str, Enum):
    LEGACY = "Legacy"
    V3 = "V3"


class SupportedDistributedIndices(str, Enum):
    # Scalar index types
    BTREE = "BTREE"
    INVERTED = "INVERTED"

    # Precise vector index types supported by distributed merge
    IVF_FLAT = "IVF_FLAT"
    IVF_PQ = "IVF_PQ"
    IVF_SQ = "IVF_SQ"

    # Deprecated generic placeholder (kept for backward compatibility)
    VECTOR = "VECTOR"


def __getattr__(name: str):
    from nanolance.lance._stubs import placeholder

    if name.startswith("__"):
        raise AttributeError(name)
    value = placeholder(f"lance.indices.{name}")
    globals()[name] = value
    return value
