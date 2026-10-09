# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor

"""``lance.lance``: pylance's native module. nanolance implements the part its Python layer calls
for index building -- ``lance.lance.indices``, the functions ``lance.indices.IndicesBuilder`` uses,
with pylance's names and argument order (callers and tests patch them there). Anything else is a
placeholder that says nanolance does not implement it."""

from __future__ import annotations

import os
import types
from typing import List, Optional

import numpy as np
import pyarrow as pa

from nanolance import _nanolance
from nanolance.lance._errors import native, unsupported

__all__ = ["indices"]


def _metric(distance_type: str) -> str:
    distance_type = str(distance_type).lower()
    if distance_type == "euclidean":
        return "l2"
    if distance_type not in ("l2", "cosine", "dot"):
        raise unsupported(f"the {distance_type} distance (l2, cosine and dot are supported)")
    return distance_type


def _float32_rows(array, dimension: int) -> np.ndarray:
    """A FixedSizeListArray (or a 2-D array) of floats as a float32 [rows][dimension] matrix."""
    if isinstance(array, pa.ChunkedArray):
        array = array.combine_chunks()
    if isinstance(array, (pa.FixedSizeListArray, pa.Array)):
        values = array.flatten().to_numpy(zero_copy_only=False)
    else:
        values = np.asarray(array)
    return np.ascontiguousarray(values, dtype=np.float32).reshape(-1, dimension)


def _fixed_size_list(values: np.ndarray, list_size: int) -> pa.FixedSizeListArray:
    return pa.FixedSizeListArray.from_arrays(pa.array(np.ascontiguousarray(values, np.float32).reshape(-1)),
                                             list_size)


def _fragments(fragment_ids) -> Optional[List[int]]:
    return None if fragment_ids is None else [int(f) for f in fragment_ids]


def train_ivf_model(dataset, column, dimension, num_partitions, distance_type, sample_rate, max_iters,
                    fragment_ids=None) -> pa.FixedSizeListArray:
    """IVF centroids by k-means over a sample of `column` (sample_rate rows per partition)."""
    options = {"metric": _metric(distance_type), "num_partitions": int(num_partitions),
               "sample_rate": int(sample_rate), "max_iters": int(max_iters)}
    with native():
        dim, _, centroids, _ = _nanolance._index_train_model(dataset.uri, dataset.version, str(column), "IVF_FLAT",
                                                             options, _fragments(fragment_ids), None)
    return _fixed_size_list(np.frombuffer(centroids, dtype=np.float32), dim)


def train_pq_model(dataset, column, dimension, num_subvectors, distance_type, sample_rate, max_iters,
                   ivf_centroids, fragment_ids=None, num_bits=8) -> pa.FixedSizeListArray:
    """A PQ codebook on the residuals of `column` from `ivf_centroids`: 2^num_bits rows of
    `dimension`, Lance's flat [num_subvectors][2^num_bits][dimension / num_subvectors] order."""
    centroids = _float32_rows(ivf_centroids, int(dimension))
    options = {"metric": _metric(distance_type), "num_partitions": len(centroids),
               "num_sub_vectors": int(num_subvectors), "num_bits": int(num_bits),
               "sample_rate": int(sample_rate), "max_iters": int(max_iters)}
    with native():
        dim, _, _, codebook = _nanolance._index_train_model(dataset.uri, dataset.version, str(column), "IVF_PQ",
                                                            options, _fragments(fragment_ids), centroids.tobytes())
    return _fixed_size_list(np.frombuffer(codebook, dtype=np.float32), dim)


def _assign(vectors: np.ndarray, centroids: np.ndarray, metric: str) -> np.ndarray:
    if metric == "dot":
        return np.argmax(vectors @ centroids.T, axis=1)
    if metric == "cosine":
        vectors = vectors / np.linalg.norm(vectors, axis=1, keepdims=True)
    distances = (vectors**2).sum(1)[:, None] - 2 * vectors @ centroids.T + (centroids**2).sum(1)[None, :]
    return np.argmin(distances, axis=1)


def _pq_codes(residuals: np.ndarray, codebook: np.ndarray, num_subvectors: int, num_bits: int) -> np.ndarray:
    n, dim = residuals.shape
    width = dim // num_subvectors
    book = codebook.reshape(num_subvectors, 1 << num_bits, width)
    codes = np.empty((n, num_subvectors), dtype=np.uint8)
    for s in range(num_subvectors):
        sub = residuals[:, s * width:(s + 1) * width]
        d = (sub**2).sum(1)[:, None] - 2 * sub @ book[s].T + (book[s] ** 2).sum(1)[None, :]
        codes[:, s] = np.argmin(d, axis=1)
    return codes


def transform_vectors(dataset, column, dimension, num_subvectors, distance_type, ivf_centroids, pq_codebook,
                      dest_uri, fragments, partition_ds_uri=None, num_bits=8) -> None:
    """Each vector of `fragments` as (_rowid, __ivf_part_id, __pq_code), in a Lance file at `dest_uri`."""
    from nanolance.lance.file import LanceFileWriter

    if partition_ds_uri is not None:
        raise unsupported("transform_vectors with precomputed partitions")
    metric = _metric(distance_type)
    dimension = int(dimension)
    centroids = _float32_rows(ivf_centroids, dimension)
    codebook = _float32_rows(pq_codebook, dimension)
    table = dataset.to_table(columns=[column], fragments=[dataset.get_fragment(int(f)) for f in fragments],
                             with_row_id=True)
    vectors = table.column(column).combine_chunks()
    keep = np.asarray(vectors.is_valid())
    rows = _float32_rows(vectors.filter(pa.array(keep)), dimension) if len(vectors) else np.zeros((0, dimension))
    row_ids = table.column("_rowid").to_numpy()[keep]
    parts = _assign(rows, centroids, metric) if len(rows) else np.zeros(0, dtype=np.int64)
    if metric == "cosine":
        rows = rows / np.linalg.norm(rows, axis=1, keepdims=True)
    residuals = rows if metric == "dot" else rows - centroids[parts]
    codes = _pq_codes(residuals, codebook, int(num_subvectors), int(num_bits))
    out = pa.table({
        "_rowid": pa.array(row_ids, pa.uint64()),
        "__ivf_part_id": pa.array(parts.astype(np.uint32), pa.uint32()),
        "__pq_code": pa.FixedSizeListArray.from_arrays(pa.array(codes.reshape(-1), pa.uint8()),
                                                       int(num_subvectors)),
    })
    with LanceFileWriter(str(dest_uri), out.schema) as writer:
        writer.write_batch(out)


def shuffle_transformed_vectors(unsorted_filenames, dir_path, ivf_centroids, shuffle_output_root_filename="sorted"):
    """The transformed rows of `unsorted_filenames` sorted by partition, in one file under `dir_path`;
    its name."""
    from nanolance.lance.file import LanceFileReader, LanceFileWriter

    tables = [LanceFileReader(os.path.join(dir_path, name)).read_all(batch_size=1 << 30).to_table()
              for name in unsorted_filenames]
    table = pa.concat_tables(tables)
    order = np.argsort(table.column("__ivf_part_id").to_numpy(), kind="stable")
    table = table.take(pa.array(order))
    name = f"{shuffle_output_root_filename}_0.lance"
    with LanceFileWriter(os.path.join(dir_path, name), table.schema) as writer:
        writer.write_batch(table)
    return [name]


def load_shuffled_vectors(filenames, dir_path, dataset, column, ivf_centroids, pq_codebook, pq_dimension,
                          num_subvectors, distance_type, index_name=None, num_bits=8) -> None:
    """Commit an IVF_PQ index with these models over the fragments the sorted files hold."""
    from nanolance.lance.file import LanceFileReader

    fragments = set()
    for name in filenames:
        table = LanceFileReader(os.path.join(dir_path, name), columns=["_rowid"]).read_all(
            batch_size=1 << 30).to_table()
        fragments.update(int(r) >> 32 for r in np.unique(table.column("_rowid").to_numpy()))
    dimension = int(pq_dimension)
    centroids = _float32_rows(ivf_centroids, dimension)
    codebook = _float32_rows(pq_codebook, dimension)
    name = index_name or f"{column}_idx"
    options = {"metric": _metric(distance_type), "num_partitions": len(centroids),
               "num_sub_vectors": int(num_subvectors), "num_bits": int(num_bits)}
    with native():
        segment = _nanolance._index_build_segment(dataset.uri, str(column), "IVF_PQ", name, dataset.version,
                                                  sorted(fragments), None, "", options, centroids.tobytes(),
                                                  codebook.tobytes())
        _nanolance._index_commit_segments(dataset.uri, name, str(column), [segment])


indices = types.SimpleNamespace(
    train_ivf_model=train_ivf_model,
    train_pq_model=train_pq_model,
    transform_vectors=transform_vectors,
    shuffle_transformed_vectors=shuffle_transformed_vectors,
    load_shuffled_vectors=load_shuffled_vectors,
)


def __getattr__(name: str):
    from nanolance.lance._stubs import placeholder

    if name.startswith("__"):
        raise AttributeError(name)
    value = placeholder(f"lance.lance.{name}")
    globals()[name] = value
    return value
