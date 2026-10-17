# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.vector``: building tables of vectors. ``vec_to_table`` follows pylance's (Apache-2.0,
the Lance authors) argument for argument; the accelerator (torch) training helpers are pylance's alone."""

from __future__ import annotations

from typing import Optional, Union

import numpy as np
import pyarrow as pa

__all__ = ["vec_to_table", "hamming_clustering_for_sample"]


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
    done = np.zeros(len(ids), dtype=bool)
    representatives, duplicates = [], []
    for i in range(len(ids)):
        if done[i]:
            continue
        done[i] = True
        rest = np.nonzero(~done[i + 1:])[0] + i + 1
        if rest.size == 0:
            continue
        bits = _POPCOUNT[np.bitwise_xor(rows[rest], rows[i])].sum(axis=1)
        members = rest[bits <= threshold]
        if members.size:
            done[members] = True
            representatives.append(int(ids[i]))
            duplicates.append(ids[members].tolist())
    schema = pa.schema([("representative", pa.uint64()), ("duplicates", pa.list_(pa.uint64()))])
    batch = pa.RecordBatch.from_arrays(
        [pa.array(representatives, type=pa.uint64()), pa.array(duplicates, type=pa.list_(pa.uint64()))],
        schema=schema)
    return pa.RecordBatchReader.from_batches(schema, [batch])


def __getattr__(name: str):
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    return placeholder(f"lance.vector.{name}")
