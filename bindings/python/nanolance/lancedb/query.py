# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lancedb.query``: the fluent query builders, sync (``LanceQueryBuilder``) and async (``AsyncQuery`` and
``AsyncVectorQuery``). A builder collects its settings and runs one ``to_table`` on the terminal call."""

from __future__ import annotations

from datetime import timedelta
from typing import Any, Dict, List, Optional, Union

import numpy as np
import pyarrow as pa

__all__ = ["LanceQueryBuilder", "LanceVectorQueryBuilder", "LanceFtsQueryBuilder", "AsyncQuery",
           "AsyncVectorQuery", "AsyncFTSQuery", "AsyncRecordBatchReader"]


class _Builder:
    """The settings of one query; ``_run`` executes them on the table's current version."""

    _default_limit: Optional[int] = 10
    _default_vector_limit: int = 10

    def __init__(self, table):
        self._table = table
        self._vector = None
        self._column: Optional[str] = None
        self._metric: Optional[str] = None
        self._limit: Optional[int] = None
        self._limit_set = False
        self._offset: Optional[int] = None
        self._where: Optional[str] = None
        self._prefilter = True
        self._select: Union[None, List[str], Dict[str, str]] = None
        self._with_row_id = False
        self._with_row_address = False
        self._nprobes: Optional[int] = None
        self._minimum_nprobes: Optional[int] = None
        self._maximum_nprobes: Optional[int] = None
        self._refine_factor: Optional[int] = None
        self._ef: Optional[int] = None
        self._bypass_index = False
        self._distance_range = None
        self._fts = None
        self._fts_columns = None
        self._take_offsets = None
        self._take_row_ids = None
        self._fast_search = False

    # -- shared fluent setters

    def where(self, predicate: str, prefilter: Optional[bool] = None):
        self._where = predicate
        if prefilter is not None:
            self._prefilter = bool(prefilter)
        return self

    def select(self, columns: Union[List[str], Dict[str, str], str]):
        self._select = [columns] if isinstance(columns, str) else (
            dict(columns) if isinstance(columns, dict) else list(columns))
        return self

    def limit(self, limit: Optional[int]):
        if limit is not None and (not isinstance(limit, int) or limit < 0):
            raise ValueError("Limit must be non-negative")
        self._limit, self._limit_set = limit, True
        return self

    def offset(self, offset: int):
        if offset is not None and offset < 0:
            raise ValueError("Offset must be a non-negative integer")
        self._offset = offset
        return self

    def postfilter(self):
        self._prefilter = False
        return self

    def with_row_id(self, with_row_id: bool = True):
        self._with_row_id = bool(with_row_id)
        return self

    def with_row_address(self, with_row_address: bool = True):
        self._with_row_address = bool(with_row_address)
        return self

    def fast_search(self):
        self._fast_search = True
        return self

    # vector settings
    def nearest_to(self, query_vector):
        if query_vector is None:
            raise ValueError("query_vector can not be None")
        self._vector = query_vector
        return self

    def column(self, column: Optional[str]):
        self._column = column
        return self

    def distance_type(self, distance_type: str):
        self._metric = str(distance_type).lower()
        return self

    metric = distance_type

    def nprobes(self, nprobes: int):
        self._nprobes = int(nprobes)
        return self

    def minimum_nprobes(self, minimum_nprobes: int):
        self._minimum_nprobes = int(minimum_nprobes)
        return self

    def maximum_nprobes(self, maximum_nprobes: int):
        self._maximum_nprobes = int(maximum_nprobes)
        return self

    def refine_factor(self, refine_factor: int):
        self._refine_factor = int(refine_factor)
        return self

    def ef(self, ef: int):
        self._ef = int(ef)
        return self

    def bypass_vector_index(self):
        self._bypass_index = True
        return self

    def distance_range(self, lower_bound: Optional[float] = None, upper_bound: Optional[float] = None):
        self._distance_range = (lower_bound, upper_bound)
        return self

    def _fts(self, query, columns=None):
        self._fts = query
        self._fts_columns = [columns] if isinstance(columns, str) else columns
        return self

    def _take(self, offsets=None, row_ids=None):
        self._take_offsets, self._take_row_ids = offsets, row_ids
        return self

    # -- execution

    def _queries(self, schema: pa.Schema):
        """The query vectors as float32 arrays, and the column they search. A 2-D query on a single-vector column
        is several queries (a ``query_index`` column tells their rows apart)."""
        from nanolance.lancedb.table import _vector_column

        q = self._vector
        if isinstance(q, (pa.Array, pa.ChunkedArray)):
            q = q.to_pylist()
        arr = np.asarray(q, dtype=np.float32) if not isinstance(q, np.ndarray) else q.astype(np.float32, copy=False)
        dim = arr.shape[-1] if arr.ndim else None
        column = self._column or _vector_column(schema, dim)
        t = schema.field(column).type
        multivector = not pa.types.is_fixed_size_list(t)
        if arr.ndim == 2 and not multivector:
            return [row for row in arr], column, True
        return [arr], column, False

    def _run(self) -> pa.Table:
        ds = self._table._dataset()
        kw: Dict[str, Any] = {}
        if self._select is not None:
            kw["columns"] = self._select
        if self._where:
            kw["filter"] = self._where
        if self._with_row_id:
            kw["with_row_id"] = True
        if self._with_row_address:
            kw["with_row_address"] = True
        if self._take_offsets is not None or self._take_row_ids is not None:
            return self._run_take(ds)
        if self._fts is not None:
            query = self._fts if not self._fts_columns else {"query": self._fts, "columns": self._fts_columns}
            kw["full_text_query"] = query
            kw["prefilter"] = self._prefilter
            limit = self._limit if self._limit_set else self._default_vector_limit
            if limit is not None:
                kw["limit"] = limit
            if self._offset:
                kw["offset"] = self._offset
            return ds.to_table(**kw)
        if self._vector is None:
            limit = self._limit if self._limit_set else self._default_limit
            if limit is not None:
                kw["limit"] = limit
            if self._offset:
                kw["offset"] = self._offset
            return ds.to_table(**kw)
        queries, column, several = self._queries(ds.schema)
        limit = self._limit if self._limit_set and self._limit is not None else self._default_vector_limit
        offset = self._offset or 0
        tables = []
        for i, q in enumerate(queries):
            nearest: Dict[str, Any] = {"column": column, "q": q, "k": limit + offset,
                                       "metric": self._metric or "l2", "use_index": not self._bypass_index}
            for key, value in (("nprobes", self._nprobes), ("minimum_nprobes", self._minimum_nprobes),
                               ("maximum_nprobes", self._maximum_nprobes), ("refine_factor", self._refine_factor),
                               ("ef", self._ef), ("distance_range", self._distance_range)):
                if value is not None:
                    nearest[key] = value
            t = ds.to_table(nearest=nearest, prefilter=self._prefilter, fast_search=self._fast_search or None,
                            **kw)
            if offset:
                t = t.slice(offset)
            if several:
                t = t.append_column("query_index", pa.array([i] * t.num_rows, pa.int32()))
            tables.append(t)
        return tables[0] if len(tables) == 1 else pa.concat_tables(tables)

    def _run_take(self, ds) -> pa.Table:
        columns = self._select
        if self._take_offsets is not None:
            t = ds.take(self._take_offsets, columns=columns)
        else:
            t = ds.take_rows(self._take_row_ids, columns=columns) if hasattr(ds, "take_rows") else \
                ds._take_rows(self._take_row_ids, columns=columns)
        return t

    def _explain(self) -> str:
        kind = "KNNVectorDistance" if self._vector is not None else "LanceScan"
        return f"{kind}: table={self._table.name}, filter={self._where!r}, limit={self._limit}"


class LanceQueryBuilder(_Builder):
    """The sync builder ``Table.search()`` returns: ``.where().limit().to_list()`` and the rest."""

    def __init__(self, table):
        super().__init__(table)

    def to_arrow(self, *, timeout: Optional[timedelta] = None) -> pa.Table:
        return self._run()

    def to_list(self, *, timeout: Optional[timedelta] = None) -> List[dict]:
        return self._run().to_pylist()

    def to_pandas(self, flatten: Optional[Union[int, bool]] = None, *, timeout=None):
        return self._run().to_pandas()

    def to_df(self):
        return self.to_pandas()

    def to_polars(self, *, timeout=None):
        import polars

        return polars.from_arrow(self._run())

    def to_pydantic(self, model, *, timeout=None) -> list:
        return [model(**{k: v for k, v in row.items() if k in model.model_fields}) for row in self.to_list()]

    def to_batches(self, batch_size: Optional[int] = None, *, timeout=None) -> pa.RecordBatchReader:
        t = self._run()
        return pa.RecordBatchReader.from_batches(t.schema, t.to_batches(max_chunksize=batch_size))

    def explain_plan(self, verbose: bool = False) -> str:
        return self._explain()


LanceVectorQueryBuilder = LanceQueryBuilder
LanceFtsQueryBuilder = LanceQueryBuilder
LanceEmptyQueryBuilder = LanceQueryBuilder


class AsyncRecordBatchReader:
    """The batches of a finished query, as an async iterator (``async for batch in reader``)."""

    def __init__(self, table: pa.Table, max_batch_length: Optional[int] = None):
        self.schema = table.schema
        self._batches = iter(table.to_batches(max_chunksize=max_batch_length))

    def __aiter__(self):
        return self

    async def __anext__(self) -> pa.RecordBatch:
        try:
            return next(self._batches)
        except StopIteration:
            raise StopAsyncIteration from None

    async def read_all(self) -> pa.Table:
        return pa.Table.from_batches(list(self._batches), schema=self.schema)


class AsyncQuery(_Builder):
    """``AsyncTable.query()``: no limit unless one is set; ``nearest_to`` makes it a vector query (limit 10)."""

    _default_limit = None

    def nearest_to(self, query_vector) -> "AsyncQuery":
        return super().nearest_to(query_vector)

    def nearest_to_text(self, query, columns=None) -> "AsyncQuery":
        if self._vector is not None:
            raise NotImplementedError("hybrid (vector + full-text) search is not supported by nanolance.lancedb")
        return self._fts(query, columns)

    async def _arun(self) -> pa.Table:
        import asyncio

        return await asyncio.to_thread(self._run)  # off the event loop; nanolance releases the GIL

    async def to_arrow(self, timeout: Optional[timedelta] = None) -> pa.Table:
        return await self._arun()

    async def to_list(self, timeout: Optional[timedelta] = None) -> List[dict]:
        return (await self._arun()).to_pylist()

    async def to_pandas(self, flatten=None, timeout: Optional[timedelta] = None, **kwargs):
        return (await self._arun()).to_pandas()

    async def to_polars(self, timeout: Optional[timedelta] = None):
        import polars

        return polars.from_arrow((await self._arun()))

    async def to_pydantic(self, model, *, timeout=None) -> list:
        return [model(**{k: v for k, v in row.items() if k in model.model_fields})
                for row in (await self._arun()).to_pylist()]

    async def to_batches(self, *, max_batch_length: Optional[int] = None,
                         timeout: Optional[timedelta] = None) -> AsyncRecordBatchReader:
        return AsyncRecordBatchReader((await self._arun()), max_batch_length)

    async def output_schema(self) -> pa.Schema:
        return (await self._arun()).schema

    async def explain_plan(self, verbose: Optional[bool] = False) -> str:
        return self._explain()


AsyncVectorQuery = AsyncQuery
AsyncFTSQuery = AsyncQuery
AsyncStandardQuery = AsyncQuery
AsyncQueryBase = AsyncQuery
