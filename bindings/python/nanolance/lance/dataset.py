# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.dataset`` as nanolance implements it: LanceDataset, its scanner, and write_dataset.

The names, signatures, defaults and exception types follow pylance's (Apache-2.0, The Lance Authors),
so code written against ``lance`` runs unchanged on the part of the API listed in
docs/PYLANCE_COMPAT.md. What is not implemented raises ``NotImplementedError`` naming the feature;
nothing is silently ignored when ignoring it would change a result.
"""

from __future__ import annotations

import os
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, Iterable, Iterator, List, Optional, Sequence, TypedDict, Union

import pyarrow as pa

from nanolance import _nanolance
from nanolance.lance._errors import native, unsupported

LANCE_COMMIT_MESSAGE_KEY = "__lance_commit_message"

ReaderLike = Any


class AutoCleanupConfig(TypedDict):
    interval: int
    older_than_seconds: int


class Version(TypedDict):
    version: int
    timestamp: datetime
    metadata: Dict[str, str]


_MEMORY_ROOT: Optional[str] = None


def _memory_root() -> str:
    """Where ``memory://`` datasets live: a directory private to this process, removed at exit --
    what pylance's in-memory object store gives, on the local file system."""
    global _MEMORY_ROOT
    if _MEMORY_ROOT is None:
        import atexit
        import shutil
        import tempfile

        _MEMORY_ROOT = tempfile.mkdtemp(prefix="nanolance-memory-")
        atexit.register(shutil.rmtree, _MEMORY_ROOT, True)
    return _MEMORY_ROOT


def _path_of(uri: Union[str, Path, "LanceDataset"]) -> str:
    if isinstance(uri, LanceDataset):
        return uri.uri
    text = os.fspath(uri)
    if text.startswith("memory://"):
        return os.path.join(_memory_root(), text[len("memory://"):].strip("/") or "_root")
    if text.startswith("file://"):
        text = text[len("file://"):]
    elif "://" in text:
        raise unsupported(f"the storage scheme of {text!r} (local paths only)")
    return os.path.abspath(os.path.expanduser(text))


def _exists(path: str) -> bool:
    return os.path.isdir(os.path.join(path, "_versions")) and any(
        name.endswith(".manifest") for name in os.listdir(os.path.join(path, "_versions"))
    )


def _normalize_columns(columns) -> Optional[List[str]]:
    if columns is None:
        return None
    if isinstance(columns, dict):
        # {"alias": "column"}: a rename of whole columns is all a projection can be here.
        for alias, expr in columns.items():
            if not isinstance(expr, str) or expr not in (alias,) and not expr.isidentifier():
                raise unsupported("SQL expressions in a column projection")
        return list(columns.values())
    if isinstance(columns, str):
        raise TypeError("columns must be a list of column names")
    return [str(c) for c in columns]


def _rename(table: pa.Table, columns) -> pa.Table:
    if isinstance(columns, dict):
        names = list(columns.keys()) + [n for n in table.column_names if n in ("_rowid", "_rowaddr")]
        return table.rename_columns(names)
    return table


class LanceDataset:
    """A Lance dataset at a version. Mirrors ``lance.LanceDataset``."""

    def __init__(
        self,
        uri: Union[str, Path],
        version: Optional[Union[int, str]] = None,
        block_size: Optional[int] = None,
        index_cache_size: Optional[int] = None,
        metadata_cache_size: Optional[int] = None,
        commit_lock=None,
        storage_options: Optional[Dict[str, str]] = None,
        serialized_manifest: Optional[bytes] = None,
        default_scan_options: Optional[Dict[str, Any]] = None,
        metadata_cache_size_bytes: Optional[int] = None,
        index_cache_size_bytes: Optional[int] = None,
        read_params: Optional[Dict[str, Any]] = None,
        session=None,
        **kwargs,
    ):
        if isinstance(version, str):
            raise unsupported("tags (version given as a string)")
        self._uri = _path_of(uri)
        self._default_scan_options = dict(default_scan_options or {})
        with native():
            info = _nanolance._ds_info(self._uri, version)
        self._pinned = version is not None
        self._set_info(info)

    def _set_info(self, info: dict) -> None:
        self._info = info
        self._version = int(info["version"])
        self._schema_cache = None

    def _refresh_latest(self) -> None:
        with native():
            self._set_info(_nanolance._ds_info(self._uri, None))

    @classmethod
    def from_pydantic_model(cls, model, data, uri, **kwargs) -> "LanceDataset":
        """Write a list of pydantic ``model`` instances as a new dataset at ``uri``."""
        try:
            from pydantic import BaseModel
        except ImportError as exc:  # pragma: no cover
            raise ImportError("from_pydantic_model needs pydantic") from exc
        if not (isinstance(model, type) and issubclass(model, BaseModel)):
            raise TypeError(f"model must be a pydantic BaseModel subclass, got {model!r}")
        if not isinstance(data, list):
            raise TypeError("data must be provided as a list of model instances")
        from nanolance.lance.pydantic import _pydantic_reader

        return write_dataset(_pydantic_reader(data, None, model), uri, **kwargs)

    # ── identity ──────────────────────────────────────────────────────────────────────────────────

    @property
    def uri(self) -> str:
        return self._uri

    @property
    def version(self) -> int:
        return self._version

    @property
    def latest_version(self) -> int:
        with native():
            return int(_nanolance._ds_latest_version(self._uri))

    @property
    def data_storage_version(self) -> str:
        return self._info["data_storage_version"]

    @property
    def has_stable_row_ids(self) -> bool:
        return bool(self._info["reader_feature_flags"] & 2)

    @property
    def schema(self) -> pa.Schema:
        if self._schema_cache is None:
            with native():
                schema = pa.schema(_nanolance._ds_schema(self._uri, self._version))
            meta = self._info["schema_metadata"]
            if meta:
                schema = schema.with_metadata(meta)
            self._schema_cache = schema
        return self._schema_cache

    @property
    def max_field_id(self) -> int:
        return max(len(self.schema.names) - 1, 0)

    @property
    def partition_expression(self):
        return None

    def __len__(self) -> int:
        return self.count_rows()

    def __repr__(self) -> str:
        return f"LanceDataset(uri={self._uri!r}, version={self._version})"

    def __getstate__(self):
        return {"uri": self._uri, "version": self._version, "pinned": self._pinned}

    def __setstate__(self, state):
        self.__init__(state["uri"], version=state["version"])
        self._pinned = state.get("pinned", True)

    def __copy__(self):
        return LanceDataset(self._uri, version=self._version)

    # ── versions ──────────────────────────────────────────────────────────────────────────────────

    def versions(self) -> List[Version]:
        with native():
            raw = _nanolance._ds_versions(self._uri)
        out = []
        for v in raw:
            with native():
                info = _nanolance._ds_info(self._uri, int(v["version"]))
            out.append(
                {
                    "version": int(v["version"]),
                    "timestamp": datetime.fromtimestamp(v["timestamp_ns"] / 1e9),
                    "metadata": _version_summary(info),
                }
            )
        return out

    def checkout_version(self, version) -> "LanceDataset":
        if isinstance(version, tuple):
            branch, number = version
            if branch not in (None, "main"):
                raise unsupported("branches")
            version = number
        if version is None:
            return LanceDataset(self._uri)
        return LanceDataset(self._uri, version=version)

    def checkout_latest(self) -> "LanceDataset":
        self._refresh_latest()
        self._pinned = False
        return self

    def restore(self) -> None:
        with native():
            _nanolance._ds_restore(self._uri, self._version)
        self._refresh_latest()

    # ── config and metadata ───────────────────────────────────────────────────────────────────────

    def config(self) -> Dict[str, str]:
        return dict(self._info["config"])

    def update_config(self, values: Dict[str, Optional[str]], *, replace: bool = False) -> Dict[str, str]:
        current = self.config()
        remove = [k for k, v in values.items() if v is None]
        if replace:
            remove = sorted(set(remove) | (set(current) - set(values)))
        upsert = {k: str(v) for k, v in values.items() if v is not None}
        with native():
            _nanolance._ds_update_config(self._uri, upsert, remove)
        self._refresh_latest()
        return self.config()

    def delete_config_keys(self, keys: List[str]) -> None:
        with native():
            _nanolance._ds_update_config(self._uri, {}, list(keys))
        self._refresh_latest()

    @property
    def metadata(self) -> Dict[str, str]:
        return dict(self._info["table_metadata"])

    def update_metadata(self, values: Dict[str, Optional[str]], *, replace: bool = False) -> Dict[str, str]:
        merged = {} if replace else self.metadata
        for k, v in values.items():
            if v is None:
                merged.pop(k, None)
            else:
                merged[k] = str(v)
        with native():
            _nanolance._ds_update_table_metadata(self._uri, merged, True)
        self._refresh_latest()
        return self.metadata

    @property
    def schema_metadata(self) -> Dict[str, str]:
        return {k.decode(): v.decode() for k, v in self._info["schema_metadata"].items()}

    def update_schema_metadata(self, values: Dict[str, Optional[str]], *, replace: bool = False) -> Dict[str, str]:
        merged = {} if replace else self.schema_metadata
        for k, v in values.items():
            if v is None:
                merged.pop(k, None)
            else:
                merged[k] = str(v)
        with native():
            _nanolance._ds_update_schema_metadata(self._uri, merged, True)
        self._refresh_latest()
        return self.schema_metadata

    def replace_schema_metadata(self, new_metadata: Dict[str, str]) -> None:
        self.update_schema_metadata(new_metadata, replace=True)

    # ── fragments ─────────────────────────────────────────────────────────────────────────────────

    def get_fragments(self, filter=None) -> List["LanceFragment"]:
        if filter is not None:
            raise unsupported("get_fragments(filter=...)")
        from nanolance.lance.fragment import LanceFragment

        return [LanceFragment(self, f["id"], _info=f) for f in self._info["fragments"]]

    def get_fragment(self, fragment_id: int) -> Optional["LanceFragment"]:
        from nanolance.lance.fragment import LanceFragment

        for f in self._info["fragments"]:
            if f["id"] == fragment_id:
                return LanceFragment(self, fragment_id, _info=f)
        return None

    # ── reads ─────────────────────────────────────────────────────────────────────────────────────

    def scanner(
        self,
        columns=None,
        filter=None,
        limit: Optional[int] = None,
        offset: Optional[int] = None,
        nearest: Optional[dict] = None,
        batch_size: Optional[int] = None,
        batch_size_bytes: Optional[int] = None,
        batch_readahead: Optional[int] = None,
        fragment_readahead: Optional[int] = None,
        scan_in_order: Optional[bool] = None,
        fragments=None,
        full_text_query=None,
        *,
        prefilter: Optional[bool] = None,
        with_row_id: Optional[bool] = None,
        with_row_address: Optional[bool] = None,
        use_stats: Optional[bool] = None,
        fast_search: Optional[bool] = None,
        io_buffer_size: Optional[int] = None,
        late_materialization=None,
        use_scalar_index: Optional[bool] = None,
        include_deleted_rows: Optional[bool] = None,
        scan_stats_callback=None,
        strict_batch_size: Optional[bool] = None,
        order_by=None,
        disable_scoring_autoprojection: Optional[bool] = None,
        substrait_filter=None,
        blob_handling=None,
        **kwargs,
    ) -> "LanceScanner":
        options = dict(self._default_scan_options)
        given = dict(
            columns=columns, filter=filter, limit=limit, offset=offset, nearest=nearest, batch_size=batch_size,
            fragments=fragments, full_text_query=full_text_query, with_row_id=with_row_id,
            with_row_address=with_row_address, include_deleted_rows=include_deleted_rows, order_by=order_by,
            substrait_filter=substrait_filter, scan_stats_callback=scan_stats_callback,
        )
        options.update({k: v for k, v in given.items() if v is not None})
        return LanceScanner(self, **options)

    def to_table(self, columns=None, filter=None, limit=None, offset=None, nearest=None, batch_size=None,
                 batch_size_bytes=None, batch_readahead=None, fragment_readahead=None, scan_in_order=None,
                 *, prefilter=None, with_row_id=None, with_row_address=None, use_stats=None, fast_search=None,
                 full_text_query=None, io_buffer_size=None, late_materialization=None, use_scalar_index=None,
                 include_deleted_rows=None, order_by=None, **kwargs) -> pa.Table:
        return self.scanner(
            columns=columns, filter=filter, limit=limit, offset=offset, nearest=nearest, batch_size=batch_size,
            full_text_query=full_text_query, with_row_id=with_row_id, with_row_address=with_row_address,
            include_deleted_rows=include_deleted_rows, order_by=order_by, **kwargs,
        ).to_table()

    def to_batches(self, columns=None, filter=None, limit=None, offset=None, nearest=None, batch_size=None,
                   batch_size_bytes=None, batch_readahead=None, fragment_readahead=None, scan_in_order=None,
                   *, prefilter=None, with_row_id=None, with_row_address=None, use_stats=None, full_text_query=None,
                   io_buffer_size=None, late_materialization=None, use_scalar_index=None,
                   strict_batch_size=None, order_by=None, **kwargs) -> Iterator[pa.RecordBatch]:
        return self.scanner(
            columns=columns, filter=filter, limit=limit, offset=offset, nearest=nearest, batch_size=batch_size,
            full_text_query=full_text_query, with_row_id=with_row_id, with_row_address=with_row_address,
            order_by=order_by, **kwargs,
        ).to_batches()

    def to_pandas(self, columns=None, filter=None, limit=None, offset=None, **kwargs):
        return self.to_table(columns=columns, filter=filter, limit=limit, offset=offset, **kwargs).to_pandas()

    def head(self, num_rows: int, **kwargs) -> pa.Table:
        return self.to_table(limit=num_rows, **kwargs)

    def slice(self, start: int, end: int, columns=None) -> pa.Table:
        n = self.count_rows()
        start, end, _ = slice(start, end).indices(n)
        return self.to_table(columns=columns, offset=start, limit=max(end - start, 0))

    def count_rows(self, filter=None, **kwargs) -> int:
        if filter is not None:
            return self.scanner(filter=filter, columns=[], with_row_id=True).count_rows()
        return sum(int(f["physical_rows"]) - int(f["deleted_rows"]) for f in self._info["fragments"])

    def take(self, indices, columns=None) -> pa.Table:
        wanted = _index_list(indices)
        names = _normalize_columns(columns)
        n = self.count_rows()
        for i in wanted:
            if i < 0 or i >= n:
                raise IndexError(f"index {i} is out of bounds for a dataset of {n} rows")
        return _rename(self._take(wanted, names, addresses=False), columns)

    def _take_rows(self, row_ids, columns=None, **kwargs) -> pa.Table:
        if self.has_stable_row_ids:
            raise unsupported("taking rows of a dataset with stable row ids")
        return _rename(self._take(_index_list(row_ids), _normalize_columns(columns), addresses=True), columns)

    def take_rows(self, row_ids, columns=None, **kwargs) -> pa.Table:
        return self._take_rows(row_ids, columns, **kwargs)

    def _take(self, wanted: List[int], names, addresses: bool, with_row_id=False, with_row_address=False):
        distinct = sorted(set(wanted))
        with native():
            table = pa.table(_nanolance._ds_take(self._uri, self._version, distinct, names, with_row_id,
                                                 with_row_address, addresses))
        if names is not None:
            table = table.select(names + [c for c in ("_rowid", "_rowaddr") if c in table.column_names])
        if wanted == distinct:
            return table
        position = {row: k for k, row in enumerate(distinct)}
        return table.take(pa.array([position[i] for i in wanted], pa.int64()))

    def sample(self, num_rows: int, columns=None, randomize_order: bool = True, **kwargs) -> pa.Table:
        import random

        n = self.count_rows()
        rows = sorted(random.sample(range(n), min(num_rows, n)))
        if randomize_order:
            random.shuffle(rows)
        return self.take(rows, columns=columns)

    # ── writes ────────────────────────────────────────────────────────────────────────────────────

    def insert(self, data: ReaderLike, *, mode="append", **kwargs) -> None:
        new = write_dataset(data, self._uri, mode=mode, **kwargs)
        self._set_info(new._info)

    # ── not supported ─────────────────────────────────────────────────────────────────────────────

    def __getattr__(self, name: str):
        known = {
            "create_index", "create_scalar_index", "drop_index", "list_indices", "describe_indices",
            "index_statistics", "optimize", "cleanup_old_versions", "merge_insert", "merge", "add_columns",
            "alter_columns", "drop_columns", "update", "delete", "tags", "branches", "create_branch", "sql",
            "shallow_clone", "deep_clone", "commit", "commit_batch", "session", "stats", "join", "delta",
            "take_blobs", "read_blobs", "has_index", "prewarm_index", "lance_schema", "validate",
        }
        if name in known:
            raise unsupported(f"LanceDataset.{name}")
        raise AttributeError(name)


def _version_summary(info: dict) -> Dict[str, str]:
    frags = info["fragments"]
    files = sum(len(f["files"]) for f in frags)
    deletions = sum(1 for f in frags if f["deletion_file"])
    return {
        "total_data_file_rows": str(sum(int(f["physical_rows"]) * len(f["files"]) for f in frags)),
        "total_data_files": str(files),
        "total_deletion_file_rows": str(sum(int(f["deleted_rows"]) for f in frags)),
        "total_deletion_files": str(deletions),
        "total_files_size": str(sum(int(fi["size_bytes"]) for f in frags for fi in f["files"])),
        "total_fragments": str(len(frags)),
        "total_rows": str(sum(int(f["physical_rows"]) - int(f["deleted_rows"]) for f in frags)),
    }


def _index_list(indices) -> List[int]:
    if isinstance(indices, (pa.Array, pa.ChunkedArray)):
        indices = indices.to_pylist()
    else:
        try:
            import numpy as np

            if isinstance(indices, np.ndarray):
                indices = indices.tolist()
        except ImportError:  # pragma: no cover
            pass
    return [int(i) for i in indices]


_SYSTEM_COLUMNS = ("_rowid", "_rowaddr", "_rowoffset")


class LanceScanner:
    """A configured read. Mirrors ``lance.LanceScanner``."""

    def __init__(self, ds: LanceDataset, columns=None, filter=None, limit=None, offset=None, nearest=None,
                 batch_size=None, fragments=None, full_text_query=None, with_row_id=False, with_row_address=False,
                 include_deleted_rows=None, order_by=None, substrait_filter=None, scan_stats_callback=None,
                 **ignored):
        if nearest is not None:
            raise unsupported("vector search (nearest=...)")
        if full_text_query is not None:
            raise unsupported("full text search")
        if substrait_filter is not None:
            raise unsupported("substrait filters")
        if include_deleted_rows:
            raise unsupported("include_deleted_rows")
        if order_by:
            raise unsupported("order_by")
        if filter is not None:
            raise unsupported("filters")
        if limit is not None and int(limit) < 0:
            raise ValueError("limit must be non-negative")
        if offset is not None and int(offset) < 0:
            raise ValueError("offset must be non-negative")
        self._ds = ds
        self._columns = columns
        self._names = _normalize_columns(columns)
        # Lance's system columns, named in the projection, come back where they were named.
        self._order = None
        self._row_offset = False
        if self._names is not None and any(n in _SYSTEM_COLUMNS for n in self._names):
            self._order = list(self._names)
            with_row_id = with_row_id or "_rowid" in self._names
            with_row_address = with_row_address or "_rowaddr" in self._names
            self._row_offset = "_rowoffset" in self._names
            self._names = [n for n in self._names if n not in _SYSTEM_COLUMNS]
            if not self._names and not (with_row_id or with_row_address):
                with_row_address = True  # something to count rows by; not returned
        self._limit = None if limit is None else int(limit)
        self._offset = 0 if offset is None else int(offset)
        self._batch_size = batch_size
        self._fragment_ids = None if fragments is None else [int(getattr(f, "fragment_id", f)) for f in fragments]
        self._with_row_id = bool(with_row_id)
        self._with_row_address = bool(with_row_address)

    def _read(self, stream: bool):
        n = self._ds.count_rows() if self._fragment_ids is None else sum(
            self._ds.get_fragment(i).count_rows() for i in self._fragment_ids
        )
        offset = min(self._offset, n)
        length = -1 if self._limit is None else self._limit
        if self._names is not None and not self._names and not (self._with_row_id or self._with_row_address):
            raise ValueError("a scan must select at least one column (or with_row_id)")
        with native():
            return _nanolance._ds_scan(self._ds.uri, self._ds.version, self._names, self._fragment_ids, offset,
                                       length, self._with_row_id, self._with_row_address, stream)

    def _shape(self, table: pa.Table) -> pa.Table:
        if self._row_offset:
            first = self._offset
            if self._fragment_ids is not None:
                starts, at = {}, 0
                for f in self._ds._info["fragments"]:
                    starts[f["id"]] = at
                    at += int(f["physical_rows"]) - int(f["deleted_rows"])
                first += starts[self._fragment_ids[0]] if len(self._fragment_ids) == 1 else 0
            table = table.append_column("_rowoffset", pa.array(range(first, first + table.num_rows), pa.uint64()))
        if self._order is not None:
            return table.select(self._order)
        if self._names is not None:
            table = table.select(self._names + [c for c in ("_rowid", "_rowaddr") if c in table.column_names])
        table = _rename(table, self._columns)
        meta = self._ds._info["schema_metadata"]
        if meta and table.schema.metadata != meta:
            table = table.replace_schema_metadata(meta)
        return table

    def to_table(self) -> pa.Table:
        return self._shape(pa.table(self._read(stream=False)))

    def to_reader(self) -> pa.RecordBatchReader:
        table = self.to_table()
        return pa.RecordBatchReader.from_batches(table.schema, self._rebatch(table))

    def _rebatch(self, table: pa.Table) -> Iterator[pa.RecordBatch]:
        if self._batch_size:
            return iter(table.to_batches(max_chunksize=int(self._batch_size)))
        return iter(table.to_batches())

    def to_batches(self) -> Iterator[pa.RecordBatch]:
        return self._rebatch(self.to_table())

    def count_rows(self) -> int:
        return self.to_table().num_rows

    @property
    def projected_schema(self) -> pa.Schema:
        return self.to_table().schema if self._names is not None else self._ds.schema

    @property
    def dataset_schema(self) -> pa.Schema:
        return self._ds.schema

    def explain_plan(self, verbose: bool = False) -> str:
        return f"nanolance scan of {self._ds.uri} v{self._ds.version}"

    def analyze_plan(self) -> str:
        return self.explain_plan()


class ScannerBuilder:
    """Mirrors ``lance.dataset.ScannerBuilder``: the scanner, configured one call at a time."""

    def __init__(self, ds: LanceDataset):
        self._ds = ds
        self._options: Dict[str, Any] = {}

    def _set(self, **kw) -> "ScannerBuilder":
        self._options.update(kw)
        return self

    def columns(self, cols=None) -> "ScannerBuilder":
        return self._set(columns=cols)

    def filter(self, filter) -> "ScannerBuilder":
        return self._set(filter=filter)

    def limit(self, n=None) -> "ScannerBuilder":
        return self._set(limit=n)

    def offset(self, n=None) -> "ScannerBuilder":
        return self._set(offset=n)

    def batch_size(self, batch_size: int) -> "ScannerBuilder":
        return self._set(batch_size=batch_size)

    def with_row_id(self, enabled: bool = True) -> "ScannerBuilder":
        return self._set(with_row_id=enabled)

    def with_row_address(self, enabled: bool = True) -> "ScannerBuilder":
        return self._set(with_row_address=enabled)

    def with_fragments(self, fragments) -> "ScannerBuilder":
        return self._set(fragments=fragments)

    def nearest(self, *args, **kwargs) -> "ScannerBuilder":
        raise unsupported("vector search (nearest=...)")

    def __getattr__(self, name):
        # Tuning knobs with no effect on the result (readahead, io buffers, ...).
        if name.startswith("_"):
            raise AttributeError(name)
        return lambda *args, **kwargs: self

    def to_scanner(self) -> LanceScanner:
        return LanceScanner(self._ds, **self._options)


# ── writes ────────────────────────────────────────────────────────────────────────────────────────


def _coerce_reader(data_obj: ReaderLike, schema: Optional[pa.Schema] = None) -> pa.RecordBatchReader:
    """Any of the inputs pylance's write_dataset takes, as a RecordBatchReader.

    Follows pylance's ``lance.types._coerce_reader`` (Apache-2.0, The Lance Authors).
    """
    if type(data_obj).__module__.startswith("pandas") and type(data_obj).__name__ == "DataFrame":
        return pa.Table.from_pandas(data_obj, schema=schema).to_reader()
    if isinstance(data_obj, pa.Table):
        return (data_obj if schema is None else data_obj.cast(schema)).to_reader()
    if isinstance(data_obj, pa.RecordBatch):
        table = pa.Table.from_batches([data_obj])
        return (table if schema is None else table.cast(schema)).to_reader()
    if isinstance(data_obj, LanceDataset):
        return data_obj.scanner().to_reader()
    if isinstance(data_obj, LanceScanner):
        return data_obj.to_reader()
    if isinstance(data_obj, pa.RecordBatchReader):
        return data_obj
    if type(data_obj).__module__.startswith("polars") and type(data_obj).__name__ == "DataFrame":
        return data_obj.to_arrow().to_reader()
    try:
        import pyarrow.dataset as pds

        if isinstance(data_obj, pds.Dataset):
            return pds.Scanner.from_dataset(data_obj).to_reader()
        if isinstance(data_obj, pds.Scanner):
            return data_obj.to_reader()
    except ImportError:  # pragma: no cover
        pass
    if isinstance(data_obj, dict):
        batch = pa.RecordBatch.from_pydict(data_obj, schema=schema)
        return pa.RecordBatchReader.from_batches(batch.schema, [batch])
    if isinstance(data_obj, list) and data_obj and isinstance(data_obj[0], dict):
        batch = pa.RecordBatch.from_pylist(data_obj, schema=schema)
        return pa.RecordBatchReader.from_batches(batch.schema, [batch])
    if isinstance(data_obj, list) and data_obj and _is_pydantic(data_obj[0]):
        from nanolance.lance.pydantic import _pydantic_reader

        return _pydantic_reader(data_obj, schema, type(data_obj[0]))
    if hasattr(data_obj, "__arrow_c_stream__"):
        return pa.RecordBatchReader.from_stream(data_obj, schema=schema)
    if isinstance(data_obj, Iterable):
        if schema is None:
            raise TypeError("a schema is required to write an iterable of batches")

        def cast_batches():
            for batch in data_obj:
                if isinstance(batch, pa.Table):
                    for b in batch.to_batches():
                        yield b.cast(schema) if b.schema != schema else b
                elif isinstance(batch, pa.RecordBatch):
                    yield batch.cast(schema) if batch.schema != schema else batch
                else:
                    raise TypeError(f"expected RecordBatch, got {type(batch)}")

        return pa.RecordBatchReader.from_batches(schema, cast_batches())
    raise TypeError(f"Unknown data type {type(data_obj)}. Please check the documentation.")


def _is_pydantic(obj) -> bool:
    try:
        from pydantic import BaseModel
    except ImportError:
        return False
    return isinstance(obj, BaseModel)


_MODES = {"create": _nanolance.COMMIT_CREATE, "append": _nanolance.COMMIT_APPEND,
          "overwrite": _nanolance.COMMIT_OVERWRITE}


def write_dataset(
    data_obj: ReaderLike,
    uri: Optional[Union[str, Path, LanceDataset]] = None,
    schema: Optional[pa.Schema] = None,
    mode: str = "create",
    *,
    max_rows_per_file: int = 1024 * 1024,
    max_rows_per_group: int = 1024,
    max_bytes_per_file: int = 90 * 1024 * 1024 * 1024,
    commit_lock=None,
    progress=None,
    storage_options: Optional[Dict[str, str]] = None,
    data_storage_version: Optional[str] = None,
    use_legacy_format: Optional[bool] = None,
    enable_v2_manifest_paths: bool = True,
    enable_stable_row_ids: bool = False,
    auto_cleanup_options=None,
    commit_message: Optional[str] = None,
    transaction_properties: Optional[Dict[str, str]] = None,
    initial_bases=None,
    target_bases=None,
    namespace=None,
    table_id=None,
    **kwargs,
) -> LanceDataset:
    """Write data to a Lance dataset. Mirrors ``lance.write_dataset``: one new version per call."""
    if uri is None:
        raise ValueError("uri is required")
    if mode not in _MODES:
        raise ValueError(f"Invalid mode: {mode}; expected one of create, append, overwrite")
    if data_storage_version not in (None, "stable", "2.2", "next") or use_legacy_format:
        raise unsupported(f"data_storage_version={data_storage_version!r} (nanolance writes 2.2)")
    if enable_stable_row_ids:
        raise unsupported("stable row ids")
    if max_rows_per_file is not None and int(max_rows_per_file) <= 0:
        raise ValueError("max_rows_per_file must be greater than 0")
    path = _path_of(uri)
    reader = _coerce_reader(data_obj, schema)
    exists = _exists(path)
    if mode == "create" and exists:
        raise OSError(f"Dataset already exists: {path}")
    append = mode == "append" and exists
    target = None
    if append:
        target = LanceDataset(path).schema
        _check_append_schema(target, reader.schema)
    options = _nanolance.WriteOptions()
    with native():
        writer = _nanolance._StagedWriter(path, options, append, int(max_rows_per_file or 0),
                                          int(max_bytes_per_file or 0) if max_bytes_per_file < 2**62 else 0)
    wrote = False
    limit = int(max_rows_per_file or 0)
    in_file = 0
    for batch in reader:
        if target is not None:
            batch = _conform(batch, target)
        if batch.num_rows == 0 and wrote:
            continue
        # A file (fragment) ends at max_rows_per_file rows, wherever that falls in a batch.
        start = 0
        while True:
            take = batch.num_rows - start if not limit else min(batch.num_rows - start, limit - in_file)
            piece = batch.slice(start, take) if (start or take != batch.num_rows) else batch
            with native():
                writer.write_batch(piece)
            wrote = True
            in_file = (in_file + take) % limit if limit else 0
            start += take
            if start >= batch.num_rows:
                break
    if not wrote:
        empty_schema = target if target is not None else reader.schema
        with native():
            writer.write_batch(pa.RecordBatch.from_pylist([], schema=empty_schema))
    code = _MODES["append" if append else ("create" if mode == "create" else "overwrite")]
    with native():
        writer.finish(code)
    return LanceDataset(path)


def _check_append_schema(target: pa.Schema, given: pa.Schema) -> None:
    missing = [n for n in target.names if n not in given.names]
    unexpected = [n for n in given.names if n not in target.names]
    if missing or unexpected:
        raise OSError(
            f"Append with different schema: fields did not match, missing={missing}, unexpected={unexpected}"
        )


def _conform(batch: pa.RecordBatch, target: pa.Schema) -> pa.RecordBatch:
    """The batch in the dataset's column order and types."""
    if batch.schema.equals(target, check_metadata=False):
        return batch
    columns = [batch.column(batch.schema.get_field_index(f.name)) for f in target]
    out = []
    for col, field in zip(columns, target):
        out.append(col if col.type == field.type else col.cast(field.type))
    return pa.RecordBatch.from_arrays(out, schema=target)


def dataset(
    uri: Union[str, Path],
    version: Optional[Union[int, str]] = None,
    asof=None,
    block_size: Optional[int] = None,
    commit_lock=None,
    index_cache_size: Optional[int] = None,
    storage_options: Optional[Dict[str, str]] = None,
    default_scan_options: Optional[Dict[str, Any]] = None,
    metadata_cache_size_bytes: Optional[int] = None,
    index_cache_size_bytes: Optional[int] = None,
    read_params: Optional[Dict[str, Any]] = None,
    session=None,
    **kwargs,
) -> LanceDataset:
    """Open a Lance dataset. Mirrors ``lance.dataset``."""
    if asof is not None:
        ds = LanceDataset(uri)
        ts = asof if isinstance(asof, datetime) else datetime.fromisoformat(str(asof))
        candidates = [v for v in ds.versions() if v["timestamp"] <= ts]
        if not candidates:
            raise ValueError(f"no version of {uri} at or before {asof}")
        return LanceDataset(uri, version=candidates[-1]["version"])
    return LanceDataset(uri, version=version, default_scan_options=default_scan_options)


def __getattr__(name: str):
    """pylance names this module does not implement import as placeholders (see _stubs.py)."""
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.dataset.{name}")
    globals()[name] = value
    return value
