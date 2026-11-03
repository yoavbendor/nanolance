# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lancedb.index``: the index configurations ``AsyncTable.create_index(column, config=...)`` takes."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Literal, Optional

__all__ = ["IvfFlat", "IvfPq", "IvfSq", "IvfRq", "HnswPq", "HnswSq", "HnswFlat", "IvfHnswPq", "IvfHnswSq",
           "IvfHnswFlat", "BTree", "Bitmap", "LabelList", "FTS", "IndexConfig", "IndexStatistics"]


@dataclass
class _Vector:
    distance_type: str = "l2"
    num_partitions: Optional[int] = None
    max_iterations: int = 50
    sample_rate: int = 256
    target_partition_size: Optional[int] = None

    def _pylance(self) -> Dict[str, Any]:
        kw: Dict[str, Any] = {"metric": self.distance_type}
        # k-means tuning goes through only when set: nanolance trains with Lance's defaults (50, 256)
        if self.max_iterations != 50:
            kw["max_iterations"] = self.max_iterations
        if self.sample_rate != 256:
            kw["sample_rate"] = self.sample_rate
        if self.num_partitions is not None:
            kw["num_partitions"] = self.num_partitions
        if self.target_partition_size is not None:
            kw["target_partition_size"] = self.target_partition_size
        return kw


@dataclass
class IvfFlat(_Vector):
    index_type = "IVF_FLAT"


@dataclass
class IvfSq(_Vector):
    index_type = "IVF_SQ"


@dataclass
class IvfRq(_Vector):
    num_bits: int = 1
    index_type = "IVF_RQ"

    def _pylance(self):
        return {**super()._pylance(), "num_bits": self.num_bits}


@dataclass
class IvfPq(_Vector):
    num_sub_vectors: Optional[int] = None
    num_bits: int = 8
    index_type = "IVF_PQ"

    def _pylance(self):
        kw = {**super()._pylance(), "num_bits": self.num_bits}
        if self.num_sub_vectors is not None:
            kw["num_sub_vectors"] = self.num_sub_vectors
        return kw


@dataclass
class _Hnsw(_Vector):
    m: int = 20
    ef_construction: int = 300

    def _pylance(self):
        return {**super()._pylance(), "m": self.m, "ef_construction": self.ef_construction}


@dataclass
class HnswFlat(_Hnsw):
    index_type = "IVF_HNSW_FLAT"


@dataclass
class HnswSq(_Hnsw):
    index_type = "IVF_HNSW_SQ"


@dataclass
class HnswPq(_Hnsw):
    num_sub_vectors: Optional[int] = None
    num_bits: int = 8
    index_type = "IVF_HNSW_PQ"

    def _pylance(self):
        kw = {**super()._pylance(), "num_bits": self.num_bits}
        if self.num_sub_vectors is not None:
            kw["num_sub_vectors"] = self.num_sub_vectors
        return kw


IvfHnswFlat, IvfHnswSq, IvfHnswPq = HnswFlat, HnswSq, HnswPq


@dataclass
class BTree:
    index_type = "BTREE"


@dataclass
class Bitmap:
    index_type = "BITMAP"


@dataclass
class LabelList:
    index_type = "LABEL_LIST"


@dataclass
class FTS:
    with_position: bool = False
    base_tokenizer: Literal["simple", "raw", "whitespace", "ngram", "jieba/default", "lindera/ipadic"] = "simple"
    language: str = "English"
    max_token_length: Optional[int] = 40
    lower_case: bool = True
    stem: bool = True
    remove_stop_words: bool = True
    ascii_folding: bool = True
    ngram_min_length: int = 3
    ngram_max_length: int = 3
    prefix_only: bool = False
    index_type = "INVERTED"

    def _pylance(self) -> Dict[str, Any]:
        kw = {k: getattr(self, k) for k in ("with_position", "base_tokenizer", "language", "max_token_length",
                                            "lower_case", "stem", "remove_stop_words", "ascii_folding",
                                            "ngram_min_length", "ngram_max_length", "prefix_only")}
        return kw


@dataclass
class IndexConfig:
    """One index of a table (``AsyncTable.list_indices``)."""

    name: str
    index_type: str
    columns: List[str] = field(default_factory=list)

    def __repr__(self) -> str:
        return f"Index({self.index_type}, columns={self.columns}, name=\"{self.name}\")"


@dataclass
class IndexStatistics:
    num_indexed_rows: int
    num_unindexed_rows: int
    index_type: str
    distance_type: Optional[str] = None
    num_indices: Optional[int] = None
    loss: Optional[float] = None
