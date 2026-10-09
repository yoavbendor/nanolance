# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.dataset`` as nanolance implements it: LanceDataset, its scanner, and write_dataset.

The names, signatures, defaults and exception types follow pylance's (Apache-2.0, The Lance Authors),
so code written against ``lance`` runs unchanged on the part of the API listed in
docs/PYLANCE_COMPAT.md. What is not implemented raises ``NotImplementedError`` naming the feature;
nothing is silently ignored when ignoring it would change a result.
"""

from __future__ import annotations

import contextlib
import dataclasses
import os
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any, Dict, Iterable, Iterator, List, Optional, Sequence, Tuple, TypedDict, Union

import numpy as np
import pyarrow as pa

from nanolance import _nanolance
from nanolance.lance._errors import native, unsupported
from nanolance.lance._transactions import Index, LanceOperation, Transaction

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


def _path_parts(text: str) -> List[str]:
    """A field path's segments: `a.b`, with back-quoted segments for names with other characters
    (`` `meta-data`.`user-id` ``)."""
    parts, current, quoted, i = [], "", False, 0
    while i < len(text):
        c = text[i]
        if c == "`":
            if quoted and i + 1 < len(text) and text[i + 1] == "`":
                current += "`"  # a doubled backtick inside a quoted name
                i += 2
                continue
            quoted = not quoted
        elif c == "." and not quoted:
            parts.append(current)
            current = ""
        else:
            current += c
        i += 1
    parts.append(current)
    return parts


def _resolve_path(schema: pa.Schema, text: str) -> Optional[List[str]]:
    """The fields a column path names, as Lance resolves it: a top-level name as written first, then
    each segment by its exact name, else by its only case-insensitive match. None: no such field."""
    if text in schema.names:
        return [text]
    fields = list(schema)
    out = []
    for i, part in enumerate(_path_parts(text)):
        if i > 0:
            if not pa.types.is_struct(node.type):
                return None
            fields = [node.type.field(k) for k in range(node.type.num_fields)]
        exact = [f for f in fields if f.name == part]
        loose = [f for f in fields if f.name.lower() == part.lower()]
        node = exact[0] if exact else (loose[0] if len(loose) == 1 else None)
        if node is None:
            return None
        out.append(node.name)
    return out


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
        self._uri = _path_of(uri)
        if isinstance(version, str):
            version = _tag_version(self._uri, version, ValueError)
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
        """The dataset's schema, with the row id columns its default scan options add (as Lance's)."""
        schema = _json_out_schema(self._data_schema)
        for option, name in (("with_row_id", "_rowid"), ("with_row_address", "_rowaddr")):
            if self._default_scan_options.get(option):
                schema = schema.append(pa.field(name, pa.uint64()))
        return schema

    @property
    def _data_schema(self) -> pa.Schema:
        """The schema of the data alone."""
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
        return max(len(self._data_schema.names) - 1, 0)

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

    def version_refs(self) -> List[Dict[str, int]]:
        """Every version, oldest first, as ``{"version": n}`` (main branch only)."""
        with native():
            return [{"version": int(v["version"])} for v in _nanolance._ds_versions(self._uri)]

    @property
    def tags(self) -> "Tags":
        return Tags(self)

    def cleanup_old_versions(self, older_than: Optional[timedelta] = None, retain_versions: Optional[int] = None, *,
                             delete_unverified: bool = False, error_if_tagged_old_versions: bool = True,
                             delete_rate_limit: Optional[int] = None,
                             versions: Optional[List[int]] = None) -> "CleanupStats":
        """Remove old versions and the files only they use, as Lance's cleanup removes them: the
        version this dataset is at, newer ones and tagged ones are kept; files no version references
        are removed only once 7 days old (unless ``delete_unverified``). ``older_than`` defaults to
        two weeks when nothing else selects versions."""
        _, stats, _, _ = self._cleanup(older_than, retain_versions, delete_unverified, error_if_tagged_old_versions,
                                       delete_rate_limit, versions, True, 0)
        return stats

    def explain_cleanup_old_versions(self, older_than: Optional[timedelta] = None,
                                     retain_versions: Optional[int] = None, *, delete_unverified: bool = False,
                                     error_if_tagged_old_versions: bool = True,
                                     delete_rate_limit: Optional[int] = None, versions: Optional[List[int]] = None,
                                     include_files: bool = False, max_files: int = 1000) -> "CleanupExplanation":
        """What ``cleanup_old_versions`` would remove, removing nothing."""
        if max_files <= 0:
            raise ValueError("max_files must be positive")
        read_version, stats, files, truncated = self._cleanup(
            older_than, retain_versions, delete_unverified, error_if_tagged_old_versions, delete_rate_limit,
            versions, False, int(max_files) if include_files else 0)
        return CleanupExplanation(read_version, stats, [CleanupCandidateFile(**f) for f in files], truncated,
                                  int(max_files) if include_files else 0)

    def _cleanup(self, older_than, retain_versions, delete_unverified, error_if_tagged, delete_rate_limit, versions,
                 execute, max_files):
        if older_than is None and retain_versions is None and versions is None:
            older_than = timedelta(days=14)
        older_than_ns = None
        if older_than is not None:
            older_than_ns = (older_than.days * 86400 + older_than.seconds) * 10**9 + older_than.microseconds * 1000
        if retain_versions is not None and int(retain_versions) <= 0:
            raise OSError(f"Invalid user input: retain_versions must be greater than 0, got {int(retain_versions)}")
        with _ref_errors():
            read_version, stats, files, truncated = _nanolance._ds_cleanup(
                self._uri, self._version, older_than_ns, None if retain_versions is None else int(retain_versions),
                bool(delete_unverified), bool(error_if_tagged),
                None if delete_rate_limit is None else int(delete_rate_limit),
                None if versions is None else [int(v) for v in versions], bool(execute), int(max_files))
        return int(read_version), CleanupStats(**stats), files, bool(truncated)

    @staticmethod
    def drop(base_uri, storage_options: Optional[Dict[str, str]] = None,
             ignore_not_found: Optional[bool] = None) -> None:
        """Delete the dataset and everything under ``base_uri``. Refused (ValueError) unless the path
        holds a manifest that reads, as Lance refuses, so a parent directory given by mistake survives."""
        with _ref_errors():
            _nanolance._ds_drop(_path_of(base_uri), bool(ignore_not_found))

    def checkout_version(self, version) -> "LanceDataset":
        if isinstance(version, str):
            version = _tag_version(self._uri, version, OSError)
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
            substrait_filter=substrait_filter, scan_stats_callback=scan_stats_callback, blob_handling=blob_handling,
            use_scalar_index=use_scalar_index, prefilter=prefilter, fast_search=fast_search,
            disable_scoring_autoprojection=disable_scoring_autoprojection, batch_size_bytes=batch_size_bytes,
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
            include_deleted_rows=include_deleted_rows, order_by=order_by, use_scalar_index=use_scalar_index,
            prefilter=prefilter, fast_search=fast_search, **kwargs,
        ).to_table()

    def to_batches(self, columns=None, filter=None, limit=None, offset=None, nearest=None, batch_size=None,
                   batch_size_bytes=None, batch_readahead=None, fragment_readahead=None, scan_in_order=None,
                   *, prefilter=None, with_row_id=None, with_row_address=None, use_stats=None, full_text_query=None,
                   io_buffer_size=None, late_materialization=None, use_scalar_index=None,
                   strict_batch_size=None, order_by=None, **kwargs) -> Iterator[pa.RecordBatch]:
        return self.scanner(
            columns=columns, filter=filter, limit=limit, offset=offset, nearest=nearest, batch_size=batch_size,
            batch_size_bytes=batch_size_bytes,
            full_text_query=full_text_query, with_row_id=with_row_id, with_row_address=with_row_address,
            order_by=order_by, use_scalar_index=use_scalar_index, prefilter=prefilter, **kwargs,
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
            return self.scanner(filter=filter).count_rows()
        return sum(int(f["physical_rows"]) - int(f["deleted_rows"]) for f in self._info["fragments"])

    def take(self, indices, columns=None) -> pa.Table:
        wanted = _index_list(indices)
        names = _normalize_columns(columns)
        n = self.count_rows()
        for i in wanted:
            if i < 0 or i >= n:
                raise IndexError(f"index {i} is out of bounds for a dataset of {n} rows")
        return _json_out(_rename(self._take(wanted, names, addresses=False), columns))

    def _take_rows(self, row_ids, columns=None, **kwargs) -> pa.Table:
        if self.has_stable_row_ids:
            raise unsupported("taking rows of a dataset with stable row ids")
        return _json_out(_rename(self._take(_index_list(row_ids), _normalize_columns(columns), addresses=True),
                                 columns))

    def take_rows(self, row_ids, columns=None, **kwargs) -> pa.Table:
        return self._take_rows(row_ids, columns, **kwargs)

    def _take(self, wanted: List[int], names, addresses: bool, with_row_id=False, with_row_address=False,
              blob_handling=None):
        blob_handling = _nanolance.BLOB_DESCRIPTIONS if blob_handling is None else blob_handling
        order = None
        derived = []  # system columns computed here rather than read
        if names is not None and any(n in _SYSTEM_COLUMNS for n in names):
            order = list(names)
            with_row_id = with_row_id or "_rowid" in names
            with_row_address = with_row_address or "_rowaddr" in names
            derived = [n for n in names if n in ("_rowoffset",) + _VERSION_COLUMNS]
            if "_rowoffset" in derived and addresses:
                raise unsupported("_rowoffset when taking rows by id or address")
            names = [n for n in names if n not in _SYSTEM_COLUMNS]
            if not names and not (with_row_id or with_row_address):
                names, with_row_address = [], True  # something to count the rows by
        distinct = sorted(set(wanted))
        with native():
            table = pa.table(_nanolance._ds_take(self._uri, self._version, distinct, names, with_row_id,
                                                 with_row_address, addresses, blob_handling))
        if wanted != distinct:
            position = {row: k for k, row in enumerate(distinct)}
            table = table.take(pa.array([position[i] for i in wanted], pa.int64()))
        for column in derived:
            if column == "_rowoffset":
                table = table.append_column(pa.field(column, pa.uint64(), nullable=False),
                                            pa.array(wanted, pa.uint64()))
            else:
                table = table.append_column(column, self._row_versions(table.num_rows))
        if order is not None:
            return table.select(order)
        if names is not None:
            table = table.select(names + [c for c in ("_rowid", "_rowaddr") if c in table.column_names])
        return table

    def _row_versions(self, rows: int) -> pa.Array:
        """`_row_created_at_version` / `_row_last_updated_at_version`: Lance keeps them per row only
        with stable row ids; without, every row reads as version 1, as in Lance."""
        if self.has_stable_row_ids:
            raise unsupported("row version columns of a dataset with stable row ids")
        return pa.array([1] * rows, pa.uint64())

    # ── blobs ───────────────────────────────────────────────────────────────────────────────────────

    def _blob_rows(self, blob_column: str, ids, addresses, indices):
        """The Locations read of `blob_column` for the selected rows, in selection order."""
        given = [(k, v) for k, v in (("ids", ids), ("addresses", addresses), ("indices", indices)) if v is not None]
        if len(given) != 1:
            raise ValueError("Exactly one of ids, addresses, or indices must be specified")
        kind, values = given[0]
        wanted = _index_list(values)
        if kind == "indices":
            n = self.count_rows()
            for i in wanted:
                if i < 0 or i >= n:
                    raise IndexError(f"index {i} is out of bounds for a dataset of {n} rows")
        field = self._data_schema.field(blob_column) if blob_column in self._data_schema.names else None
        if field is None:
            raise ValueError(f"column {blob_column!r} does not exist")
        table = self._take(wanted, [blob_column], addresses=kind != "indices",
                           with_row_address=True, blob_handling=_nanolance.BLOB_LOCATIONS)
        column = table.column(blob_column)
        if not pa.types.is_struct(column.type) or column.type.get_field_index("file") < 0:
            raise ValueError(f"column {blob_column!r} is not a blob column")
        return table.column("_rowaddr").to_pylist(), column.to_pylist()

    def take_blobs(self, blob_column: str, ids=None, addresses=None, indices=None) -> List[Optional["BlobFile"]]:
        """One file-like :class:`lance.BlobFile` per selected row (``None`` for a null value)."""
        from .blob import BlobFile

        _, rows = self._blob_rows(blob_column, ids, addresses, indices)
        return [None if r is None else
                BlobFile(r["file"], r["kind"] == 3, 0 if r["kind"] == 2 else r["position"], r["size"])
                for r in rows]

    def read_blobs(self, blob_column: str, ids=None, addresses=None, indices=None, *, io_buffer_size=None,
                   preserve_order=None) -> List[Tuple[int, Optional[bytes]]]:
        """``(row_address, bytes)`` per selected row (``None`` for a null value), in selection order."""
        addrs, _ = self._blob_rows(blob_column, ids, addresses, indices)
        files = self.take_blobs(blob_column, ids=ids, addresses=addresses, indices=indices)
        return [(a, None if f is None else f.readall()) for a, f in zip(addrs, files)]

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

    # ── changes ───────────────────────────────────────────────────────────────────────────────────

    def delete(self, predicate, *, conflict_retries: int = 10, retry_timeout=None) -> Dict[str, int]:
        """Delete the rows where `predicate` (SQL or a pyarrow expression) is true. One new version."""
        sql = _filter_sql(predicate)
        if sql is None:
            raise ValueError("delete needs a predicate")
        with native():
            deleted, _ = _nanolance._ds_delete(self._uri, sql)
        self._refresh_latest()
        return {"num_deleted_rows": int(deleted)}

    def update(self, updates: Dict[str, str], where: Optional[str] = None, conflict_retries: int = 10,
               retry_timeout=None) -> Dict[str, int]:
        """Set columns to SQL values on the rows `where` selects (every row when None)."""
        if not updates:
            raise ValueError("update needs at least one column to set")
        assignments = [(str(k), str(v)) for k, v in updates.items()]
        with native():
            updated, _ = _nanolance._ds_update(self._uri, _filter_sql(where), assignments)
        self._refresh_latest()
        return {"num_rows_updated": int(updated)}

    def merge_insert(self, on=None) -> "MergeInsertBuilder":
        return MergeInsertBuilder(self, on)

    def add_columns(self, transforms, read_columns=None, reader_schema=None, batch_size=None) -> None:
        """New columns: {name: SQL expression}, all-null fields (a pyarrow Field, list of Fields or
        Schema), or data (a table / reader with one row per row of the dataset)."""
        if isinstance(transforms, dict):
            columns = [(str(k), str(v)) for k, v in transforms.items()]
            with native():
                _nanolance._ds_add_columns_sql(self._uri, columns)
        elif isinstance(transforms, (pa.Field, pa.Schema)) or (
            isinstance(transforms, list) and transforms and all(isinstance(f, pa.Field) for f in transforms)
        ):
            fields = [transforms] if isinstance(transforms, pa.Field) else list(transforms)
            with native():
                _nanolance._ds_add_columns_nulls(self._uri, pa.schema(fields))
        elif callable(transforms):
            raise unsupported("add_columns with a UDF")
        else:
            reader = _coerce_reader(transforms, reader_schema)
            with native():
                _nanolance._ds_add_columns_stream(self._uri, reader)
        self._refresh_latest()

    def truncate_table(self) -> None:
        """Delete every row, keeping the schema: a new version (Lance's delete("true"))."""
        self.delete("true")

    def merge(self, data_obj, left_on: str, right_on: Optional[str] = None, schema=None) -> None:
        """Add the columns of ``data_obj`` (a table, reader, dataset ...), matched to this dataset's
        rows on ``left_on`` = ``right_on`` (Lance's hash join: rows without a match get nulls; the
        right key column itself is not added)."""
        right_on = left_on if right_on is None else right_on
        if left_on not in self._data_schema.names and _resolve_path(self._data_schema, left_on) is None:
            raise OSError(f"Invalid user input: Column {left_on} does not exist in the left side dataset")
        if isinstance(data_obj, LanceDataset):
            right = data_obj.to_table()
        else:
            right = _coerce_reader(data_obj, schema).read_all()
        if right_on not in right.schema.names:
            raise OSError(f"Invalid user input: Column {right_on} does not exist in the right side dataset")
        for name in right.schema.names:
            if name != right_on and (name in self._data_schema.names):
                raise OSError(f"Invalid user input: Column {name} exists in both sides of the dataset")
        if right.num_rows == 0:
            raise OSError("Invalid user input: HashJoiner: No data")
        if any(f["deleted_rows"] for f in self._info["fragments"]):
            raise unsupported("merge into a dataset with deleted rows")
        keys = self.to_table(columns=[left_on]).column(0)
        index: Dict[Any, int] = {}
        for row, key in enumerate(right.column(right_on).to_pylist()):
            if key is not None:
                index[key] = row  # a repeated key: its last row
        take = pa.array([index.get(k) for k in keys.to_pylist()], pa.int64())
        added = right.drop_columns([right_on]).take(take)
        fields = [f.with_nullable(True) if added.column(f.name).null_count else f for f in added.schema]
        added = pa.table(added.columns, schema=pa.schema(fields, metadata=added.schema.metadata))
        with native():
            _nanolance._ds_add_columns_stream(self._uri, pa.RecordBatchReader.from_batches(
                added.schema, added.to_batches()))
        self._refresh_latest()

    def drop_columns(self, columns: List[str]) -> None:
        if isinstance(columns, str):
            columns = [columns]
        with native():
            _nanolance._ds_drop_columns(self._uri, [str(c) for c in columns])
        self._refresh_latest()

    def alter_columns(self, *alterations) -> None:
        items = []
        for a in alterations:
            if not isinstance(a, dict) or "path" not in a:
                raise ValueError("each alteration must be a dict with a 'path'")
            unknown = set(a) - {"path", "name", "nullable", "data_type"}
            if unknown:
                raise ValueError(f"unknown alteration keys: {sorted(unknown)}")
            item = dict(a)
            if item.get("data_type") is not None:
                item["data_type"] = pa.field("x", item["data_type"])
            items.append(item)
        with native():
            _nanolance._ds_alter_columns(self._uri, items)
        self._refresh_latest()

    # ── indices ───────────────────────────────────────────────────────────────────────────────────

    def create_scalar_index(self, column: str, index_type: str, name: Optional[str] = None, *,
                            replace: bool = True, **kwargs) -> "LanceDataset":
        """Build a BTREE, BITMAP, LABEL_LIST or INVERTED (full-text) index on `column`, in Lance's own
        format: pylance and LanceDB use it as one they built. Commits a new version."""
        if isinstance(column, str):
            path = _resolve_path(self._data_schema, column)
            if path is not None:
                column = ".".join(path)  # the schema's own names, as Lance stores them
        if str(index_type).upper() in ("INVERTED", "FTS"):
            return self._create_inverted_index(column, name, replace, kwargs)
        if kwargs:
            raise unsupported(f"create_scalar_index options {sorted(kwargs)}")
        with native():
            _nanolance._ds_create_scalar_index(self._uri, str(column), str(index_type), name or "", bool(replace))
        self._refresh_latest()
        return self

    _INVERTED_OPTIONS = ("base_tokenizer", "language", "with_position", "max_token_length", "lower_case", "stem",
                         "remove_stop_words", "custom_stop_words", "ascii_folding", "min_ngram_length",
                         "max_ngram_length", "prefix_only")

    def _create_inverted_index(self, column, name, replace, kwargs) -> "LanceDataset":
        import json

        params = {}
        for key in list(kwargs):
            if key in self._INVERTED_OPTIONS:
                value = kwargs.pop(key)
                if value is not None or key in ("max_token_length", "custom_stop_words"):
                    params[key] = value
        for ignored in ("num_workers", "memory_limit", "progress", "train"):
            kwargs.pop(ignored, None)
        if kwargs:
            raise unsupported(f"INVERTED index options {sorted(kwargs)}")
        if isinstance(column, (list, tuple)):
            if len(column) != 1:
                raise unsupported("an index over more than one column")
            column = column[0]
        with _lance_errors():
            _nanolance._ds_create_inverted_index(self._uri, str(column), name or "", bool(replace),
                                                 json.dumps(params))
        self._refresh_latest()
        return self

    _VECTOR_INDEX_TYPES = ["IVF_FLAT", "IVF_PQ", "IVF_SQ", "IVF_HNSW_FLAT", "IVF_HNSW_PQ", "IVF_HNSW_SQ", "IVF_RQ"]

    def create_index(self, column, index_type: str, name: Optional[str] = None, metric: str = "L2",
                     replace: bool = False, num_partitions: Optional[int] = None, ivf_centroids=None,
                     pq_codebook=None, num_sub_vectors: Optional[int] = None, accelerator=None,
                     index_cache_size: Optional[int] = None, shuffle_partition_batches: Optional[int] = None,
                     shuffle_partition_concurrency: Optional[int] = None, ivf_centroids_file=None,
                     precomputed_partition_dataset=None, storage_options=None, filter_nan: bool = True,
                     train: bool = True, fragment_ids=None, index_uuid=None, *,
                     target_partition_size: Optional[int] = None, **kwargs) -> "LanceDataset":
        """Build an index on `column`, in Lance's format. IVF_FLAT, IVF_PQ and IVF_HNSW_SQ vector indexes
        (pylance and LanceDB search them as their own); scalar index types go to create_scalar_index."""
        kind = str(index_type).upper()
        if isinstance(column, (list, tuple)):
            if len(column) != 1:
                raise unsupported("an index over more than one column")
            column = column[0]
        if not kind.startswith("IVF") and kind not in ("VECTOR",):
            return self.create_scalar_index(column, kind, name, replace=replace, **kwargs)
        if kind not in self._VECTOR_INDEX_TYPES:
            raise NotImplementedError(f"Only {self._VECTOR_INDEX_TYPES} index types supported. Got {index_type}")
        if kind not in ("IVF_FLAT", "IVF_PQ", "IVF_HNSW_SQ"):
            raise unsupported(f"{kind} indexes (IVF_FLAT, IVF_PQ and IVF_HNSW_SQ are supported)")
        for option, value in (("ivf_centroids", ivf_centroids), ("pq_codebook", pq_codebook),
                              ("ivf_centroids_file", ivf_centroids_file), ("accelerator", accelerator),
                              ("precomputed_partition_dataset", precomputed_partition_dataset),
                              ("fragment_ids", fragment_ids), ("index_uuid", index_uuid)):
            if value is not None:
                raise unsupported(f"create_index option {option}")
        if not train:
            raise unsupported("create_index(train=False)")
        if kind == "IVF_PQ" and num_sub_vectors is None:
            raise ValueError("num_partitions and num_sub_vectors are required for IVF_PQ")
        num_bits = int(kwargs.pop("num_bits", 8))
        max_iters = int(kwargs.pop("max_iters", 50))
        sample_rate = int(kwargs.pop("sample_rate", 256))
        seed = kwargs.pop("seed", None)
        hnsw_m = int(kwargs.pop("m", 20))
        ef_construction = int(kwargs.pop("ef_construction", 150))
        max_level = int(kwargs.pop("max_level", 7))
        for ignored in ("one_pass_ivfpq", "skip_transpose", "streaming_sample_rate", "streaming_coreset_rate",
                        "streaming_refine_passes", "progress", "kmeans_redos"):
            kwargs.pop(ignored, None)
        if kwargs:
            raise unsupported(f"create_index options {sorted(kwargs)}")
        field = next((f for f in self._data_schema if f.name == str(column)), None)
        if field is not None:
            storage = getattr(field.type, "storage_type", field.type)
            if not pa.types.is_fixed_size_list(storage):
                raise TypeError(f"Vector column {column} must be FixedSizeListArray 1-dimensional "
                                f"FixedShapeTensorArray, got {field.type}")
        with _lance_errors():
            _nanolance._ds_create_vector_index(
                self._uri, str(column), kind, name or "", str(metric).lower(), bool(replace),
                None if num_partitions is None else int(num_partitions),
                None if target_partition_size is None else int(target_partition_size),
                0 if num_sub_vectors is None else int(num_sub_vectors), num_bits, max_iters, sample_rate,
                None if seed is None else int(seed), hnsw_m, ef_construction, max_level)
        self._refresh_latest()
        return self

    def drop_index(self, name: str) -> None:
        with native():
            _nanolance._ds_drop_index(self._uri, str(name))
        self._refresh_latest()

    def list_indices(self) -> List[Dict[str, Any]]:
        with native():
            indices = _nanolance._ds_list_indices(self._uri, self._version)
        return [{"name": i["name"], "type": i["type"], "uuid": i["uuid"], "fields": i["fields"],
                 "version": i["dataset_version"], "fragment_ids": set(i["fragment_ids"]), "base_id": None}
                for i in indices]

    def has_index(self) -> bool:
        return bool(self.list_indices())

    def describe_indices(self) -> List["IndexDescription"]:
        with native():
            indices = _nanolance._ds_list_indices(self._uri, self._version)
        by_name: Dict[str, List[dict]] = {}
        for i in indices:
            by_name.setdefault(i["name"], []).append(i)
        out = []
        for name, segments in by_name.items():
            first = segments[0]
            out.append(IndexDescription(
                name=name, type_url=first["type_url"], index_type=first["type"] if first["type_url"] else "Unknown",
                fields=list(first["field_ids"]), field_names=list(first["fields"]),
                num_rows_indexed=sum(s["rows_indexed"] for s in segments),
                total_size_bytes=sum(s["size_bytes"] for s in segments), details={},
                segments=[IndexSegmentDescription(
                    uuid=s["uuid"], fragment_ids=set(s["fragment_ids"]), index_version=s["index_version"],
                    dataset_version_at_last_update=s["dataset_version"], size_bytes=s["size_bytes"],
                    created_at=datetime.fromtimestamp(s["created_at"] / 1000) if s["created_at"] else None,
                    base_id=None, covering_fields=list(first["field_ids"])) for s in segments],
            ))
        return out

    @property
    def optimize(self) -> "DatasetOptimizer":
        return DatasetOptimizer(self)

    def read_transaction(self, version: int) -> Optional["Transaction"]:
        """The transaction that made ``version``, from the file its manifest names; None without one."""
        from nanolance.lance._transactions import decode_transaction

        with native():
            info = _nanolance._ds_info(self._uri, int(version))
        name = info.get("transaction_file") or ""
        if not name:
            return None
        try:
            with open(os.path.join(self._uri, "_transactions", name), "rb") as handle:
                data = handle.read()
        except FileNotFoundError:
            return None
        return decode_transaction(data)

    def validate(self) -> None:
        """Check the manifest against itself and the files, as Lance's validate does: fragment ids
        unique and ascending, no field in two files of a fragment, every data file as long as its
        fragment, index segments unique and not overlapping."""
        path = self._uri
        fragments = self._info["fragments"]
        ids = [f["id"] for f in fragments]
        for i in sorted(set(ids)):
            if ids.count(i) > 1:
                raise OSError(f"LanceError(IO): Duplicate fragment id {i} found in dataset {path!r}")
        for prev, cur in zip(ids, ids[1:]):
            if cur < prev:
                raise OSError("LanceError(IO): Fragment ids are not sorted in increasing fragment-id order. "
                              f"Found {cur} after {prev} in dataset {path!r}")
        for f in fragments:
            seen = set()
            lengths = []
            for file in f["files"]:
                for field_id in file["fields"]:
                    if field_id == -2:
                        continue  # a tombstone
                    if field_id in seen:
                        raise OSError(f"LanceError(IO): Field id {field_id} is duplicated in fragment {f['id']}")
                    seen.add(field_id)
                with native():
                    lengths.append(int(_nanolance._file_info(os.path.join(path, "data", file["path"]))[0]))
            for file, n in zip(f["files"], lengths):
                if n != lengths[0]:
                    raise OSError(f"LanceError(IO): data file has incorrect length. Expected: {lengths[0]} "
                                  f"Got: {n}")
            if lengths and lengths[0] != f["physical_rows"]:
                raise OSError("LanceError(IO): Fragment metadata has incorrect physical_rows. "
                              f"Actual: {lengths[0]} Metadata: {f['physical_rows']}")
        with native():
            indices = _nanolance._ds_list_indices(self._uri, self._version)
        uuids = [i["uuid"] for i in indices]
        for u in sorted(set(uuids)):
            if uuids.count(u) > 1:
                raise OSError(f"LanceError(IO): Duplicate index id {u} found in dataset {path!r}")
        by_name: Dict[str, List[set]] = {}
        for i in indices:
            by_name.setdefault(i["name"], []).append(set(i["fragment_ids"]))
        bad = []
        for name, covers in by_name.items():
            overlap = set()
            for a in range(len(covers)):
                for b in range(a + 1, len(covers)):
                    overlap |= covers[a] & covers[b]
            if overlap:
                bad.append((name, sorted(overlap)))
        if bad:
            message = "Overlapping fragments detected in dataset."
            for name, frags in bad:
                message += f"\nIndex {name!r} has overlapping fragments: {frags}"
            raise OSError(f"LanceError(IO): {message}")

    def get_transactions(self, recent_transactions: int = 10) -> List[Optional["Transaction"]]:
        """The transactions of this version and those before it, newest first (None where a version
        has no transaction file)."""
        out = []
        version = self._version
        while version >= 1 and len(out) < int(recent_transactions):
            try:
                out.append(self.read_transaction(version))
            except OSError:
                break  # cleaned up
            version -= 1
        return out

    @property
    def lance_schema(self) -> "LanceSchema":
        """The schema as Lance holds it: field ids, parents, logical types, metadata."""
        from nanolance.lance.schema import LanceSchema

        with native():
            fields = _nanolance._ds_fields(self._uri, self._version)
        metadata = {k.decode(): v.decode(errors="replace") for k, v in self._info["schema_metadata"].items()}
        return LanceSchema(fields, metadata, self.schema)

    def update_field_metadata(self, field_updates: Dict[str, Dict[str, Optional[str]]], *,
                              replace: bool = False) -> None:
        """Set (a value) or remove (None) metadata keys of fields named by path; with ``replace``, a
        field's metadata becomes exactly the keys given a value."""
        if not isinstance(field_updates, dict):
            raise TypeError(f"argument 'field_updates': '{type(field_updates).__name__}' object cannot be "
                            "converted to 'PyDict'")
        schema = self.lance_schema
        by_id = {}
        for path, values in field_updates.items():
            if not isinstance(path, str):
                raise TypeError(f"argument 'field_updates': '{type(path).__name__}' object is not an "
                                "instance of 'str'")
            entries = []
            for k, v in dict(values).items():
                for x in (k,) if v is None else (k, v):
                    if not isinstance(x, str):
                        raise TypeError(f"argument 'field_updates': '{type(x).__name__}' object is not an "
                                        "instance of 'str'")
                entries.append((k, v))
            field = schema.field(path)
            if field is None:
                names = []

                def walk(fields, prefix):
                    for f in fields:
                        names.append(prefix + f.name())
                        walk(f.children(), prefix + f.name() + ".")

                walk(schema.fields(), "")
                raise OSError(f"Field '{path}' not found.\nAvailable fields: {names}")
            by_id[field.id()] = entries
        with native():
            _nanolance._ds_update_field_metadata(self._uri, by_id, bool(replace))
        self._refresh_latest()

    @property
    def stats(self) -> "LanceStats":
        return LanceStats(self)

    def index_statistics(self, index_name: str) -> str:
        """The statistics of an index as Lance reports them (a JSON string; stats.index_stats parses it)."""
        import json

        return json.dumps(_index_statistics(self, index_name))

    # ── not supported ─────────────────────────────────────────────────────────────────────────────

    def __getattr__(self, name: str):
        known = {
            "create_index",
            "branches", "create_branch", "sql",
            "shallow_clone", "deep_clone", "commit", "commit_batch", "session", "join", "delta",
            "prewarm_index",
        }
        if name in known:
            raise unsupported(f"LanceDataset.{name}")
        raise AttributeError(name)


def _format_duration(seconds: int) -> str:
    """Seconds as humantime's format_duration writes them ("14days", "1h 30m", "1s")."""
    if seconds <= 0:
        return "0s"
    parts = []
    for size, one, many in ((31557600, "year", "years"), (2630016, "month", "months"), (86400, "day", "days"),
                            (3600, "h", "h"), (60, "m", "m"), (1, "s", "s")):
        n, seconds = divmod(seconds, size)
        if n:
            parts.append(f"{n}{one if n == 1 else many}")
    return " ".join(parts)


def _parse_rfc3339(text: str) -> Optional[datetime]:
    """A tag's time ("2026-10-08T16:00:17.502516072Z"), to the microsecond, in UTC."""
    if not text:
        return None
    import re
    from datetime import timezone

    m = re.match(r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d+))?(Z|[+-]\d{2}:\d{2})$", text)
    if m is None:
        return None
    fraction = (m.group(2) or "")[:6].ljust(6, "0")
    out = datetime.fromisoformat(f"{m.group(1)}.{fraction}" + ("+00:00" if m.group(3) == "Z" else m.group(3)))
    return out.astimezone(timezone.utc)


@contextlib.contextmanager
def _ref_errors():
    """Tag, cleanup and drop errors raised as pylance raises them."""
    try:
        yield
    except RuntimeError as exc:
        text = str(exc)
        if text.startswith(("Ref ", "Version not found", "Invalid user input: Refusing to drop")):
            raise ValueError(text) from None
        if text.startswith(("Cleanup error", "Invalid user input", "Dataset at path")):
            raise OSError(text) from None
        from nanolance.lance._errors import translate

        raise translate(text) from None


def _tag_reference(ds: "LanceDataset", reference) -> int:
    """A tag's target as pylance takes it: a version, another tag's name, ``(branch, version)`` on the
    main branch (``None`` for the latest), or ``None`` for the latest; 0 means the latest."""
    if reference is None:
        return 0
    if isinstance(reference, str):
        return _tag_version(ds.uri, reference, ValueError)
    if isinstance(reference, tuple):
        branch, number = reference
        if branch not in (None, "main"):
            raise unsupported("branches")
        return 0 if number is None else int(number)
    return int(reference)


def _tag_version(uri: str, name: str, error_type) -> int:
    for tag in _nanolance._ds_tags(uri):
        if tag["name"] == name:
            if tag["branch"] is not None:
                raise unsupported("branches")
            return int(tag["version"])
    raise error_type(f"Ref not found error: tag {name} does not exist")


class Tags:
    """Mirrors ``lance.dataset.Tags``: names for versions, kept by cleanup (``_refs/tags``, Lance's
    format: pylance sees nanolance's tags and nanolance pylance's)."""

    def __init__(self, dataset: "LanceDataset"):
        self._ds = dataset

    def _all(self):
        with _ref_errors():
            raw = _nanolance._ds_tags(self._ds.uri)
        return [(t["name"], {"branch": t["branch"], "version": int(t["version"]),
                             "created_at": _parse_rfc3339(t["created_at"]),
                             "updated_at": _parse_rfc3339(t["updated_at"]),
                             "manifest_size": int(t["manifest_size"]), "metadata": dict(t["metadata"])})
                for t in raw]

    def list(self) -> Dict[str, Dict[str, Any]]:
        return dict(self._all())

    def list_ordered(self, order: Optional[str] = None) -> List[Tuple[str, Dict[str, Any]]]:
        """By version, newest first (``order="desc"``, the default) or oldest first; then by name."""
        if order not in (None, "asc", "desc"):
            raise ValueError(f"Invalid order: {order}; expected 'asc' or 'desc'")
        tags = self._all()
        tags.sort(key=lambda t: t[0])
        tags.sort(key=lambda t: t[1]["version"], reverse=order != "asc")
        return tags

    def get_version(self, tag: str) -> int:
        return _tag_version(self._ds.uri, tag, ValueError)

    def create(self, tag: str, reference=None) -> None:
        version = _tag_reference(self._ds, reference)
        with _ref_errors():
            _nanolance._ds_create_tag(self._ds.uri, str(tag), version)

    def delete(self, tag: str) -> None:
        with _ref_errors():
            _nanolance._ds_delete_tag(self._ds.uri, str(tag))

    def update(self, tag: str, reference=None) -> None:
        version = _tag_reference(self._ds, reference)
        with _ref_errors():
            _nanolance._ds_update_tag(self._ds.uri, str(tag), version)

    def replace_metadata(self, tag: str, metadata: Dict[str, str]) -> None:
        with _ref_errors():
            _nanolance._ds_replace_tag_metadata(self._ds.uri, str(tag), {str(k): str(v) for k, v in metadata.items()})


class CleanupStats:
    """Mirrors ``lance.dataset.CleanupStats``: what a cleanup removed."""

    def __init__(self, bytes_removed=0, old_versions=0, data_files_removed=0, transaction_files_removed=0,
                 index_files_removed=0, deletion_files_removed=0):
        self.bytes_removed = int(bytes_removed)
        self.old_versions = int(old_versions)
        self.data_files_removed = int(data_files_removed)
        self.transaction_files_removed = int(transaction_files_removed)
        self.index_files_removed = int(index_files_removed)
        self.deletion_files_removed = int(deletion_files_removed)

    def __repr__(self) -> str:
        return "CleanupStats(" + ", ".join(f"{k}={v}" for k, v in vars(self).items()) + ")"


class CleanupCandidateFile:
    def __init__(self, path, kind, unverified, size_bytes):
        self.path, self.kind, self.unverified, self.size_bytes = str(path), str(kind), bool(unverified), int(size_bytes)

    def __repr__(self) -> str:
        return f"CleanupCandidateFile(path={self.path!r}, kind={self.kind!r}, size_bytes={self.size_bytes})"


class CleanupExplanation:
    """Mirrors ``lance.dataset.CleanupExplanation``."""

    def __init__(self, read_version, stats, candidate_files, candidate_files_truncated, candidate_file_limit):
        self.read_version = read_version
        self.stats = stats
        self.candidate_files = candidate_files
        self.candidate_files_truncated = candidate_files_truncated
        self.candidate_file_limit = candidate_file_limit
        self.referenced_branches = []
        self.warnings = []


@dataclasses.dataclass
class ColumnOrdering:
    """Mirrors ``lance.dataset.ColumnOrdering``: one sort key of ``order_by``."""

    column_name: str
    ascending: bool = True
    nulls_first: bool = False


def _total_order_key(column):
    """Floats as integers that sort in IEEE total order (Rust's total_cmp, which the sort in Lance
    uses: -NaN < -inf < ... < -0.0 < 0.0 < ... < inf < NaN), nulls kept; other columns unchanged.
    Arrow's own sort ranks NaN with the nulls and -0.0 equal to 0.0."""
    if isinstance(column, pa.ChunkedArray):
        column = column.combine_chunks()
    widths = {pa.float64(): (np.int64, 63), pa.float32(): (np.int32, 31), pa.float16(): (np.int16, 15)}
    if column.type not in widths:
        return column
    int_type, sign = widths[column.type]
    bits = np.frombuffer(column.buffers()[1], dtype=int_type, count=len(column) + column.offset)[column.offset:]
    unsigned = np.dtype(int_type).str.replace("i", "u")
    flip = ((bits >> sign).view(unsigned) >> 1).view(int_type)
    keys = bits ^ flip
    return pa.array(keys, mask=column.is_null().to_numpy(zero_copy_only=False) if column.null_count else None)


def _orderings(ds: "LanceDataset", order_by) -> List[ColumnOrdering]:
    """``order_by`` (column names or ColumnOrdering): each a top-level column, named exactly (Lance
    resolves sort columns by exact name)."""
    if not order_by:
        return []
    if isinstance(order_by, (str, ColumnOrdering)):
        order_by = [order_by]
    out = []
    for o in order_by:
        if isinstance(o, str):
            o = ColumnOrdering(o)
        elif not isinstance(o, ColumnOrdering) and hasattr(o, "column_name"):
            o = ColumnOrdering(o.column_name, bool(getattr(o, "ascending", True)), bool(getattr(o, "nulls_first", False)))
        elif not isinstance(o, ColumnOrdering):
            raise TypeError(f"order_by takes column names or ColumnOrdering, got {type(o).__name__}")
        if o.column_name not in ds._data_schema.names:
            if "." in o.column_name and _resolve_path(ds._data_schema, o.column_name) is not None:
                raise unsupported("order_by on a nested field")
            raise ValueError(f"Invalid user input: Column {o.column_name} not found")
        out.append(ColumnOrdering(o.column_name, bool(o.ascending), bool(o.nulls_first)))
    return out


def _blob_mode(blob_handling) -> int:
    """pylance's blob_handling names, as nanolance's reader modes. The default, as in pylance: a blob
    column comes back as its description."""
    if blob_handling is None:
        return _nanolance.BLOB_DESCRIPTIONS
    name = str(getattr(blob_handling, "value", blob_handling)).lower()
    if name in ("all_binary", "allbinary"):
        return _nanolance.BLOB_BINARY
    if name in ("blobs_descriptions", "all_descriptions", "blobsdescriptions", "alldescriptions"):
        return _nanolance.BLOB_DESCRIPTIONS
    raise ValueError(f"unknown blob_handling {blob_handling!r}")


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


_VERSION_COLUMNS = ("_row_created_at_version", "_row_last_updated_at_version")
_SYSTEM_COLUMNS = ("_rowid", "_rowaddr", "_rowoffset", "_distance") + _VERSION_COLUMNS
_ROW_ID_REFERENCE = None


def _references_row_ids(sql: Optional[str]) -> bool:
    """Whether a filter reads `_rowid` or `_rowaddr` (which the scan cannot push down)."""
    global _ROW_ID_REFERENCE
    if sql is None:
        return False
    if _ROW_ID_REFERENCE is None:
        import re

        _ROW_ID_REFERENCE = re.compile(r"(?<![A-Za-z0-9_])_row(id|addr)(?![A-Za-z0-9_])")
    return _ROW_ID_REFERENCE.search(sql) is not None


@contextlib.contextmanager
def _lance_errors():
    """Errors raised as pylance raises them: its Rust core's (k-means, PQ training, a name taken)
    as RuntimeError, its Python checks' as ValueError."""
    try:
        yield
    except RuntimeError as exc:
        text = str(exc)
        if text.startswith("dimension (") or "num_sub_vectors are required" in text:
            raise ValueError(text) from None
        if ("KMeans cannot train" in text or "Not enough rows to train PQ" in text or "already exists" in text
                or "num_bits" in text):
            raise RuntimeError(text) from None
        from nanolance.lance._errors import translate
        raise translate(text) from None


def _coerce_query_vector(query):
    """A query vector as a float array and its length, coerced as pylance's ``_coerce_query_vector``
    (Apache-2.0, the Lance authors) coerces it; several vectors become a list array."""
    if hasattr(query, "__getitem__") and not isinstance(query, (str, bytes)) and len(query) > 0 and isinstance(
            query[0], (list, tuple, np.ndarray, pa.Array)):
        dim = len(query[0])
        vectors = []
        for q in query:
            if len(q) != dim:
                raise ValueError(f"All query vectors must have the same length, but got {dim} and {len(q)}")
            vectors.append(_coerce_query_vector(q)[0])
        return pa.array(vectors, type=pa.list_(pa.float32())), dim
    if isinstance(query, pa.Scalar):
        if isinstance(query, pa.ExtensionScalar):
            query = query.value
        if isinstance(query.type, pa.FixedSizeListType):
            query = query.values
    elif isinstance(query, (list, tuple, np.ndarray)):
        query = pa.FloatingPointArray.from_pandas(np.array(query).astype("float64"), type=pa.float32())
    elif not isinstance(query, pa.Array):
        try:
            query = pa.array(query)
        except Exception:
            raise TypeError("Query vectors should be an array of floats, "
                            f"got {type(query)} which we cannot coerce to a float array") from None
    if not isinstance(query, pa.FloatingPointArray):
        if pa.types.is_integer(query.type):
            query = query.cast(pa.float32())
        else:
            raise TypeError(f"query vector must be list-like or pa.FloatingPointArray but received {query.type}")
    return query, len(query)


def _query_vector(q) -> np.ndarray:
    """A single query vector as float32."""
    array, _ = _coerce_query_vector(q)
    if pa.types.is_list(array.type):
        raise unsupported("batch and multivector queries (a 2-D q)")
    return np.asarray(array.to_numpy(zero_copy_only=False), np.float32).ravel()


def _fts_query_json(query) -> str:
    """``full_text_query=`` (a string, ``{"query": ..., "columns": [...]}``, or a lance.query object) as
    the query JSON nanolance's search reads."""
    import json

    if isinstance(query, str):
        if len(query) >= 2 and query[0] == '"' and query[-1] == '"':
            # pylance: a string in double quotes is an exact phrase on the indexed column.
            return json.dumps({"match_phrase": {"column": None, "terms": query[1:-1], "slop": 0}})
        return json.dumps(query)
    if isinstance(query, dict):
        if "query" in query:
            columns = query.get("columns")
            if isinstance(columns, str):
                columns = [columns]
            return json.dumps({"query": query["query"], "columns": columns})
        return json.dumps(query)
    inner = getattr(query, "inner", None)
    spec = getattr(inner, "spec", None)
    if spec is None:
        raise TypeError(f"full_text_query must be a string, a dict or a lance.query query, got {type(query)}")
    return json.dumps(spec)


def _nearest_query(ds, nearest) -> dict:
    """``nearest={...}`` checked as pylance checks it."""
    if not isinstance(nearest, dict):
        raise TypeError(f"nearest must be a dict, got {type(nearest)}")
    known = {"column", "q", "k", "metric", "distance_type", "nprobes", "minimum_nprobes", "maximum_nprobes",
             "refine_factor", "use_index", "ef", "query_parallelism", "approx_mode", "distance_range"}
    unknown = set(nearest) - known
    if unknown:
        raise TypeError(f"nearest() got unexpected keyword arguments {sorted(unknown)}")
    column = nearest.get("column")
    q = _query_vector(nearest.get("q"))
    node = None
    for i, part in enumerate(str(column).split(".")):
        if i > 0 and not pa.types.is_struct(node.type):
            node = None
            break
        node = {f.name.lower(): f for f in (ds._data_schema if i == 0 else node.type)}.get(part.lower())
        if node is None:
            break
    if node is None:
        raise ValueError(f"Embedding column {column} is not in the dataset")
    arrow_type = node.type
    storage = getattr(arrow_type, "storage_type", arrow_type)
    if pa.types.is_fixed_size_list(storage):
        dim = storage.list_size
    elif pa.types.is_list(storage) and pa.types.is_fixed_size_list(storage.value_type):
        raise unsupported("multivector search")
    else:
        raise TypeError(f"Query column {column} must be a vector. Got {arrow_type}.")
    if len(q) != dim:
        raise ValueError(f"Query vector size {len(q)} does not match index column size {dim}")
    k, nprobes = nearest.get("k"), nearest.get("nprobes")
    minimum, maximum = nearest.get("minimum_nprobes"), nearest.get("maximum_nprobes")
    refine = nearest.get("refine_factor")
    if k is not None and int(k) <= 0:
        raise ValueError(f"Nearest-K must be > 0 but got {k}")
    if nprobes is not None and int(nprobes) <= 0:
        raise ValueError(f"Nprobes must be > 0 but got {nprobes}")
    if minimum is not None and int(minimum) < 0:
        raise ValueError(f"Minimum nprobes must be >= 0 but got {minimum}")
    if maximum is not None and int(maximum) < 0:
        raise ValueError(f"Maximum nprobes must be >= 0 but got {maximum}")
    if nprobes is not None:
        if minimum is not None or maximum is not None:
            raise ValueError("nprobes cannot be set in combination with minimum_nprobes or maximum_nprobes")
        minimum = maximum = nprobes
    if minimum is not None and maximum is not None and int(minimum) > int(maximum):
        raise ValueError("minimum_nprobes must be <= maximum_nprobes")
    if refine is not None and int(refine) < 1:
        raise ValueError(f"Refine factor must be 1 or more got {refine}")
    ef = nearest.get("ef")
    if ef is not None and int(ef) <= 0:
        raise ValueError(f"ef must be > 0 but got {ef}")
    lower = upper = None
    distance_range = nearest.get("distance_range")
    if distance_range is not None:
        if len(distance_range) != 2:
            raise ValueError("distance_range must be a tuple of (lower_bound, upper_bound)")
        lower, upper = distance_range
    metric = nearest.get("metric") or nearest.get("distance_type")
    return {
        "column": str(column), "q": q.tolist(), "k": 10 if k is None else int(k),
        "minimum_nprobes": 1 if minimum is None else int(minimum),
        "maximum_nprobes": None if maximum is None else int(maximum),
        "refine_factor": None if refine is None else int(refine),
        "metric": None if metric is None else str(metric),
        "use_index": nearest.get("use_index", True) is not False,
        "lower_bound": None if lower is None else float(lower),
        "upper_bound": None if upper is None else float(upper),
        "ef": None if ef is None else int(ef),
    }


def _filter_sql(filter) -> Optional[str]:
    """A filter as SQL: a string as given, a pyarrow compute expression translated."""
    if filter is None:
        return None
    if isinstance(filter, str):
        return filter if filter.strip() else None
    try:
        import pyarrow.compute as pc

        if isinstance(filter, pc.Expression):
            return _expression_sql(filter)
    except ImportError:  # pragma: no cover
        pass
    raise unsupported(f"filters of type {type(filter).__name__} (use SQL or a pyarrow.compute.Expression)")


def _expression_sql(expr) -> str:
    """pyarrow.compute.Expression -> SQL, from its string form: (a > 1), ((a == "x") and is_null(b)),
    is_in(a, {value_set=int64:[1, 2], ...}), invert(...)."""
    import re

    text = str(expr)

    def value_set(m):
        body = m.group(2)
        items = body.split(":", 1)[1] if ":" in body.split("[", 1)[0] else body
        items = items.strip().lstrip("[").rstrip("]")
        return f"({m.group(1)} IN ({items}))"

    text = re.sub(r'is_in\(([^,]+), \{value_set=([^}]*?\])[^}]*\}\)', value_set, text)
    text = re.sub(r", \{[^}]*\}\)", ")", text)  # function options: is_null(a, {nan_is_null=false})

    # pyarrow prints temporal scalars bare: a timestamp as "2021-01-01 02:00:00.000[Z]" (a zoned one
    # in UTC, how the column holds it too), a date as "2021-01-01". Outside string literals, they
    # become SQL's TIMESTAMP and DATE literals.
    def temporal(segment):
        segment = re.sub(r"(?<![\w'])(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)Z?(?![\w'])",
                         lambda m: f"TIMESTAMP '{m.group(1)}'", segment)
        return re.sub(r"(?<![\w'-])(\d{4}-\d{2}-\d{2})(?![\w:'-]| \d)", lambda m: f"DATE '{m.group(1)}'", segment)

    parts = re.split(r'("(?:[^"\\]|\\.)*")', text)
    text = "".join(part if i % 2 else temporal(part) for i, part in enumerate(parts))
    # pyarrow quotes strings with double quotes; SQL uses single.
    text = re.sub(r'"((?:[^"\\]|\\.)*)"', lambda m: "'" + m.group(1).replace("'", "''") + "'", text)
    return text


class MergeInsertBuilder:
    """Mirrors ``lance.dataset.MergeInsertBuilder``."""

    def __init__(self, dataset: LanceDataset, on):
        if on is None:
            # The schema's (unenforced) primary key, as Lance does it.
            on = [f.name for f in dataset._data_schema
                  if (f.metadata or {}).get(b"lance-schema:unenforced-primary-key", b"").lower() == b"true"]
            if not on:
                raise ValueError(
                    "Invalid user input: A merge insert operation requires join keys: specify `on` columns "
                    "explicitly or configure a primary key in the dataset schema"
                )
        self._ds = dataset
        self._on = [on] if isinstance(on, str) else list(on)
        self._update_all = False
        self._insert_all = False
        self._delete_by_source = False
        self._delete_condition = ""
        self._update_condition = ""
        self._when_matched = ""  # "fail" / "delete"; otherwise by _update_all
        self._write_mode = "auto"

    def when_matched_update_all(self, condition: Optional[str] = None) -> "MergeInsertBuilder":
        """Update matched rows; with `condition` (SQL over `source.<col>` and `target.<col>`), only
        those for which it is TRUE."""
        self._update_all = True
        self._update_condition = condition or ""
        self._when_matched = ""
        return self

    def when_matched_fail(self) -> "MergeInsertBuilder":
        """Fail the whole merge if a source row matches a row of the dataset."""
        self._when_matched = "fail"
        self._update_all = False
        return self

    def when_matched_delete(self) -> "MergeInsertBuilder":
        """Delete the dataset's rows the source matches (the source needs only the keys)."""
        self._when_matched = "delete"
        self._update_all = False
        return self

    def write_mode(self, mode: str) -> "MergeInsertBuilder":
        """How the merged rows are written. nanolance gives every mode the same results, written as
        whole rows ('rewrite_rows'); 'rewrite_columns' is checked as Lance checks it."""
        if mode not in ("auto", "rewrite_rows", "rewrite_columns"):
            raise ValueError(f"Invalid write_mode: {mode}. Expected one of 'auto', 'rewrite_rows', 'rewrite_columns'")
        self._write_mode = mode
        return self

    def when_not_matched_insert_all(self) -> "MergeInsertBuilder":
        self._insert_all = True
        return self

    def when_not_matched_by_source_delete(self, expr: Optional[str] = None) -> "MergeInsertBuilder":
        self._delete_by_source = True
        self._delete_condition = "" if expr is None else _filter_sql(expr) or ""
        return self

    def conflict_retries(self, max_retries: int) -> "MergeInsertBuilder":
        return self

    def retry_timeout(self, timeout) -> "MergeInsertBuilder":
        return self

    def use_index(self, use_index: bool) -> "MergeInsertBuilder":
        return self

    def execute(self, data_obj, *, schema: Optional[pa.Schema] = None) -> Dict[str, int]:
        reader = _coerce_reader(data_obj, schema)
        target = self._ds._data_schema
        if not (self._update_all or self._insert_all or self._delete_by_source or self._when_matched):
            raise ValueError("Invalid user input: The merge insert job is not configured to change the data in any way")
        if self._when_matched == "delete" and not self._insert_all:
            # Only the keys matter: the source may hold just those.
            unexpected = [n for n in reader.schema.names if n not in target.names]
            if unexpected:
                _check_append_schema(target, reader.schema, allow_subset=True)
            keys = reader.read_all().select(self._on)
            with native():
                stats, _ = _nanolance._ds_merge_insert(
                    self._ds.uri, self._on, False, False, self._delete_by_source, self._delete_condition,
                    self._fill_missing_columns(pa.RecordBatchReader.from_batches(keys.schema, keys.to_batches()),
                                               target, read_old=False),
                    "", "delete")
            self._ds._refresh_latest()
            return {k: int(v) for k, v in stats.items()}
        _check_append_schema(target, reader.schema, allow_subset=True)
        if self._write_mode == "rewrite_columns":
            if len(reader.schema.names) >= len(target.names) or self._insert_all:
                raise OSError(
                    "Invalid user input: MergeInsertWriteMode::RewriteColumns cannot express this merge insert: "
                    + ("the source covers every dataset column, so there is nothing to skip; "
                       if len(reader.schema.names) >= len(target.names) else "")
                    + ("inserting unmatched source rows adds rows, which patching cannot do. " if self._insert_all
                       else "")
                    + "Use MergeInsertWriteMode::Auto or RewriteRows instead."
                )
        if len(reader.schema.names) < len(target.names):
            reader = self._fill_missing_columns(reader, target)
        conformed = pa.RecordBatchReader.from_batches(target, (_conform(b, target) for b in reader))
        with native():
            stats, _ = _nanolance._ds_merge_insert(self._ds.uri, self._on, self._update_all, self._insert_all,
                                                   self._delete_by_source, self._delete_condition, conformed,
                                                   self._update_condition, self._when_matched)
        self._ds._refresh_latest()
        return {k: int(v) for k, v in stats.items()}


    def _fill_missing_columns(self, reader: pa.RecordBatchReader, target: pa.Schema,
                              read_old: bool = True) -> pa.RecordBatchReader:
        """A source with part of the dataset's columns, made whole: a matched row keeps the values
        it has in the columns the source lacks, and an inserted row gets nulls there (as Lance's
        merge of part of the schema does)."""
        source = reader.read_all()
        for k in self._on:
            if k not in source.column_names:
                raise ValueError(f"the source has no key column '{k}'")
        missing = [f.name for f in target if f.name not in source.column_names]
        # Each source row's matching dataset row (its position in a scan), by the keys. Null keys
        # match nothing.
        keys = self._ds.to_table(columns=self._on)
        left = pa.table({**{f"k{i}": source.column(k) for i, k in enumerate(self._on)},
                         "__source": pa.array(range(source.num_rows), pa.int64())})
        right = pa.table({**{f"k{i}": keys.column(k) for i, k in enumerate(self._on)},
                          "__target": pa.array(range(keys.num_rows), pa.int64())})
        names = [f"k{i}" for i in range(len(self._on))]
        joined = left.join(right, keys=names, join_type="left outer").sort_by("__source")
        if joined.num_rows != source.num_rows:
            raise ValueError("merge insert: a source row matches more than one row of the dataset")
        at = joined.column("__target")
        matched = at.drop_null()
        if len(matched) and read_old:
            old = self._ds.take(matched.to_pylist(), columns=missing)
        else:
            old = pa.table({n: pa.array([], target.field(n).type) for n in missing})
        # Row i of `old` belongs to the i-th matched source row; the others take null.
        import pyarrow.compute as pc
        valid = pc.is_valid(at)
        position = pc.subtract(pc.cumulative_sum(pc.cast(valid, pa.int64())), 1)
        pick = pc.if_else(valid, position, pa.scalar(None, pa.int64()))
        inserted = (self._insert_all and len(matched) < source.num_rows)
        if inserted:
            for n in missing:
                if not target.field(n).nullable:
                    raise OSError(
                        "Append with different schema: fields did not match, "
                        f"missing=[{n}], unexpected=[]"
                    )
        columns = {}
        for f in target:
            if f.name in source.column_names:
                columns[f.name] = source.column(f.name)
            elif read_old:
                columns[f.name] = old.column(f.name).take(pick)
            else:
                columns[f.name] = pa.nulls(source.num_rows, f.type)
        whole = pa.table(columns)
        return pa.RecordBatchReader.from_batches(whole.schema, whole.to_batches())


class DatasetOptimizer:
    """Mirrors ``lance.dataset.DatasetOptimizer``: compaction."""

    def __init__(self, dataset: LanceDataset):
        self._ds = dataset

    def compact_files(self, *, target_rows_per_fragment: Optional[int] = None, max_rows_per_group=None,
                      max_bytes_per_file=None, materialize_deletions: Optional[bool] = None,
                      materialize_deletions_threshold: Optional[float] = None, num_threads=None, batch_size=None,
                      reindex: bool = True, max_source_fragments: Optional[int] = None,
                      max_source_rows: Optional[int] = None, max_source_bytes: Optional[int] = None,
                      excluded_fragment_ids=None, **kwargs):
        """Rewrite small fragments (and those with many deleted rows) into fewer, larger ones. The
        indexes that covered the rewritten rows take them back, as Lance's compaction remaps its
        indexes (a second version; nanolance's `reindex=False` leaves them covering fewer fragments)."""
        from nanolance.lance.optimize import CompactionMetrics

        with native():
            metrics, _ = _nanolance._ds_compact_files(
                self._ds.uri, int(target_rows_per_fragment or 1024 * 1024),
                True if materialize_deletions is None else bool(materialize_deletions),
                0.1 if materialize_deletions_threshold is None else float(materialize_deletions_threshold),
                bool(reindex), *(None if v is None else int(v)
                                 for v in (max_source_fragments, max_source_rows, max_source_bytes)),
                sorted({int(i) for i in (excluded_fragment_ids or [])}), int(max_bytes_per_file or 0),
                int(batch_size or 0))
        self._ds._refresh_latest()
        return CompactionMetrics(**{k: int(v) for k, v in metrics.items() if not k.startswith("indexes_")})

    def enable_auto_cleanup(self, auto_cleanup_config, **kwargs) -> None:
        """Clean up old versions after commits, as ``lance.auto_cleanup.*`` in the config asks: every
        ``interval`` versions, the versions older than ``older_than_seconds``."""
        self._ds.update_config({
            "lance.auto_cleanup.interval": str(auto_cleanup_config["interval"]),
            "lance.auto_cleanup.older_than": f"{auto_cleanup_config['older_than_seconds']}s",
        })

    def disable_auto_cleanup(self, **kwargs) -> None:
        self._ds.delete_config_keys(["lance.auto_cleanup.interval", "lance.auto_cleanup.older_than"])

    def optimize_indices(self, *, num_indices_to_merge: Optional[int] = None, index_names=None, retrain: bool = False,
                         **kwargs):
        """Fold rows the indexes do not cover yet into them, as pylance's optimize_indices does: the last
        `num_indices_to_merge` (default 1) segments of each index are replaced by one covering their
        fragments and every uncovered one (0: a new segment over the uncovered fragments alone), with
        the index's parameters -- a vector index keeps its trained model unless `retrain`."""
        unknown = sorted(set(kwargs) - {"num_threads"})
        if unknown:
            raise unsupported(f"optimize_indices options {unknown}")
        names = [index_names] if isinstance(index_names, str) else list(index_names or [])
        with native():
            _nanolance._ds_optimize_indices(self._ds.uri, names,
                                            None if num_indices_to_merge is None else int(num_indices_to_merge),
                                            bool(retrain))
        self._ds._refresh_latest()


class IndexSegmentDescription:
    """One segment of an index. Mirrors ``lance.indices.IndexSegmentDescription``."""

    def __init__(self, **fields):
        self.__dict__.update(fields)

    def __repr__(self) -> str:
        return f"IndexSegmentDescription(uuid={self.uuid!r}, fragment_ids={sorted(self.fragment_ids)})"


@dataclasses.dataclass
class FieldStatistics:
    """Statistics about a field in the dataset"""

    id: int  #: id of the field
    bytes_on_disk: int  #: (possibly compressed) bytes on disk used to store the field


@dataclasses.dataclass
class DataStatistics:
    """Statistics about the data in the dataset"""

    fields: List[FieldStatistics]  #: Statistics about the fields in the dataset


class DatasetStats(TypedDict):
    num_deleted_rows: int
    num_fragments: int
    num_small_files: int


class LanceStats:
    """Statistics about a LanceDataset."""

    def __init__(self, dataset: "LanceDataset"):
        self._ds = dataset

    def dataset_stats(self, max_rows_per_group: int = 1024) -> DatasetStats:
        fragments = self._ds._info["fragments"]
        return {
            "num_deleted_rows": sum(f["deleted_rows"] for f in fragments),
            "num_fragments": len(fragments),
            "num_small_files": sum(1 for f in fragments if f["physical_rows"] < max_rows_per_group),
        }

    def index_stats(self, index_name: str) -> Dict[str, Any]:
        return _index_statistics(self._ds, index_name)

    def data_stats(self) -> DataStatistics:
        with native():
            stats = _nanolance._ds_data_stats(self._ds._uri, self._ds._version)
        return DataStatistics(fields=[FieldStatistics(id=i, bytes_on_disk=b) for i, b in stats])


def _index_statistics(ds: "LanceDataset", index_name: str) -> Dict[str, Any]:
    """Lance's index_statistics: per-segment details from the index files, coverage from the manifest."""
    with native():
        segments = [i for i in _nanolance._ds_list_indices(ds._uri, ds._version) if i["name"] == index_name]
    if not segments:
        raise KeyError(f'Index "{index_name}" not found')
    details = [_segment_statistics(ds, s) for s in segments]
    kind = details[0].get("index_type") if details and isinstance(details[0].get("index_type"), str) else None
    if kind is None:
        kind = segments[0]["type"] if segments[0]["type_url"] else "N/A"
    live = {f["id"]: f["physical_rows"] - f["deleted_rows"] for f in ds._info["fragments"]}
    per_delta = [sum(live[i] for i in s["fragment_ids"] if i in live) for s in segments]
    covered = [i for s in segments for i in s["fragment_ids"] if i in live]
    indexed_rows = sum(per_delta)
    created = [s["created_at"] for s in segments if s["created_at"]]
    return {
        "index_type": kind,
        "name": index_name,
        "num_indices": len(segments),
        "num_segments": len(segments),
        "indices": details,
        "segments": [dict(d) for d in details],
        "num_indexed_fragments": len(set(covered)),
        "num_indexed_rows": indexed_rows,
        "num_unindexed_fragments": len(live) - len(set(covered)),
        "num_unindexed_rows": sum(live.values()) - indexed_rows,
        "num_indexed_rows_per_delta": per_delta,
        "updated_at_timestamp_ms": max(created) if created else None,
    }


def _scalar_display(value) -> Optional[str]:
    """A value as DataFusion's ScalarValue displays it (what a BTree's statistics print)."""
    if value is None:
        return None
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (float, np.floating)):
        if np.isnan(value):
            return "NaN"
        if np.isinf(value):
            return "inf" if value > 0 else "-inf"
        return np.format_float_positional(value, trim="-")
    if isinstance(value, bytes):
        return value.hex()
    return str(value)


def _segment_statistics(ds: "LanceDataset", segment: dict) -> Dict[str, Any]:
    """One index segment's own statistics, as Lance's index of that type reports them."""
    import json

    directory = os.path.join(ds._uri, "_indices", segment["uuid"])
    kind = segment["type"]

    def metadata(name: str) -> Dict[bytes, bytes]:
        with native():
            return _nanolance._file_info(os.path.join(directory, name))[4]

    def rows(name: str) -> int:
        with native():
            return int(_nanolance._file_info(os.path.join(directory, name))[0])

    try:
        if kind in ("IVF_FLAT", "IVF_PQ", "IVF_HNSW_SQ", "IVF_SQ", "IVF_HNSW_PQ", "IVF_HNSW_FLAT"):
            return _vector_statistics(ds, segment, directory)
        if kind == "BTree":
            table = pa.table(_nanolance._file_read(os.path.join(directory, "page_lookup.lance"), None, 0, -1))
            mins, maxs = table.column(0).to_pylist(), table.column(1).to_pylist()
            start = next((k for k, v in enumerate(mins) if v is not None), len(mins))
            if pa.types.is_floating(table.schema.field(0).type):
                np_type = table.schema.field(0).type.to_pandas_dtype()
                mins = [None if v is None else np_type(v) for v in mins]
                maxs = [None if v is None else np_type(v) for v in maxs]
            return {"min": _scalar_display(mins[start]) if start < len(mins) else None,
                    "max": _scalar_display(maxs[-1]) if start < len(mins) else None,
                    "num_pages": len(mins)}
        if kind in ("Bitmap", "LabelList"):
            stats = metadata("bitmap_page_lookup.lance").get(b"lance:index_stats")
            if stats is not None:
                return json.loads(stats)
            return {"num_bitmaps": rows("bitmap_page_lookup.lance")}
        if kind == "Inverted":
            meta = metadata("metadata.lance")
            params = json.loads(meta.get(b"params", b"{}"))
            parts = json.loads(meta.get(b"partitions", b"[]"))
            return {"params": params,
                    "num_tokens": sum(rows(f"part_{p}_invert.lance") for p in parts),
                    "num_docs": sum(rows(f"part_{p}_docs.lance") for p in parts)}
        if kind == "NGram":
            return {"num_ngrams": rows("ngram_postings.lance")}
    except (OSError, ValueError, KeyError):
        pass
    return {}


def _vector_statistics(ds: "LanceDataset", segment: dict, directory: str) -> Dict[str, Any]:
    import json

    with native():
        model = _nanolance._vector_index_model(directory)
    index_meta = json.loads(model["index_metadata"] or "{}")
    kind = index_meta.get("type", segment["type"])
    metric = index_meta.get("distance_type", "l2")
    sub: Dict[str, Any] = {}
    if model["sub_index_metadata"]:
        for item in json.loads(model["sub_index_metadata"]):
            if item:
                sub.update(json.loads(item))
                break
    for item in json.loads(model["storage_metadata"] or "[]"):
        sub.update(json.loads(item) if isinstance(item, str) else item)
        break
    sub_kind = kind.split("_")[-1]
    sub["index_type"] = "HNSW" if "HNSW" in kind else ("FLAT" if sub_kind == "FLAT" else sub_kind)
    sub["metric_type"] = "l2" if sub_kind == "PQ" and metric == "cosine" else metric
    for key in ("codebook_position", "codebook", "codebook_tensor"):
        sub.pop(key, None)
    include = os.environ.get("LANCE_INCLUDE_VECTOR_CENTROIDS", "true").strip().lower()
    return {
        "index_type": kind,
        "uuid": segment["uuid"],
        "uri": os.path.join(directory, "index.idx"),
        "metric_type": metric,
        "num_partitions": len(model["partition_sizes"]),
        "sub_index": sub,
        "partitions": [{"size": int(n)} for n in model["partition_sizes"]],
        "centroids": None if include in ("0", "false", "no", "off") else model["centroids"],
        "loss": model["loss"],
        "index_file_version": "V3",
    }


class IndexDescription:
    """An index of a dataset, its segments together. Mirrors ``lance.indices.IndexDescription``."""

    def __init__(self, **fields):
        self.__dict__.update(fields)

    def __repr__(self) -> str:
        return (f"IndexDescription(name={self.name!r}, type_url={self.type_url!r}, "
                f"num_rows_indexed={self.num_rows_indexed}, fields={self.fields}, "
                f"field_names={self.field_names}, num_segments={len(self.segments)}, "
                f"total_size_bytes={self.total_size_bytes})")


class LanceScanner:
    """A configured read. Mirrors ``lance.LanceScanner``."""

    def __init__(self, ds: LanceDataset, columns=None, filter=None, limit=None, offset=None, nearest=None,
                 batch_size=None, fragments=None, full_text_query=None, with_row_id=False, with_row_address=False,
                 include_deleted_rows=None, order_by=None, substrait_filter=None, scan_stats_callback=None,
                 blob_handling=None, use_scalar_index=None, prefilter=None, fast_search=None,
                 disable_scoring_autoprojection=None, batch_size_bytes=None, **ignored):
        # order_by: the scan without limit and offset (and with the sort columns), sorted, then sliced.
        self._order_by = _orderings(ds, order_by)
        if self._order_by:
            if nearest is not None or full_text_query is not None:
                raise unsupported("order_by with a vector or full-text search")
            if isinstance(columns, dict):
                raise unsupported("order_by with a {alias: column} projection")
            self._unsorted = dict(
                columns=columns, filter=filter, fragments=fragments, with_row_id=with_row_id,
                with_row_address=with_row_address, blob_handling=blob_handling, use_scalar_index=use_scalar_index,
                substrait_filter=substrait_filter, include_deleted_rows=include_deleted_rows)
        self._batch_size_bytes = None if batch_size_bytes is None else int(batch_size_bytes)
        # With a projection, `_distance` / `_score` come back only where named (Lance's
        # disable_scoring_autoprojection); otherwise after the named columns.
        self._no_score_autoprojection = bool(disable_scoring_autoprojection) and columns is not None
        self._nearest = None if nearest is None else _nearest_query(ds, nearest)
        self._prefilter = bool(prefilter)
        self._fast_search = bool(fast_search)
        self._fts = None if full_text_query is None else _fts_query_json(full_text_query)
        if self._fts is not None and self._nearest is not None:
            raise unsupported("hybrid (vector and full-text) search")
        if substrait_filter is not None:
            raise unsupported("substrait filters")
        if include_deleted_rows:
            raise unsupported("include_deleted_rows")
        self._filter = _filter_sql(filter)
        # A filter on _rowid / _rowaddr is evaluated here, over the rows the scan returns with their
        # addresses, before limit and offset (the scan cannot push it down).
        self._post_filter = None
        if _references_row_ids(self._filter):
            if self._nearest is not None or self._fts is not None:
                raise unsupported("a filter on _rowid or _rowaddr with a vector or full-text search")
            self._post_filter, self._filter = self._filter, None
        self._blob_handling = _blob_mode(blob_handling)
        if offset is not None and int(offset) < 0:
            raise ValueError("Offset must be non-negative")
        if limit is not None and int(limit) < 0:
            raise ValueError("Limit must be non-negative")
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
            self._row_versions = [n for n in self._names if n in _VERSION_COLUMNS]
            self._names = [n for n in self._names if n not in _SYSTEM_COLUMNS]
            if not self._names and not (with_row_id or with_row_address):
                with_row_address = True  # something to count rows by; not returned
        if self._fts is not None and self._names is not None and "_score" in self._names:
            # The score, named in the projection, comes back where it was named.
            if self._order is None:
                self._order = list(self._names)
            self._names = [n for n in self._names if n != "_score"]
        # A nested field (`s.x`, `` `meta-data`.`id` ``) or a differently cased name: its top-level
        # column is read, and the field comes back under the name as given.
        self._nested = {}
        if self._names is not None:
            requested = list(self._order if self._order is not None else self._names)
            schema = ds._data_schema
            read = []
            for n in self._names:
                path = None if n in schema.names else _resolve_path(schema, n)
                if path is not None:
                    self._nested[n] = path
                    n = path[0]
                if n not in read:
                    read.append(n)
            if self._nested:
                self._names = read
                if self._order is None:
                    self._order = requested
        self._limit = None if limit is None else int(limit)
        self._offset = 0 if offset is None else int(offset)
        self._batch_size = batch_size
        self._fragment_ids = None if fragments is None else [int(getattr(f, "fragment_id", f)) for f in fragments]
        self._with_row_id = bool(with_row_id)
        self._with_row_address = bool(with_row_address)
        self._use_scalar_index = use_scalar_index is not False

    def _read(self, stream: bool):
        if self._filter is None:
            n = self._ds.count_rows() if self._fragment_ids is None else sum(
                self._ds.get_fragment(i).count_rows() for i in self._fragment_ids
            )
            offset = min(self._offset, n)
        else:
            offset = self._offset
        length = -1 if self._limit is None else self._limit
        if self._names is not None and not self._names and not (self._with_row_id or self._with_row_address):
            self._drop_rowaddr = True
            with native():
                return _nanolance._ds_scan(self._ds.uri, self._ds.version, [], self._fragment_ids, offset, length,
                                           False, True, stream, self._filter, self._blob_handling,
                                           self._use_scalar_index)
        with native():
            return _nanolance._ds_scan(self._ds.uri, self._ds.version, self._names, self._fragment_ids, offset,
                                       length, self._with_row_id, self._with_row_address, stream, self._filter,
                                       self._blob_handling, self._use_scalar_index)

    _drop_rowaddr = False
    _row_versions: List[str] = []

    def _shape(self, table: pa.Table) -> pa.Table:
        if self._drop_rowaddr:
            # No columns asked for: the rows, without any column.
            return pa.table({"_": pa.nulls(table.num_rows)}).drop_columns(["_"])
        if self._row_offset:
            first = self._offset
            if self._fragment_ids is not None:
                starts, at = {}, 0
                for f in self._ds._info["fragments"]:
                    starts[f["id"]] = at
                    at += int(f["physical_rows"]) - int(f["deleted_rows"])
                first += starts[self._fragment_ids[0]] if len(self._fragment_ids) == 1 else 0
            table = table.append_column("_rowoffset", pa.array(range(first, first + table.num_rows), pa.uint64()))
        for column in self._row_versions:
            table = table.append_column(column, self._ds._row_versions(table.num_rows))
        for name, path in self._nested.items():
            import pyarrow.compute as pc

            column = table.column(path[0])
            for part in path[1:]:
                column = pc.struct_field(column, part)
            table = table.append_column(name, column)
        if self._order is not None and self._nested:
            extra = [c for c in ("_rowid", "_rowaddr") if c in table.column_names and c not in self._order]
            return table.select(self._order + extra)
        if self._order is not None:
            return table.select(self._order)
        if self._names is not None:
            table = table.select(self._names + [c for c in ("_rowid", "_rowaddr") if c in table.column_names])
        table = _rename(table, self._columns)
        meta = self._ds._info["schema_metadata"]
        if meta and table.schema.metadata != meta:
            table = table.replace_schema_metadata(meta)
        return table

    def _sorted_table(self) -> pa.Table:
        """Every row the filter passes, sorted by the orderings (each key with its own direction and
        null placement, as DataFusion's sort in Lance), then offset and limit."""
        import pyarrow.compute as pc

        args = dict(self._unsorted)
        names = None if args["columns"] is None else _normalize_columns(args["columns"])
        extra = []
        if names is not None:
            extra = [o.column_name for o in self._order_by if o.column_name not in names]
            args["columns"] = names + [c for c in dict.fromkeys(extra)]
        table = LanceScanner(self._ds, **args).to_table()
        order = pa.array(range(table.num_rows), pa.int64())
        for o in reversed(self._order_by):  # stable passes, least significant key first
            column = _total_order_key(table.column(o.column_name).take(order))
            keys = pc.array_sort_indices(column, order="ascending" if o.ascending else "descending",
                                         null_placement="at_start" if o.nulls_first else "at_end")
            order = order.take(keys)
        table = table.take(order)
        if self._offset or self._limit is not None:
            table = table.slice(self._offset, self._limit)
        return table.drop_columns(list(dict.fromkeys(extra))) if extra else table

    def to_table(self) -> pa.Table:
        return _json_out(self._to_table())

    def _to_table(self) -> pa.Table:
        if self._order_by:
            return self._sorted_table()
        if self._nearest is not None:
            return self._nearest_table()
        if self._fts is not None:
            return self._fts_table()
        if self._post_filter is not None:
            return self._shape(self._row_id_filtered())
        return self._shape(pa.table(self._read(stream=False)))

    def _row_id_filtered(self) -> pa.Table:
        """The scan with a filter on _rowid / _rowaddr: every row with its address (and the columns
        the filter reads), the filter evaluated by the scan's own expression engine, then offset and
        limit."""
        import re

        sql = self._post_filter
        if self._ds.has_stable_row_ids:
            raise unsupported("a filter on _rowid or _rowaddr over a dataset with stable row ids")
        names = self._names
        extra = []
        if names is not None:
            extra = [n for n in self._ds._data_schema.names
                     if n not in names and re.search(r"(?<![A-Za-z0-9_])" + re.escape(n) + r"(?![A-Za-z0-9_])", sql)]
            if not names and not (self._with_row_id or self._with_row_address):
                self._drop_rowaddr = True
        with native():
            table = pa.table(_nanolance._ds_scan(self._ds.uri, self._ds.version,
                                                 None if names is None else names + extra, self._fragment_ids, 0, -1,
                                                 False, True, False, None, self._blob_handling,
                                                 self._use_scalar_index))
        address = table.column("_rowaddr")
        probe = table.append_column("_rowid", address)  # without stable row ids, a row's id is its address
        masks = []
        for batch in probe.combine_chunks().to_batches():
            with native():
                masks.append(np.frombuffer(_nanolance._filter_mask(batch, sql), dtype=np.uint8).astype(bool))
        mask = np.concatenate(masks) if masks else np.zeros(0, dtype=bool)
        table = table.filter(pa.array(mask, pa.bool_()))
        if self._offset or self._limit is not None:
            table = table.slice(self._offset, self._limit)
        if self._with_row_id:
            table = table.append_column("_rowid", table.column("_rowaddr"))
        drop = extra + ([] if self._with_row_address else ["_rowaddr"])
        return table.drop_columns([c for c in drop if c in table.column_names])

    def _search(self):
        n = self._nearest
        with native():
            return _nanolance._ds_nearest(
                self._ds.uri, self._ds.version, n["column"], n["q"], n["k"], n["minimum_nprobes"],
                n["maximum_nprobes"], n["refine_factor"], n["metric"], n["use_index"], n["lower_bound"],
                n["upper_bound"], self._filter, self._prefilter, self._fast_search, n.get("ef"))

    def _nearest_table(self) -> pa.Table:
        """The k nearest rows: the columns asked for, then ``_distance`` (then the row id columns)."""
        ids, distances, _ = self._search()
        names = self._names
        if self._order is not None and "_distance" in self._order:
            names = [c for c in names if c != "_distance"] if names is not None else None
        table = self._ds._take(list(ids), names, addresses=True, with_row_id=self._with_row_id,
                               with_row_address=self._with_row_address, blob_handling=self._blob_handling)
        system = [c for c in ("_rowid", "_rowaddr") if c in table.column_names]
        data = table.drop_columns(system)
        out = data.append_column("_distance", pa.array(distances, pa.float32()))
        for c in system:
            out = out.append_column(c, table.column(c))
        if self._offset or self._limit is not None:
            out = out.slice(self._offset, self._limit)
        if self._order is not None:
            # Named columns where named; `_distance`, when not named, after them (as Lance).
            order = [c for c in self._order if c in out.column_names]
            if "_distance" not in order and not self._no_score_autoprojection:
                order.append("_distance")
            order += [c for c in system if c not in order]
            out = out.select(order)
        elif self._no_score_autoprojection:
            out = out.drop_columns(["_distance"])
        meta = self._ds._info["schema_metadata"]
        if meta and out.schema.metadata != meta:
            out = out.replace_schema_metadata(meta)
        return out

    def _fts_search(self):
        limit = None if self._limit is None else self._offset + self._limit
        with native():
            return _nanolance._ds_full_text_search(self._ds.uri, self._ds.version, self._fts, limit, self._filter,
                                                   self._prefilter, self._fast_search)

    def _fts_table(self) -> pa.Table:
        """The matching rows, best first: the columns asked for, then ``_score`` (then the row id columns)."""
        ids, scores, _ = self._fts_search()
        names = self._names
        if self._order is not None and "_score" in self._order:
            names = [c for c in names if c != "_score"] if names is not None else None
        table = self._ds._take(list(ids), names, addresses=True, with_row_id=self._with_row_id,
                               with_row_address=self._with_row_address, blob_handling=self._blob_handling)
        system = [c for c in ("_rowid", "_rowaddr") if c in table.column_names]
        data = table.drop_columns(system)
        out = data.append_column("_score", pa.array(scores, pa.float32()))
        for c in system:
            out = out.append_column(c, table.column(c))
        if self._offset:
            out = out.slice(self._offset)
        if self._order is not None:
            order = [c for c in self._order if c in out.column_names]
            if "_score" not in order and not self._no_score_autoprojection:
                order.append("_score")
            order += [c for c in system if c not in order]
            out = out.select(order)
        elif self._no_score_autoprojection:
            out = out.drop_columns(["_score"])
        meta = self._ds._info["schema_metadata"]
        if meta and out.schema.metadata != meta:
            out = out.replace_schema_metadata(meta)
        return out

    def to_reader(self) -> pa.RecordBatchReader:
        table = self.to_table()
        return pa.RecordBatchReader.from_batches(table.schema, self._rebatch(table))

    def _rebatch(self, table: pa.Table) -> Iterator[pa.RecordBatch]:
        size = self._batch_size or os.environ.get("LANCE_DEFAULT_BATCH_SIZE")
        if not size and self._batch_size_bytes and table.num_rows:
            # About that many bytes a batch, by the rows' average in-memory size.
            size = max(1, self._batch_size_bytes * table.num_rows // max(table.nbytes, 1))
        if size:
            return iter(table.to_batches(max_chunksize=int(size)))
        return iter(table.to_batches())

    def to_batches(self) -> Iterator[pa.RecordBatch]:
        return self._rebatch(self.to_table())

    def count_rows(self) -> int:
        if self._fts is not None:
            return len(self._fts_search()[0][self._offset:])
        if self._names is None and not (self._with_row_id or self._with_row_address):
            # Count by reading only what the filter needs (a row address column when it needs nothing).
            counter = LanceScanner(self._ds, columns=[], filter=self._post_filter or self._filter, limit=self._limit,
                                   offset=self._offset,
                                   fragments=self._fragment_ids, with_row_address=True,
                                   use_scalar_index=self._use_scalar_index)
            return counter.to_table().num_rows
        return self.to_table().num_rows

    @property
    def projected_schema(self) -> pa.Schema:
        return self.to_table().schema if self._names is not None else self._ds.schema

    @property
    def dataset_schema(self) -> pa.Schema:
        return self._ds.schema

    def explain_plan(self, verbose: bool = False) -> str:
        lines = [f"nanolance scan of {self._ds.uri} v{self._ds.version}"]
        if self._nearest is not None:
            lines += ["  " + line for line in self._search()[2]]
            return "\n".join(lines)
        if self._fts is not None:
            lines += ["  " + line for line in self._fts_search()[2]]
            return "\n".join(lines)
        if self._filter is not None:
            lines.append(f"  filter={self._filter}")
            if self._use_scalar_index:
                with native():
                    lines += ["  " + line for line in _nanolance._ds_explain_filter(
                        self._ds.uri, self._ds.version, self._filter)]
        return "\n".join(lines)

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

    def nearest(self, column, q, k=None, metric=None, nprobes=None, minimum_nprobes=None, maximum_nprobes=None,
                refine_factor=None, use_index=True, ef=None, distance_range=None, **kwargs) -> "ScannerBuilder":
        return self._set(nearest=dict(column=column, q=q, k=k, metric=metric, nprobes=nprobes,
                                      minimum_nprobes=minimum_nprobes, maximum_nprobes=maximum_nprobes,
                                      refine_factor=refine_factor, use_index=use_index, ef=ef,
                                      distance_range=distance_range, **kwargs))

    def full_text_search(self, query, columns=None) -> "ScannerBuilder":
        if hasattr(query, "inner"):
            return self._set(full_text_query=query)
        return self._set(full_text_query={"query": query, "columns": columns})

    def prefilter(self, enabled: bool) -> "ScannerBuilder":
        return self._set(prefilter=enabled)

    def fast_search(self, enabled: bool) -> "ScannerBuilder":
        return self._set(fast_search=enabled)

    def __getattr__(self, name):
        # Tuning knobs with no effect on the result (readahead, io buffers, ...).
        if name.startswith("_"):
            raise AttributeError(name)
        return lambda *args, **kwargs: self

    def to_scanner(self) -> LanceScanner:
        return LanceScanner(self._ds, **self._options)


# ── writes ────────────────────────────────────────────────────────────────────────────────────────


# ── JSON columns ───────────────────────────────────────────────────────────────────────────────────
# Lance stores JSON as JSONB in a large_binary column marked lance.json and reads it back as
# arrow.json text (pa.json_()); nanolance does the same, converting at the edges (jsonb.hpp).

_LANCE_JSON = {b"ARROW:extension:name": b"lance.json", b"ARROW:extension:metadata": b""}


def _is_arrow_json(field: pa.Field) -> bool:
    t = field.type
    return isinstance(t, pa.BaseExtensionType) and t.extension_name == "arrow.json"


def _is_lance_json(field: pa.Field) -> bool:
    return pa.types.is_large_binary(field.type) and (field.metadata or {}).get(b"ARROW:extension:name") == b"lance.json"


def _json_in_schema(schema: pa.Schema) -> pa.Schema:
    return pa.schema([pa.field(f.name, pa.large_binary(), f.nullable, {**(f.metadata or {}), **_LANCE_JSON})
                      if _is_arrow_json(f) else f for f in schema], metadata=schema.metadata)


def _json_in(batch: pa.RecordBatch) -> pa.RecordBatch:
    """arrow.json text columns as JSONB (Lance's errors for text that is not JSON)."""
    columns = []
    for field, column in zip(batch.schema, batch.columns):
        if _is_arrow_json(field):
            try:
                column = pa.Array._import_from_c_capsule(*_nanolance._json_encode(column.storage))
            except ValueError as exc:
                raise OSError(f"LanceError(Arrow): Invalid argument error: {exc}") from None
        columns.append(column)
    return pa.RecordBatch.from_arrays(columns, schema=_json_in_schema(batch.schema))


def _json_out_field(field: pa.Field) -> pa.Field:
    meta = {k: v for k, v in (field.metadata or {}).items()
            if k not in (b"ARROW:extension:name", b"ARROW:extension:metadata")}
    return pa.field(field.name, pa.json_(pa.utf8()), field.nullable, meta or None)


def _json_out_schema(schema: pa.Schema) -> pa.Schema:
    if not any(_is_lance_json(f) for f in schema):
        return schema
    return pa.schema([_json_out_field(f) if _is_lance_json(f) else f for f in schema], metadata=schema.metadata)


def _json_out(table: pa.Table) -> pa.Table:
    """lance.json JSONB columns as arrow.json text, as pylance returns them."""
    if not any(_is_lance_json(f) for f in table.schema):
        return table
    columns = []
    for field, column in zip(table.schema, table.columns):
        if _is_lance_json(field):
            chunks = [pa.ExtensionArray.from_storage(
                pa.json_(pa.utf8()), pa.Array._import_from_c_capsule(*_nanolance._json_decode(c)))
                for c in column.chunks]
            column = pa.chunked_array(chunks, type=pa.json_(pa.utf8()))
        columns.append(column)
    return pa.Table.from_arrays(columns, schema=_json_out_schema(table.schema))


def _coerce_reader(data_obj: ReaderLike, schema: Optional[pa.Schema] = None) -> pa.RecordBatchReader:
    """Any of the inputs pylance's write_dataset takes, as a RecordBatchReader, with JSON columns
    (arrow.json) as the JSONB Lance stores (lance.json)."""
    reader = _coerce_reader_raw(data_obj, schema)
    if not any(_is_arrow_json(f) for f in reader.schema):
        return reader
    target = _json_in_schema(reader.schema)
    return pa.RecordBatchReader.from_batches(target, (_json_in(b) for b in reader))


def _coerce_reader_raw(data_obj: ReaderLike, schema: Optional[pa.Schema] = None) -> pa.RecordBatchReader:
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
    in_memory = not isinstance(uri, LanceDataset) and os.fspath(uri).startswith("memory://")
    if mode == "create" and exists and in_memory:
        # Every memory:// write is its own store in Lance: creating one never finds an earlier one.
        import shutil

        shutil.rmtree(path, ignore_errors=True)
        exists = False
    if mode == "create" and exists:
        raise OSError(f"Dataset already exists: {path}")
    append = mode == "append" and exists
    target = None
    if append:
        target = LanceDataset(path)._data_schema
        _check_append_schema(target, reader.schema, allow_subset=True)
    options = _nanolance.WriteOptions()
    with native():
        writer = _nanolance._StagedWriter(path, options, append, int(max_rows_per_file or 0),
                                          int(max_bytes_per_file or 0) if max_bytes_per_file < 2**62 else 0)
    if auto_cleanup_options is not None and not exists:
        # Recorded in the first version's config, as Lance records it when it creates a dataset.
        with native():
            writer.set_initial_config("lance.auto_cleanup.interval", str(int(auto_cleanup_options["interval"])))
            writer.set_initial_config("lance.auto_cleanup.older_than",
                                      _format_duration(int(auto_cleanup_options["older_than_seconds"])))
    properties = dict(transaction_properties or {})
    if commit_message is not None:
        properties[LANCE_COMMIT_MESSAGE_KEY] = str(commit_message)
    for key, value in properties.items():
        with native():
            writer.set_transaction_property(str(key), str(value))
    if target is not None and len(reader.schema.names) < len(target.names):
        # Part of the schema: the new files hold those columns alone, as Lance writes them, and the
        # others read as null.
        target = pa.schema([f for f in target if f.name in reader.schema.names], metadata=target.metadata)
        with native():
            writer.project(target.names)
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


def _check_append_schema(target: pa.Schema, given: pa.Schema, allow_subset: bool = False) -> None:
    """Lance's check: no column the dataset lacks, and (with `allow_subset`) only nullable columns
    left out."""
    missing = [f.name for f in target if f.name not in given.names and (not allow_subset or not f.nullable)]
    unexpected = [n for n in given.names if n not in target.names]
    if missing or unexpected:
        raise OSError(
            "Append with different schema: fields did not match, "
            f"missing=[{', '.join(missing)}], unexpected=[{', '.join(unexpected)}]"
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
