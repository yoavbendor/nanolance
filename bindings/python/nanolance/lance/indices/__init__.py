# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor

"""``lance.indices``: IndicesBuilder (training IVF and PQ models for distributed vector index
builds), the models, and pylance's enums."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Set

from nanolance.lance.indices.builder import IndexConfig, IndicesBuilder
from nanolance.lance.indices.ivf import IvfModel
from nanolance.lance.indices.pq import PqModel

__all__ = [
    "IndicesBuilder",
    "IndexConfig",
    "PqModel",
    "IvfModel",
    "IndexFileVersion",
    "IndexSegment",
    "IndexSegmentDescription",
]


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


@dataclass
class IndexSegment:
    """One physical segment of an index: its UUID, the fragments it covers, its format version.
    ``commit_existing_index_segments`` takes these, or ``Index`` objects."""

    uuid: str
    fragment_ids: Set[int]
    index_version: int
    _nl_message: bytes = b""  # the IndexMetadata message, when nanolance made it


def __getattr__(name: str):
    if name == "IndexSegmentDescription":
        from nanolance.lance.dataset import IndexSegmentDescription

        return IndexSegmentDescription
    from nanolance.lance._stubs import placeholder

    if name.startswith("__"):
        raise AttributeError(name)
    value = placeholder(f"lance.indices.{name}")
    globals()[name] = value
    return value
