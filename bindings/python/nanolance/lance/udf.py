# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.udf``: functions that compute new columns batch by batch (``add_columns`` /
``LanceFragment.merge_columns``), and the checkpoint that lets a failed run resume where it stopped."""

from __future__ import annotations

import os
import pickle
import sqlite3
from contextlib import closing
from typing import Any, Callable, Dict, List, NamedTuple, Optional, Union

import pyarrow as pa

from nanolance.lance.dependencies import _check_for_pandas
from nanolance.lance.dependencies import pandas as pd

__all__ = ["BatchUDF", "BatchUDFCheckpoint", "batch_udf", "normalize_transform"]


class BatchUDF:
    """A function of a RecordBatch returning a RecordBatch (or pandas DataFrame) of new columns."""

    def __init__(self, func: Callable[[pa.RecordBatch], Any], output_schema: Optional[pa.Schema] = None,
                 checkpoint_file=None) -> None:
        self.func = func
        self.output_schema = output_schema
        self.cache: Optional[BatchUDFCheckpoint] = None if checkpoint_file is None else \
            BatchUDFCheckpoint(checkpoint_file)

    def __call__(self, batch: pa.RecordBatch) -> Any:
        return self.func(batch)

    def _call(self, batch: pa.RecordBatch) -> pa.RecordBatch:
        if self.output_schema is None:
            raise ValueError("output_schema must be provided when using a function that returns a RecordBatch")
        result = self.func(batch)
        if _check_for_pandas(result) and isinstance(result, pd.DataFrame):
            result = pa.RecordBatch.from_pandas(result)
        assert result.schema == self.output_schema, (
            f"Output schema of function does not match the expected schema. "
            f"Expected:\n{self.output_schema}\nGot:\n{result.schema}")
        return result


def batch_udf(output_schema: Optional[pa.Schema] = None, checkpoint_file=None):
    """Make a function of batches a BatchUDF: ``@lance.batch_udf(output_schema=...)``. Without an
    ``output_schema`` it is found by calling the function on the first batch. With a
    ``checkpoint_file`` (an SQLite file), ``add_columns`` keeps each batch's result there, so a run
    that failed resumes without computing them again."""

    def inner(func):
        return BatchUDF(func, output_schema, checkpoint_file)

    return inner


class BatchUDFCheckpoint:
    """Results of a UDF kept by (fragment id, batch index), and finished fragments' metadata."""

    class BatchInfo(NamedTuple):
        fragment_id: int
        batch_index: int

    def __init__(self, path):
        self.path = path
        with closing(sqlite3.connect(path)) as conn:
            conn.execute("CREATE TABLE IF NOT EXISTS batches (fragment_id INT, batch_index INT, result BLOB)")
            conn.execute("CREATE TABLE IF NOT EXISTS fragments (fragment_id INT, data BLOB)")
            conn.commit()

    def cleanup(self) -> None:
        os.remove(self.path)

    def get_batch(self, info: "BatchUDFCheckpoint.BatchInfo") -> Optional[pa.RecordBatch]:
        with closing(sqlite3.connect(self.path)) as conn:
            row = conn.execute("SELECT result FROM batches WHERE fragment_id = ? AND batch_index = ?",
                               (info.fragment_id, info.batch_index)).fetchone()
        return None if row is None else pickle.loads(row[0])

    def insert_batch(self, info: "BatchUDFCheckpoint.BatchInfo", batch: pa.RecordBatch) -> None:
        with closing(sqlite3.connect(self.path)) as conn:
            conn.execute("INSERT INTO batches (fragment_id, batch_index, result) VALUES (?, ?, ?)",
                         (info.fragment_id, info.batch_index, pickle.dumps(batch)))
            conn.commit()

    def get_fragment(self, fragment_id: int) -> Optional[str]:
        with closing(sqlite3.connect(self.path)) as conn:
            row = conn.execute("SELECT data FROM fragments WHERE fragment_id = ?", (fragment_id,)).fetchone()
        return None if row is None else row[0]

    def insert_fragment(self, fragment_id: int, fragment: str) -> None:
        with closing(sqlite3.connect(self.path)) as conn:
            conn.execute("INSERT INTO fragments (fragment_id, data) VALUES (?, ?)", (fragment_id, fragment))
            conn.execute("DELETE FROM batches WHERE fragment_id = ?", (fragment_id,))
            conn.commit()


def normalize_transform(udf_like, data_source, read_columns: Optional[List[str]] = None,
                        reader_schema: Optional[pa.Schema] = None):
    """What a new-columns transform is: a BatchUDF (a plain function made one, its output schema from
    its first batch), a dict of SQL expressions, or a reader of the new columns' values."""
    from nanolance.lance.dataset import _coerce_reader

    filtered = None
    with_row_id = with_row_address = False
    if read_columns is not None:
        filtered = [c for c in read_columns if c not in ("_rowid", "_rowaddr")]
        with_row_id = "_rowid" in read_columns
        with_row_address = "_rowaddr" in read_columns

    def first_batch():
        return next(iter(data_source.to_batches(limit=1, columns=filtered, with_row_id=with_row_id,
                                                with_row_address=with_row_address)))

    if isinstance(udf_like, BatchUDF):
        if udf_like.output_schema is None:
            sample = udf_like(first_batch())
            if _check_for_pandas(sample) and isinstance(sample, pd.DataFrame):
                sample = pa.RecordBatch.from_pandas(sample)
            udf_like.output_schema = sample.schema
        return udf_like
    if isinstance(udf_like, dict):
        for k, v in udf_like.items():
            if not isinstance(k, str):
                raise TypeError(f"Column names must be a string. Got {type(k)}")
            if not isinstance(v, str):
                raise TypeError(f"Column expressions must be a string. Got {type(k)}")
        return udf_like
    if callable(udf_like):
        try:
            sample = udf_like(first_batch())
            if _check_for_pandas(sample) and isinstance(sample, pd.DataFrame):
                sample = pa.RecordBatch.from_pandas(sample)
            return BatchUDF(udf_like, output_schema=sample.schema)
        except Exception as inner:
            raise TypeError("transforms must be a BatchUDF, dict, map function, or ReaderLike value.  Received "
                            f"{type(udf_like)}, which is callable, but gave an error when called with a batch of "
                            f"data: {inner}")
    try:
        return _coerce_reader(udf_like, reader_schema)
    except TypeError as inner:
        raise TypeError("transforms must be a BatchUDF, dict, map function, or ReaderLike  value.  Received "
                        f"{type(udf_like)}.  Could not coerce to a reader: {inner}")
