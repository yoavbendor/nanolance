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
from nanolance.lance._errors import native, refuse_object_store_write, unsupported

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


def _fixed_size_list(values: np.ndarray, list_size: int, dtype=np.float32) -> pa.FixedSizeListArray:
    return pa.FixedSizeListArray.from_arrays(pa.array(np.ascontiguousarray(values, dtype).reshape(-1)), list_size)


def _fragments(fragment_ids) -> Optional[List[int]]:
    return None if fragment_ids is None else [int(f) for f in fragment_ids]


def train_ivf_model(dataset, column, dimension, num_partitions, distance_type, sample_rate, max_iters,
                    fragment_ids=None) -> pa.FixedSizeListArray:
    """IVF centroids by k-means over a sample of `column` (sample_rate rows per partition); k-modes over a
    uint8 column with distance_type "hamming" (uint8 centroids)."""
    hamming = str(distance_type).lower() == "hamming"
    options = {"metric": "hamming" if hamming else _metric(distance_type), "num_partitions": int(num_partitions),
               "sample_rate": int(sample_rate), "max_iters": int(max_iters)}
    with native():
        dim, _, centroids, _ = _nanolance._index_train_model(dataset.uri, dataset.version, str(column), "IVF_FLAT",
                                                             options, _fragments(fragment_ids), None)
    if hamming:
        return _fixed_size_list(np.frombuffer(centroids, dtype=np.uint8), dim, np.uint8)
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
    refuse_object_store_write(dataset.uri, "load shuffled vectors")
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


def build_rq_model(dimension: int, num_bits: int = 5) -> str:
    """pylance's indices.build_rq_model: a RaBitQ model (JSON) for IVF_RQ builds that share one rotation (the
    rabitq_model of create_index_uncommitted; segments built with the same model merge). Fast rotation only."""
    import json

    dimension = int(dimension)
    if dimension <= 0 or dimension % 8:
        raise ValueError("dimension must be divisible by 8 for IVF_RQ")
    signs = [int(b) for b in os.urandom(4 * (dimension // 8))]
    return json.dumps({"rotate_mat_position": None, "fast_rotation_signs": signs, "rotation_type": "fast",
                       "code_dim": dimension, "num_bits": int(num_bits), "packed": False,
                       "query_estimator": "raw_query"})


indices = types.SimpleNamespace(
    build_rq_model=build_rq_model,
    train_ivf_model=train_ivf_model,
    train_pq_model=train_pq_model,
    transform_vectors=transform_vectors,
    shuffle_transformed_vectors=shuffle_transformed_vectors,
    load_shuffled_vectors=load_shuffled_vectors,
)


# ── compaction: pylance's Compaction.plan / CompactionPlan / CompactionTask / Compaction.commit ──────────────


def _fragments_by_id(uri: str, version: int):
    from nanolance.lance._transactions import _fragment_id
    from nanolance.lance.fragment import _decode_fragment

    with native():
        messages = _nanolance._ds_fragment_messages(uri, int(version))
    return {_fragment_id(m): _decode_fragment(bytes(m)) for m in messages}


class CompactionTask:
    """One bin of a plan: the fragments one rewrite takes. ``execute`` writes them; nothing is committed."""

    def __init__(self, uri: str, read_version: int, fragment_ids: List[int], fragments, max_bytes_per_file: int,
                 batch_size: int):
        self._uri = uri
        self.read_version = read_version
        self.fragment_ids = fragment_ids
        self.fragments = fragments
        self._max_bytes_per_file = max_bytes_per_file
        self._batch_size = batch_size

    def __eq__(self, other) -> bool:
        return (isinstance(other, CompactionTask) and self.read_version == other.read_version
                and self.fragment_ids == other.fragment_ids and self.fragments == other.fragments)

    def __repr__(self) -> str:
        return f"CompactionTask(read_version={self.read_version}, fragments={self.fragment_ids})"

    def execute(self, dataset) -> "RewriteResult":
        from nanolance.lance._errors import refuse_object_store_write
        from nanolance.lance.fragment import _decode_fragment
        from nanolance.lance.optimize import CompactionMetrics, RewriteResult

        refuse_object_store_write(dataset.uri, "compact files")
        with native():
            old_ids, messages = _nanolance._ds_compact_execute(self._uri, self.read_version, self.fragment_ids,
                                                               self._max_bytes_per_file, self._batch_size)
        messages = [bytes(m) for m in messages]
        new = [_decode_fragment(m) for m in messages]
        old = list(self.fragments)
        metrics = CompactionMetrics(
            fragments_removed=len(old), fragments_added=len(new),
            files_removed=sum(len(f.files) + (f.deletion_file is not None) for f in old), files_added=len(new))
        return RewriteResult(read_version=self.read_version, new_fragments=new, old_fragments=old, metrics=metrics,
                             _messages=messages)


class CompactionPlan:
    """The tasks a compaction would run on one version (``Compaction.plan``)."""

    def __init__(self, read_version: int, tasks: List[CompactionTask]):
        self.read_version = read_version
        self.tasks = tasks

    def num_tasks(self) -> int:
        return len(self.tasks)

    def __eq__(self, other) -> bool:
        return (isinstance(other, CompactionPlan) and self.read_version == other.read_version
                and self.tasks == other.tasks)

    def __repr__(self) -> str:
        return f"CompactionPlan(read_version={self.read_version}, tasks=<{len(self.tasks)} compaction tasks>)"


class Compaction:
    """Compaction in steps: ``plan`` the tasks, ``execute`` each (in any process), ``commit`` the results."""

    @staticmethod
    def plan(dataset, options: Optional[dict] = None) -> CompactionPlan:
        from nanolance.lance._errors import refuse_object_store_write

        refuse_object_store_write(dataset.uri, "compact files")
        o = dict(options or {})

        def opt(name, default):
            return default if o.get(name) is None else o[name]

        def limit(name):
            return None if o.get(name) is None else int(o[name])

        with native():
            read_version, tasks = _nanolance._ds_compact_plan(
                dataset.uri, int(opt("target_rows_per_fragment", 1024 * 1024)),
                bool(opt("materialize_deletions", True)), float(opt("materialize_deletions_threshold", 0.1)),
                limit("max_source_fragments"), limit("max_source_rows"), limit("max_source_bytes"),
                sorted({int(i) for i in (o.get("excluded_fragment_ids") or [])}),
                int(o.get("max_bytes_per_file") or 0), int(o.get("batch_size") or 0))
        by_id = _fragments_by_id(dataset.uri, read_version)
        max_bytes, batch = int(o.get("max_bytes_per_file") or 0), int(o.get("batch_size") or 0)
        return CompactionPlan(read_version, [
            CompactionTask(dataset.uri, read_version, list(ids), [by_id[i] for i in ids], max_bytes, batch)
            for ids in tasks])

    @staticmethod
    def commit(dataset, rewrites, options: Optional[dict] = None):
        """Commit rewritten tasks as one version (``options``: ``defer_index_remap``, ``reindex``). Returns the
        CompactionMetrics. A writer that changed a rewritten fragment meanwhile makes it a conflict."""
        from nanolance.lance._errors import refuse_object_store_write
        from nanolance.lance.optimize import CompactionMetrics

        refuse_object_store_write(dataset.uri, "compact files")
        rewrites = list(rewrites)
        if not rewrites:
            return CompactionMetrics()
        o = dict(options or {})
        versions = {r.read_version for r in rewrites}
        if len(versions) != 1:
            raise ValueError(f"the rewrites were made from different versions: {sorted(versions)}")
        with native():
            metrics, _ = _nanolance._ds_compact_commit(
                dataset.uri, rewrites[0].read_version, [[int(f.id) for f in r.old_fragments] for r in rewrites],
                [list(r._messages) for r in rewrites], bool(o.get("defer_index_remap", False)),
                bool(o.get("reindex", True)))
        dataset._refresh_latest("Rewrite")
        return CompactionMetrics(**{k: int(v) for k, v in metrics.items() if not k.startswith("indexes_")})


def __getattr__(name: str):
    from nanolance.lance._stubs import placeholder

    if name.startswith("__"):
        raise AttributeError(name)
    value = placeholder(f"lance.lance.{name}")
    globals()[name] = value
    return value
