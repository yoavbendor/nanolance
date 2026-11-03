# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lancedb.table``: ``LanceTable`` (sync) and ``AsyncTable``, over ``nanolance.lance`` datasets.

A table is the Lance dataset ``<db>/<name>.lance``. Unless a version is checked out, every call reads the latest
version (another writer's commits are seen at once); a checked-out table reads that version and refuses writes,
as LanceDB does.
"""

from __future__ import annotations

import asyncio
import datetime
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Union

import numpy as np
import pyarrow as pa

import nanolance.lance as nl

__all__ = ["Table", "LanceTable", "AsyncTable", "AddResult", "DeleteResult", "UpdateResult", "MergeResult",
           "OptimizeStats", "CompactionStats", "RemovalStats"]


# ------------------------------------------------------------------------------------------------ results


@dataclass
class AddResult:
    version: int


@dataclass
class DeleteResult:
    num_deleted_rows: int
    version: int


@dataclass
class UpdateResult:
    rows_updated: int
    version: int


@dataclass
class MergeResult:
    version: int
    num_updated_rows: int
    num_inserted_rows: int
    num_deleted_rows: int
    num_attempts: int = 1
    num_rows: int = 0


MergeInsertResult = MergeResult


@dataclass
class CompactionStats:
    fragments_removed: int = 0
    fragments_added: int = 0
    files_removed: int = 0
    files_added: int = 0


@dataclass
class RemovalStats:
    bytes_removed: int = 0
    old_versions_removed: int = 0


@dataclass
class OptimizeStats:
    compaction: CompactionStats
    prune: RemovalStats


# ------------------------------------------------------------------------------------------------ data in


def _is_model(obj) -> bool:
    return hasattr(obj, "model_dump") or (hasattr(obj, "dict") and hasattr(obj, "__fields__"))


def _model_row(obj) -> dict:
    return obj.model_dump() if hasattr(obj, "model_dump") else obj.dict()


def _plain(value):
    """A value pyarrow converts: numpy arrays and scalars become lists and Python scalars."""
    if isinstance(value, np.ndarray):
        return value.tolist()
    if isinstance(value, np.generic):
        return value.item()
    if isinstance(value, dict):
        return {k: _plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)) and value and isinstance(value[0], (np.ndarray, dict, list, tuple)):
        return [_plain(v) for v in value]
    return value


def _infer_vectors(table: pa.Table) -> pa.Table:
    """As LanceDB infers a schema: a list<float> column whose lists are all one length is a vector,
    fixed_size_list<float32>[n]."""
    columns, fields = [], []
    for name, column in zip(table.column_names, table.columns):
        f = table.schema.field(name)
        t = f.type
        if (pa.types.is_list(t) or pa.types.is_large_list(t)) and (
                pa.types.is_floating(t.value_type) or pa.types.is_integer(t.value_type)):
            lengths = pa.compute.list_value_length(column).drop_null()
            distinct = pa.compute.unique(lengths).to_pylist() if len(lengths) else []
            if len(distinct) == 1 and distinct[0] > 0:
                value = pa.float32() if not pa.types.is_float16(t.value_type) else t.value_type
                if pa.types.is_float64(t.value_type) and name != "vector":
                    value = pa.float32()
                target = pa.list_(pa.field("item", value), distinct[0])
                column = column.cast(target)
                f = pa.field(name, target, f.nullable, f.metadata)
        columns.append(column)
        fields.append(f)
    return pa.Table.from_arrays(columns, schema=pa.schema(fields, metadata=table.schema.metadata))


def _rows_to_table(rows: List[dict], schema: Optional[pa.Schema]) -> pa.Table:
    rows = [_plain(r) for r in rows]
    if schema is None:
        return _infer_vectors(pa.Table.from_pylist(rows))
    missing = [f.name for f in schema if not f.nullable and any(f.name not in r for r in rows)]
    if missing:
        raise RuntimeError(
            f"lance error: Append with different schema: fields did not match, missing={missing}, unexpected=[]")
    return pa.Table.from_pylist(rows, schema=schema)


class _UnknownField(ValueError):
    pass


def _extra_in_type(t: pa.DataType, target: pa.DataType) -> Optional[str]:
    """The first field of ``t`` (at any depth) that ``target`` has no place for."""
    if pa.types.is_struct(t) and pa.types.is_struct(target):
        names = {target.field(i).name: target.field(i).type for i in range(target.num_fields)}
        for i in range(t.num_fields):
            f = t.field(i)
            if f.name not in names:
                return f.name
            found = _extra_in_type(f.type, names[f.name])
            if found:
                return found
    elif (pa.types.is_list(t) or pa.types.is_large_list(t) or pa.types.is_fixed_size_list(t)) and (
            pa.types.is_list(target) or pa.types.is_large_list(target) or pa.types.is_fixed_size_list(target)):
        return _extra_in_type(t.value_type, target.value_type)
    return None


def _extra_in_value(value, target: pa.DataType) -> Optional[str]:
    if isinstance(value, dict) and pa.types.is_struct(target):
        names = {target.field(i).name: target.field(i).type for i in range(target.num_fields)}
        for key, v in value.items():
            if key not in names:
                return key
            found = _extra_in_value(v, names[key])
            if found:
                return found
    elif isinstance(value, (list, tuple)) and (pa.types.is_list(target) or pa.types.is_large_list(target)):
        for v in value:
            found = _extra_in_value(v, target.value_type)
            if found:
                return found
    return None


def _unknown_field(data, schema: pa.Schema) -> Optional[str]:
    """A field of ``data`` -- rows or a table -- that ``schema`` does not have, nested ones included."""
    as_struct = pa.struct(list(schema))
    if isinstance(data, pa.Table):
        return _extra_in_type(pa.struct(list(data.schema)), as_struct)
    for row in data:
        found = _extra_in_value(row, as_struct)
        if found:
            return found
    return None


def _to_arrow(data, schema: Optional[pa.Schema] = None, op: str = "add") -> pa.Table:
    try:
        return _to_arrow_checked(data, schema)
    except _UnknownField as e:
        name = str(e)
        if op == "add":
            raise ValueError(f"Invalid input, field '{name}' does not exist in table schema") from None
        raise ValueError(f"Field '{name}' not found in target schema") from None


def _to_arrow_checked(data, schema: Optional[pa.Schema] = None) -> pa.Table:
    """``data`` -- list of dicts or pydantic models, a dict, a pyarrow Table / RecordBatch / reader, an iterable of
    batches, a pandas or polars DataFrame -- as a pyarrow Table in ``schema`` (inferred when None)."""
    if isinstance(data, pa.RecordBatch):
        data = pa.Table.from_batches([data])
    elif isinstance(data, pa.RecordBatchReader):
        data = data.read_all()
    elif hasattr(data, "to_arrow") and not isinstance(data, pa.Table) and type(data).__module__.startswith("polars"):
        data = data.to_arrow()
    elif type(data).__name__ == "DataFrame" and hasattr(data, "to_dict"):
        data = pa.Table.from_pandas(data, preserve_index=False)
    elif _is_model(data) or isinstance(data, dict):
        data = [data]
    if isinstance(data, pa.Table):
        if schema is not None:
            unknown = _unknown_field(data, schema)
            if unknown:
                raise _UnknownField(unknown)
            return _conform(data, schema)
        return _infer_vectors(data)
    if isinstance(data, (list, tuple)) or (isinstance(data, Iterable) and not isinstance(data, (str, bytes))):
        items = list(data)
        if items and isinstance(items[0], (pa.RecordBatch, pa.Table)):
            batches = [pa.Table.from_batches([b]) if isinstance(b, pa.RecordBatch) else b for b in items]
            return _to_arrow_checked(pa.concat_tables(batches), schema)
        rows = [_model_row(r) if _is_model(r) else r for r in items]
        if schema is not None:
            unknown = _unknown_field(rows, schema)
            if unknown:
                raise _UnknownField(unknown)
        return _rows_to_table(rows, schema)
    raise TypeError(f"Unsupported data type: {type(data)}")


def _conform(table: pa.Table, schema: pa.Schema) -> pa.Table:
    """``table`` with ``schema``'s columns, in its order: absent nullable columns as nulls, the rest cast."""
    columns = []
    for f in schema:
        if f.name in table.column_names:
            column = table.column(f.name)
            if column.type != f.type:
                column = _cast(column, f.type)
            columns.append(column)
        elif f.nullable:
            columns.append(pa.nulls(table.num_rows, f.type))
        else:
            raise RuntimeError(f"lance error: Append with different schema: fields did not match, "
                               f"missing=[{f.name}], unexpected=[]")
    return pa.Table.from_arrays(columns, schema=schema)


def _cast(column, target: pa.DataType):
    try:
        return column.cast(target)
    except (pa.ArrowInvalid, pa.ArrowNotImplementedError):
        # struct children in another order, or a struct missing a nullable child: rebuild through Python
        return pa.array(column.to_pylist(), target)


# ------------------------------------------------------------------------------------------------ SQL values


def _sql_literal(value) -> str:
    """A Python value as a Lance SQL literal (``update(values=...)``)."""
    value = _plain(value)
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    if isinstance(value, bytes):
        return "X'" + value.hex() + "'"
    if isinstance(value, datetime.datetime):
        return f"TIMESTAMP '{value.isoformat(sep=' ')}'"
    if isinstance(value, datetime.date):
        return f"DATE '{value.isoformat()}'"
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(_sql_literal(v) for v in value) + "]"
    raise NotImplementedError(f"SQL conversion is not implemented for this type: {type(value)}")


# ------------------------------------------------------------------------------------------------ the table


_INDEX_NAMES = {"IVF_FLAT": "IvfFlat", "IVF_PQ": "IvfPq", "IVF_SQ": "IvfSq", "IVF_RQ": "IvfRq",
                "IVF_HNSW_FLAT": "IvfHnswFlat", "IVF_HNSW_PQ": "IvfHnswPq", "IVF_HNSW_SQ": "IvfHnswSq",
                "BTREE": "BTree", "BITMAP": "Bitmap", "LABEL_LIST": "LabelList", "LABELLIST": "LabelList",
                "INVERTED": "FTS", "FTS": "FTS", "ZONEMAP": "ZoneMap", "BLOOMFILTER": "BloomFilter"}


class LanceTable:
    """A LanceDB table (the synchronous API). ``AsyncTable`` wraps one."""

    def __init__(self, connection, name: str, uri: str, storage_options: Optional[Dict[str, str]] = None):
        self._conn = connection
        self._name = name
        self._uri = uri
        self._storage_options = storage_options
        self._version: Optional[int] = None  # None: the latest
        self._open = True

    # -- plumbing

    def _dataset(self):
        kw = {"storage_options": self._storage_options} if self._storage_options else {}
        if self._version is not None:
            kw["version"] = self._version
        return nl.dataset(self._uri, **kw)

    def _writable(self):
        if self._version is not None:
            raise ValueError("Invalid input, table cannot be modified when a specific version is checked out")

    def _latest_version(self) -> int:
        return self._dataset().latest_version if self._version is None else self._version

    def _write(self, table: pa.Table, mode: str):
        kw = {"storage_options": self._storage_options} if self._storage_options else {}
        return nl.write_dataset(table, self._uri, mode=mode, **kw)

    def __repr__(self) -> str:
        return f"NativeTable({self._name}, uri={self._uri}, read_consistency_interval=None)"

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self) -> None:
        self._open = False

    def is_open(self) -> bool:
        return self._open

    # -- metadata

    @property
    def name(self) -> str:
        return self._name

    @property
    def uri(self) -> str:
        return self._uri

    @property
    def schema(self) -> pa.Schema:
        return self._dataset().schema

    @property
    def version(self) -> int:
        return self._dataset().version

    def list_versions(self) -> List[dict]:
        return [{"version": int(v["version"]), "timestamp": v["timestamp"], "metadata": dict(v.get("metadata") or {})}
                for v in self._dataset().versions()]

    def count_rows(self, filter: Optional[str] = None) -> int:
        return self._dataset().count_rows(filter)

    def __len__(self) -> int:
        return self.count_rows()

    def head(self, n: int = 5) -> pa.Table:
        return self._dataset().head(n)

    def to_arrow(self) -> pa.Table:
        return self._dataset().to_table()

    def to_pandas(self, **kwargs):
        return self.to_arrow().to_pandas(**kwargs)

    def to_polars(self):
        import polars

        return polars.from_arrow(self.to_arrow())

    def to_lance(self, **kwargs):
        return self._dataset()

    def stats(self) -> dict:
        ds = self._dataset()
        lengths = [f.count_rows() for f in ds.get_fragments()]
        current = [v for v in ds.versions() if int(v["version"]) == ds.version]
        total_bytes = int((current[0].get("metadata") or {}).get("total_files_size", 0)) if current else 0
        small = sum(1 for n in lengths if n < 100_000)
        q = (lambda p: int(np.percentile(lengths, p))) if lengths else (lambda p: 0)
        return {"total_bytes": total_bytes, "num_rows": sum(lengths), "num_indices": len(ds.list_indices()),
                "fragment_stats": {"num_fragments": len(lengths), "num_small_fragments": small,
                                   "lengths": {"min": min(lengths, default=0), "max": max(lengths, default=0),
                                               "mean": int(np.mean(lengths)) if lengths else 0, "p25": q(25),
                                               "p50": q(50), "p75": q(75), "p99": q(99)}}}

    # -- versions

    def checkout(self, version: Union[int, str]) -> None:
        if isinstance(version, str):
            version = self._dataset().tags.get_version(version)
        self._dataset().checkout_version(int(version))  # raises for a version that does not exist
        self._version = int(version)

    def checkout_latest(self) -> None:
        self._version = None

    def restore(self, version: Optional[Union[int, str]] = None) -> None:
        if version is not None:
            self.checkout(version)
        if self._version is None:
            return
        self._dataset().restore()
        self._version = None

    def cleanup_old_versions(self, older_than: Optional[datetime.timedelta] = None, *,
                             delete_unverified: bool = False):
        return self._dataset().cleanup_old_versions(older_than=older_than, delete_unverified=delete_unverified)

    # -- writes

    def add(self, data, mode: str = "append", on_bad_vectors: Optional[str] = None,
            fill_value: Optional[float] = None, **_) -> AddResult:
        self._writable()
        if mode not in ("append", "overwrite"):
            raise ValueError(f"mode must be one of 'append', 'overwrite', got {mode!r}")
        schema = self.schema
        table = _to_arrow(data, schema)
        table = _handle_bad_vectors(table, on_bad_vectors or "error", fill_value or 0.0)
        ds = self._write(table, mode)
        return AddResult(version=ds.version)

    def delete(self, where: str) -> DeleteResult:
        self._writable()
        ds = self._dataset()
        out = ds.delete(where)
        return DeleteResult(num_deleted_rows=int(out.get("num_deleted_rows", 0)), version=self._latest_version())

    def update(self, where: Optional[str] = None, values: Optional[dict] = None, *,
               values_sql: Optional[Dict[str, str]] = None, updates: Optional[dict] = None,
               updates_sql: Optional[Dict[str, str]] = None) -> UpdateResult:
        self._writable()
        values = values if values is not None else updates
        values_sql = values_sql if values_sql is not None else updates_sql
        if values is not None and values_sql is not None:
            raise ValueError("Only one of values or values_sql can be provided")
        if values is None and values_sql is None:
            raise ValueError("Either values or values_sql must be provided")
        assignments = dict(values_sql) if values_sql is not None else {k: _sql_literal(v) for k, v in values.items()}
        out = self._dataset().update(assignments, where)
        return UpdateResult(rows_updated=int(out.get("num_rows_updated", 0)), version=self._latest_version())

    def merge_insert(self, on: Union[str, Iterable[str]]):
        from nanolance.lancedb.merge import LanceMergeInsertBuilder

        return LanceMergeInsertBuilder(self, [on] if isinstance(on, str) else list(on))

    def _do_merge(self, merge, new_data, on_bad_vectors="error", fill_value=0.0, timeout=None) -> MergeResult:
        self._writable()
        ds = self._dataset()
        table = _handle_bad_vectors(_to_arrow(new_data, ds.schema, op="merge"), on_bad_vectors, fill_value)
        builder = ds.merge_insert(merge._on if len(merge._on) > 1 else merge._on[0])
        if merge._when_matched_update_all:
            builder = builder.when_matched_update_all(merge._when_matched_update_all_condition)
        if merge._when_not_matched_insert_all:
            builder = builder.when_not_matched_insert_all()
        if merge._when_not_matched_by_source_delete:
            builder = builder.when_not_matched_by_source_delete(merge._when_not_matched_by_source_condition)
        out = builder.execute(table)
        return MergeResult(version=self._latest_version(), num_updated_rows=int(out.get("num_updated_rows", 0)),
                           num_inserted_rows=int(out.get("num_inserted_rows", 0)),
                           num_deleted_rows=int(out.get("num_deleted_rows", 0)),
                           num_attempts=int(out.get("num_attempts", 1) or 1), num_rows=table.num_rows)

    def add_columns(self, transforms) -> None:
        self._writable()
        if isinstance(transforms, (pa.Field, pa.Schema)) or (
                isinstance(transforms, list) and transforms and isinstance(transforms[0], pa.Field)):
            fields = list(transforms) if not isinstance(transforms, pa.Field) else [transforms]
            self._dataset().add_columns(pa.schema(fields))
        else:
            self._dataset().add_columns(transforms)

    def alter_columns(self, *alterations) -> None:
        self._writable()
        self._dataset().alter_columns(*alterations)

    def drop_columns(self, columns: Iterable[str]) -> None:
        self._writable()
        self._dataset().drop_columns(list(columns))

    # -- indexes

    def create_index(self, metric: str = "l2", num_partitions: Optional[int] = None,
                     num_sub_vectors: Optional[int] = None, vector_column_name: Optional[str] = None,
                     replace: bool = True, accelerator=None, index_cache_size=None, *, index_type: str = "IVF_PQ",
                     num_bits: int = 8, max_iterations: int = 50, sample_rate: int = 256, m: int = 20,
                     ef_construction: int = 300, name: Optional[str] = None, **kwargs) -> None:
        """The sync API's create_index (``metric`` / ``num_partitions`` / ``index_type`` keywords)."""
        column = vector_column_name or _vector_column(self.schema)
        kw = {"metric": metric}
        if max_iterations != 50:
            kw["max_iterations"] = max_iterations
        if sample_rate != 256:
            kw["sample_rate"] = sample_rate
        if num_partitions is not None:
            kw["num_partitions"] = num_partitions
        if num_sub_vectors is not None:
            kw["num_sub_vectors"] = num_sub_vectors
        kind = index_type.upper()
        if "PQ" in kind or "RQ" in kind:
            kw["num_bits"] = num_bits
        if "HNSW" in kind:
            kw.update(m=m, ef_construction=ef_construction)
        self._create_index(column, kind, name or f"{column}_idx", replace, kw)

    def _create_index(self, column: str, kind: str, name: str, replace: bool, kw: dict) -> None:
        self._writable()
        ds = self._dataset()
        existing = {i["name"] for i in ds.list_indices()}
        if name in existing and not replace:
            raise RuntimeError(f"Index name '{name}' already exists")
        if kind.startswith("IVF"):
            ds.create_index(column, kind, name=name, replace=True, **kw)
        else:
            ds.create_scalar_index(column, kind, name=name, replace=True, **kw)

    def create_index_from_config(self, column: str, config=None, replace: Optional[bool] = None,
                                 name: Optional[str] = None) -> None:
        from nanolance.lancedb import index as ix

        schema = self.schema
        if config is None:
            t = schema.field(column).type
            if pa.types.is_fixed_size_list(t):
                config = ix.IvfPq()
            elif pa.types.is_list(t) or pa.types.is_large_list(t):
                config = ix.LabelList()
            else:
                config = ix.BTree()
        kw = config._pylance() if hasattr(config, "_pylance") else {}
        self._create_index(column, config.index_type, name or f"{column}_idx", True if replace is None else replace,
                           kw)

    def create_scalar_index(self, column: str, *, replace: bool = True, index_type: str = "BTREE",
                            name: Optional[str] = None) -> None:
        self._create_index(column, index_type.upper(), name or f"{column}_idx", replace, {})

    def create_fts_index(self, field_names: Union[str, List[str]], *, replace: bool = False, with_position=False,
                         name: Optional[str] = None, **kwargs) -> None:
        from nanolance.lancedb.index import FTS

        names = [field_names] if isinstance(field_names, str) else list(field_names)
        for column in names:
            config = FTS(with_position=with_position, **{k: v for k, v in kwargs.items()
                                                         if k in FTS.__dataclass_fields__})
            self._create_index(column, "INVERTED", name or f"{column}_idx", replace, config._pylance())

    def list_indices(self) -> list:
        from nanolance.lancedb.index import IndexConfig

        out = []
        for i in self._dataset().list_indices():
            kind = str(i["type"]).upper().replace(" ", "_")
            out.append(IndexConfig(name=i["name"], index_type=_INDEX_NAMES.get(kind, i["type"]),
                                   columns=list(i["fields"])))
        return out

    def index_stats(self, index_name: str):
        import json

        from nanolance.lancedb.index import IndexStatistics

        names = {i["name"] for i in self._dataset().list_indices()}
        if index_name not in names:
            return None
        raw = self._dataset().stats.index_stats(index_name) if hasattr(self._dataset(), "stats") else None
        if isinstance(raw, str):
            raw = json.loads(raw)
        raw = raw or {}
        kind = str(raw.get("index_type", "")).upper()
        return IndexStatistics(num_indexed_rows=int(raw.get("num_indexed_rows", 0)),
                               num_unindexed_rows=int(raw.get("num_unindexed_rows", 0)),
                               index_type=_INDEX_NAMES.get(kind, raw.get("index_type", "")),
                               distance_type=(raw.get("indices") or [{}])[0].get("metric_type"),
                               num_indices=raw.get("num_indices"))

    def drop_index(self, name: str) -> None:
        self._writable()
        self._dataset().drop_index(name)

    def wait_for_index(self, index_names: Iterable[str], timeout=None) -> None:
        names = {i["name"] for i in self._dataset().list_indices()}
        missing = [n for n in index_names if n not in names]
        if missing:
            raise RuntimeError(f"Timeout waiting for indices: {missing}")

    def optimize(self, *, cleanup_older_than: Optional[datetime.timedelta] = None, delete_unverified: bool = False,
                 retrain: bool = False) -> OptimizeStats:
        """Compact the fragments, then delete versions older than ``cleanup_older_than`` (7 days when None), then
        bring the indexes up to date -- LanceDB's ``optimize``."""
        self._writable()
        ds = self._dataset()
        metrics = ds.optimize.compact_files()
        compaction = CompactionStats(int(getattr(metrics, "fragments_removed", 0)),
                                     int(getattr(metrics, "fragments_added", 0)),
                                     int(getattr(metrics, "files_removed", 0)),
                                     int(getattr(metrics, "files_added", 0)))
        ds = self._dataset()
        removed = ds.cleanup_old_versions(older_than=cleanup_older_than or datetime.timedelta(days=7),
                                          delete_unverified=delete_unverified)
        if ds.list_indices():
            self._dataset().optimize.optimize_indices(retrain=retrain)
        return OptimizeStats(compaction=compaction,
                             prune=RemovalStats(bytes_removed=int(getattr(removed, "bytes_removed", 0)),
                                                old_versions_removed=int(getattr(removed, "old_versions", 0))))

    def compact_files(self, *args, **kwargs):
        self._writable()
        return self._dataset().optimize.compact_files(*args, **kwargs)

    # -- queries

    def search(self, query=None, vector_column_name: Optional[str] = None, query_type: str = "auto",
               ordering_field_name: Optional[str] = None, fts_columns=None):
        from nanolance.lancedb.query import LanceQueryBuilder

        builder = LanceQueryBuilder(self)
        if query is None:
            return builder
        if isinstance(query, str) or query_type == "fts":
            return builder._fts(query, fts_columns)
        return builder.nearest_to(query).column(vector_column_name) if vector_column_name else \
            builder.nearest_to(query)

    def query(self):
        from nanolance.lancedb.query import LanceQueryBuilder

        return LanceQueryBuilder(self)

    def vector_search(self, query_vector):
        return self.query().nearest_to(query_vector)

    def take_offsets(self, offsets: List[int]):
        from nanolance.lancedb.query import LanceQueryBuilder

        return LanceQueryBuilder(self)._take(offsets=list(offsets))

    def take_row_ids(self, row_ids: List[int]):
        from nanolance.lancedb.query import LanceQueryBuilder

        return LanceQueryBuilder(self)._take(row_ids=list(row_ids))


Table = LanceTable


def _vector_columns(schema: pa.Schema) -> List[str]:
    def is_vector(t):
        if pa.types.is_fixed_size_list(t):
            return pa.types.is_floating(t.value_type) or pa.types.is_integer(t.value_type)
        if pa.types.is_list(t) or pa.types.is_large_list(t):
            return pa.types.is_fixed_size_list(t.value_type)  # a multivector
        return False

    return [f.name for f in schema if is_vector(f.type)]


def _vector_column(schema: pa.Schema, dim: Optional[int] = None) -> str:
    names = _vector_columns(schema)
    if dim is not None:
        def width(name):
            t = schema.field(name).type
            return t.list_size if pa.types.is_fixed_size_list(t) else t.value_type.list_size

        sized = [n for n in names if width(n) == dim]
        names = sized or names
    if len(names) == 1:
        return names[0]
    if "vector" in names:
        return "vector"
    if not names:
        raise ValueError("There is no vector column in the data. Please specify the vector column name for "
                         "vector search")
    raise ValueError(f"Schema has more than one vector column: {names}. Please specify the vector column name "
                     "for vector search")


def _handle_bad_vectors(table: pa.Table, on_bad_vectors: str, fill_value: float) -> pa.Table:
    """``on_bad_vectors``: 'error' refuses a vector with a NaN, 'drop' drops its row, 'fill' fills it, 'null' nulls
    it."""
    if on_bad_vectors not in ("error", "drop", "fill", "null"):
        raise ValueError(f"on_bad_vectors must be one of 'error', 'drop', 'fill', 'null'; got {on_bad_vectors!r}")
    for name in _vector_columns(table.schema):
        column = table.column(name)
        if not pa.types.is_fixed_size_list(column.type) or not pa.types.is_floating(column.type.value_type):
            continue
        flat = pa.compute.list_flatten(column)
        if not pa.compute.any(pa.compute.is_nan(flat)).as_py():
            continue
        size = column.type.list_size
        nan_rows = np.zeros(len(column), dtype=bool)
        valid = np.asarray(column.is_valid())
        values = flat.to_numpy(zero_copy_only=False).reshape(-1, size)
        nan_rows[np.flatnonzero(valid)] = np.isnan(values).any(axis=1)
        if on_bad_vectors == "error":
            raise ValueError(f"Vector column '{name}' has NaNs")
        if on_bad_vectors == "drop":
            table = table.filter(pa.array(~nan_rows))
        else:
            rows = column.to_pylist()
            for i in np.flatnonzero(nan_rows):
                rows[i] = None if on_bad_vectors == "null" else [fill_value] * size
            table = table.set_column(table.schema.get_field_index(name), table.schema.field(name),
                                     pa.array(rows, column.type))
    return table


# ------------------------------------------------------------------------------------------------ async


class AsyncTable:
    """``lancedb.AsyncTable``: the same table, its methods coroutines."""

    def __init__(self, table: LanceTable):
        self._table = table

    def __repr__(self) -> str:
        return repr(self._table)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    @property
    def name(self) -> str:
        return self._table.name

    def is_open(self) -> bool:
        return self._table.is_open()

    def close(self) -> None:
        self._table.close()

    async def uri(self) -> str:
        return await _io(getattr, self._table, "uri")

    async def schema(self) -> pa.Schema:
        return await _io(getattr, self._table, "schema")

    async def version(self) -> int:
        return await _io(getattr, self._table, "version")

    async def list_versions(self) -> List[dict]:
        return await _io(self._table.list_versions)

    async def count_rows(self, filter: Optional[str] = None) -> int:
        return await _io(self._table.count_rows, filter)

    async def head(self, n: int = 5) -> pa.Table:
        return await _io(self._table.head, n)

    async def to_arrow(self) -> pa.Table:
        return await _io(self._table.to_arrow)

    async def to_pandas(self, **kwargs):
        return await _io(self._table.to_pandas, **kwargs)

    async def to_lance(self, **kwargs):
        return await _io(self._table.to_lance, **kwargs)

    async def stats(self) -> dict:
        return await _io(self._table.stats)

    async def checkout(self, version: Union[int, str]) -> None:
        await _io(self._table.checkout, version)

    async def checkout_latest(self) -> None:
        await _io(self._table.checkout_latest)

    async def restore(self, version: Optional[Union[int, str]] = None) -> None:
        await _io(self._table.restore, version)

    async def add(self, data, *, mode: str = "append", on_bad_vectors: Optional[str] = None,
                  fill_value: Optional[float] = None, **kwargs) -> AddResult:
        return await _io(self._table.add, data, mode=mode, on_bad_vectors=on_bad_vectors, fill_value=fill_value)

    async def delete(self, where: str) -> DeleteResult:
        return await _io(self._table.delete, where)

    async def update(self, updates: Optional[dict] = None, *, where: Optional[str] = None,
                     updates_sql: Optional[Dict[str, str]] = None) -> UpdateResult:
        return await _io(self._table.update, where=where, values=updates, values_sql=updates_sql)

    def merge_insert(self, on: Union[str, Iterable[str]]):
        from nanolance.lancedb.merge import AsyncMergeInsertBuilder

        return AsyncMergeInsertBuilder(self._table, [on] if isinstance(on, str) else list(on))

    async def add_columns(self, transforms) -> None:
        await _io(self._table.add_columns, transforms)

    async def alter_columns(self, *alterations) -> None:
        await _io(self._table.alter_columns, *alterations)

    async def drop_columns(self, columns: Iterable[str]) -> None:
        await _io(self._table.drop_columns, columns)

    async def create_index(self, column: str, *, replace: Optional[bool] = None, config=None,
                           wait_timeout=None, name: Optional[str] = None, train: bool = True) -> None:
        await _io(self._table.create_index_from_config, column, config, replace, name)

    async def drop_index(self, name: str) -> None:
        await _io(self._table.drop_index, name)

    async def list_indices(self) -> list:
        return await _io(self._table.list_indices)

    async def index_stats(self, index_name: str):
        return await _io(self._table.index_stats, index_name)

    async def wait_for_index(self, index_names: Iterable[str], timeout=None) -> None:
        await _io(self._table.wait_for_index, index_names, timeout)

    async def optimize(self, *, cleanup_older_than: Optional[datetime.timedelta] = None,
                       delete_unverified: bool = False, retrain=False) -> OptimizeStats:
        return await _io(self._table.optimize, cleanup_older_than=cleanup_older_than,
                         delete_unverified=delete_unverified, retrain=retrain)

    def query(self):
        from nanolance.lancedb.query import AsyncQuery

        return AsyncQuery(self._table)

    def vector_search(self, query_vector):
        return self.query().nearest_to(query_vector)

    async def search(self, query=None, vector_column_name: Optional[str] = None, query_type: str = "auto",
                     ordering_field_name: Optional[str] = None, fts_columns=None):
        q = self.query()
        if query is None:
            return q
        if isinstance(query, str) or query_type == "fts":
            return q.nearest_to_text(query, fts_columns)
        q = q.nearest_to(query)
        return q.column(vector_column_name) if vector_column_name else q

    def take_offsets(self, offsets: List[int]):
        from nanolance.lancedb.query import AsyncQuery

        return AsyncQuery(self._table)._take(offsets=list(offsets))

    def take_row_ids(self, row_ids: List[int]):
        from nanolance.lancedb.query import AsyncQuery

        return AsyncQuery(self._table)._take(row_ids=list(row_ids))


async def _io(fn, *args, **kwargs):
    """Run a native call on a worker thread: nanolance releases the GIL while it reads and writes, so the event
    loop keeps running and concurrent queries overlap, as on LanceDB's Rust runtime."""
    return await asyncio.to_thread(fn, *args, **kwargs)
