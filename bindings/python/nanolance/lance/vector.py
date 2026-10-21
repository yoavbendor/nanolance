# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.vector``: building tables of vectors. ``vec_to_table`` follows pylance's (Apache-2.0,
the Lance authors) argument for argument; the accelerator (torch) training helpers are pylance's alone."""

from __future__ import annotations

import os
import uuid
from typing import Iterable, List, Optional, Union

import numpy as np
import pyarrow as pa

from nanolance import _nanolance
from nanolance.lance._errors import native

__all__ = ["vec_to_table", "hamming_clustering_for_sample", "get_ivf_partition_info",
           "hamming_clustering_for_ivf_partition"]


def _normalize_vectors(vectors, ndim):
    if ndim is None:
        ndim = len(next(iter(vectors)))
    values = np.array(vectors, dtype="float32").ravel()
    return pa.FixedSizeListArray.from_arrays(values, list_size=ndim)


def _validate_ndim(values, ndim):
    for v in values:
        if ndim is None:
            ndim = len(v)
        elif ndim != len(v):
            raise ValueError(f"Expected {ndim} dimensions but got {len(v)} for {v}")
    return ndim


def vec_to_table(data: Union[dict, list, np.ndarray], names: Optional[Union[str, list]] = None,
                 ndim: Optional[int] = None, check_ndim: bool = True) -> pa.Table:
    """A table of float32 fixed-size-list vectors: a dict's keys become an ``id`` column; a list's
    or an ndarray's rows are the vectors. Mirrors ``lance.vector.vec_to_table``."""
    if isinstance(data, dict):
        if names is None:
            names = ["id", "vector"]
        elif not isinstance(names, (list, tuple)) and len(names) == 2:
            raise ValueError("If data is a dict, names must be a list or tuple of 2 strings")
        values = list(data.values())
        if check_ndim:
            ndim = _validate_ndim(values, ndim)
        arrays = [pa.array(data.keys()), _normalize_vectors(values, ndim)]
    elif isinstance(data, (list, np.ndarray)):
        if names is None:
            names = ["vector"]
        elif isinstance(names, str):
            names = [names]
        elif not isinstance(names, (list, tuple)) and len(names) == 1:
            raise ValueError(f"names cannot be more than 1 got {len(names)}")
        if check_ndim:
            ndim = _validate_ndim(data, ndim)
        arrays = [_normalize_vectors(data, ndim)]
    else:
        raise NotImplementedError(f"data must be dict, list, or ndarray, got {type(data)} instead")
    return pa.Table.from_arrays(arrays, names=names)


_POPCOUNT = np.array([bin(i).count("1") for i in range(256)], dtype=np.uint16)


def hamming_clustering_for_sample(dataset, column: str, sample_size: Optional[int],
                                  threshold: int) -> pa.RecordBatchReader:
    """Group the rows of a binary (fixed-size uint8) column whose Hamming distance is at most
    ``threshold`` bits. Rows are taken in row-id order (all of them, or ``sample_size`` drawn at random),
    and each unassigned row starts a cluster with every later unassigned row within the threshold.

    Returns a reader of ``representative`` (uint64, the first row of the cluster) and ``duplicates``
    (list<uint64>, the other rows, in order). Clusters of one row are not emitted. Null vectors are skipped.
    """
    table = dataset.to_table(columns=[column], with_row_id=True)
    if sample_size is not None and sample_size < table.num_rows:
        keep = np.sort(np.random.default_rng().choice(table.num_rows, size=int(sample_size), replace=False))
        table = table.take(pa.array(keep, type=pa.int64()))
    vectors = table[column].combine_chunks()
    valid = np.asarray(vectors.is_valid())
    width = vectors.type.list_size
    flat = np.asarray(vectors.values.slice(vectors.offset * width, len(vectors) * width), dtype=np.uint8)
    rows = flat.reshape(-1, width)
    ids = np.asarray(table["_rowid"].to_numpy(), dtype=np.uint64)
    rows, ids = rows[valid], ids[valid]
    return _cluster_hamming(ids, rows, threshold)


def _cluster_hamming(ids: np.ndarray, rows: np.ndarray, threshold: int) -> pa.RecordBatchReader:
    """Lance's clustering: every pair of rows within ``threshold`` bits is an edge, and the clusters are the
    connected components (union-find). A cluster's representative is its least row id; its duplicates are the
    others, ascending; clusters of one row are not emitted; clusters are in representative order."""
    parent = list(range(len(ids)))

    def find(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for i in range(len(ids) - 1):
        bits = _POPCOUNT[np.bitwise_xor(rows[i + 1:], rows[i])].sum(axis=1)
        for j in np.nonzero(bits <= threshold)[0] + i + 1:
            a, b = find(i), find(int(j))
            if a != b:
                parent[max(a, b)] = min(a, b)
    groups = {}
    for i in range(len(ids)):
        groups.setdefault(find(i), []).append(int(ids[i]))
    clusters = sorted((sorted(g) for g in groups.values() if len(g) > 1), key=lambda g: g[0])
    schema = pa.schema([("representative", pa.uint64()), ("duplicates", pa.list_(pa.uint64()))])
    batch = pa.RecordBatch.from_arrays(
        [pa.array([g[0] for g in clusters], type=pa.uint64()),
         pa.array([g[1:] for g in clusters], type=pa.list_(pa.uint64()))],
        schema=schema)
    return pa.RecordBatchReader.from_batches(schema, [batch])


def _segment_dirs(dataset, index_name: str, index_segments) -> List[str]:
    """The directories (_indices/<uuid>) of the named index's segments: all of them, or the ones asked for."""
    if isinstance(index_segments, (str, uuid.UUID)):
        raise TypeError("index_segments must be a list of str or uuid.UUID, not a single value")
    segments = [i for i in dataset.list_indices() if i["name"] == index_name]
    if not segments:
        raise KeyError(f"Index {index_name} not found")
    if index_segments is not None:
        wanted = []
        for s in index_segments:
            if not isinstance(s, (str, uuid.UUID)):
                raise TypeError(f"index_segments entries must be str or uuid.UUID, got {type(s).__name__}")
            try:
                wanted.append(str(uuid.UUID(str(s))))
            except ValueError as error:
                raise ValueError(f"invalid index segment uuid: {s!r}") from error
        known = {str(s["uuid"]) for s in segments}
        for s in wanted:
            if s not in known:
                raise ValueError(f"invalid index segment uuid: {s} is not a segment of index {index_name}")
        segments = [s for s in segments if str(s["uuid"]) in set(wanted)]
    return [os.path.join(dataset.uri, "_indices", str(s["uuid"])) for s in segments]


def _partition_sizes(directory: str) -> List[int]:
    with native():
        return [int(n) for n in _nanolance._vector_index_model(directory)["partition_sizes"]]


def get_ivf_partition_info(dataset, index_name: str, *, index_segments: Optional[Iterable] = None) -> List[dict]:
    """The rows of each partition of an IVF_FLAT index, summed over its segments (or the ones given).
    Returns ``[{"partition_id": i, "size": n}, ...]``. Mirrors ``lance.vector.get_ivf_partition_info``."""
    directories = _segment_dirs(dataset, index_name, index_segments)
    totals = None
    for directory in directories:
        sizes = _partition_sizes(directory)
        totals = sizes if totals is None else [a + b for a, b in zip(totals, sizes)]
    return [{"partition_id": p, "size": n} for p, n in enumerate(totals or [])]


def hamming_clustering_for_ivf_partition(dataset, index_name: str, partition_id: int, hamming_threshold: int, *,
                                         index_segments: Optional[Iterable] = None) -> pa.RecordBatchReader:
    """Hamming clustering over one partition of a Hamming IVF_FLAT index: the partition's rows of every selected
    segment (or the ones given) are compared pairwise, and the rows within ``hamming_threshold`` bits are
    clustered by union-find. Returns a reader of ``representative`` and ``duplicates`` as
    ``hamming_clustering_for_sample`` does. Mirrors ``lance.vector.hamming_clustering_for_ivf_partition``."""
    directories = _segment_dirs(dataset, index_name, index_segments)
    count = len(_partition_sizes(directories[0])) if directories else 0
    if not 0 <= partition_id < count:
        raise ValueError(f"Invalid user input: Partition ID {partition_id} is out of range (0..{count})")
    ids, rows = [], []
    for directory in directories:
        with native():
            part = _nanolance._vector_index_partition(directory, int(partition_id), True)
        if not part["uint8"]:
            raise ValueError(f"Index {index_name} is not a Hamming index over uint8 vectors")
        ids.append(np.frombuffer(part["ids"], dtype=np.uint64))
        rows.append(np.frombuffer(part["values"], dtype=np.uint8).reshape(-1, part["width"]))
    if not ids:
        return _cluster_hamming(np.zeros(0, dtype=np.uint64), np.zeros((0, 0), dtype=np.uint8), hamming_threshold)
    return _cluster_hamming(np.concatenate(ids), np.concatenate(rows), hamming_threshold)


def __getattr__(name: str):
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    return placeholder(f"lance.vector.{name}")
