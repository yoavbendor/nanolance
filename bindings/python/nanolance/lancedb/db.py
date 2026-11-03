# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lancedb.db``: a database is a directory (local, or a prefix on an object store) holding one ``<name>.lance``
dataset per table. ``LanceDBConnection`` is the sync API, ``AsyncConnection`` the async one."""

from __future__ import annotations

import os
import re
import shutil
from dataclasses import dataclass, field
from datetime import timedelta
from typing import Dict, Iterable, List, Optional, Union
from urllib.parse import urlparse


import nanolance.lance as nl
from nanolance.lancedb.table import AsyncTable, LanceTable, _to_arrow

__all__ = ["DBConnection", "LanceDBConnection", "AsyncConnection", "ListTablesResponse"]

_NAME = re.compile(r"^[A-Za-z0-9_\-.]+$")


@dataclass
class ListTablesResponse:
    context: Optional[dict] = None
    tables: List[str] = field(default_factory=list)
    page_token: Optional[str] = None


# ------------------------------------------------------------------------------------------------ storage


class _Root:
    """The database directory: list, test and remove the table datasets in it, on a local disk or an object
    store (through pyarrow.fs, with the storage options nanolance takes)."""

    def __init__(self, uri: str, storage_options: Optional[Dict[str, str]]):
        uri = str(uri)
        parsed = urlparse(uri)
        self.remote = parsed.scheme not in ("", "file") and len(parsed.scheme) > 1
        self.storage_options = dict(storage_options or {})
        if self.remote:
            self.uri = uri.rstrip("/")
            self._fs, self._base = self._filesystem(parsed)
        else:
            path = parsed.path if parsed.scheme == "file" else uri
            self.uri = os.path.abspath(os.path.expanduser(path))
            self._fs = None
            self._base = self.uri

    def _filesystem(self, parsed):
        from pyarrow import fs

        o = {k.lower(): v for k, v in self.storage_options.items()}
        if parsed.scheme in ("s3", "s3a"):
            kw = {}
            for key, target in (("aws_access_key_id", "access_key"), ("access_key_id", "access_key"),
                                ("aws_secret_access_key", "secret_key"), ("secret_access_key", "secret_key"),
                                ("aws_session_token", "session_token"), ("session_token", "session_token"),
                                ("aws_region", "region"), ("region", "region")):
                if key in o:
                    kw[target] = o[key]
            endpoint = o.get("aws_endpoint") or o.get("endpoint") or o.get("aws_endpoint_url")
            if endpoint:
                ep = urlparse(endpoint)
                kw["endpoint_override"] = ep.netloc or ep.path
                kw["scheme"] = ep.scheme or "https"
            if str(o.get("aws_virtual_hosted_style_request", "")).lower() == "true":
                kw["force_virtual_addressing"] = True
            return fs.S3FileSystem(**kw), (parsed.netloc + parsed.path).rstrip("/")
        filesystem, path = fs.FileSystem.from_uri(self.uri)
        return filesystem, path.rstrip("/")

    def table_uri(self, name: str) -> str:
        return f"{self.uri}/{name}.lance" if self.remote else os.path.join(self.uri, f"{name}.lance")

    def names(self) -> List[str]:
        if not self.remote:
            if not os.path.isdir(self.uri):
                return []
            entries = [e for e in os.listdir(self.uri) if e.endswith(".lance")
                       and os.path.isdir(os.path.join(self.uri, e))]
        else:
            from pyarrow import fs

            infos = self._fs.get_file_info(fs.FileSelector(self._base, allow_not_found=True))
            entries = [i.base_name for i in infos if i.type == fs.FileType.Directory and i.base_name.endswith(".lance")]
        return sorted(e[: -len(".lance")] for e in entries)

    def exists(self, name: str) -> bool:
        if not self.remote:
            versions = os.path.join(self.table_uri(name), "_versions")
            return os.path.isdir(versions) and any(True for _ in os.scandir(versions))
        try:
            nl.dataset(self.table_uri(name), storage_options=self.storage_options or None)
            return True
        except Exception:
            return False

    def remove(self, name: str) -> None:
        if not self.remote:
            shutil.rmtree(self.table_uri(name), ignore_errors=True)
        else:
            self._fs.delete_dir(f"{self._base}/{name}.lance")

    def ensure(self) -> None:
        if not self.remote:
            os.makedirs(self.uri, exist_ok=True)


# ------------------------------------------------------------------------------------------------ sync


class LanceDBConnection:
    """``lancedb.connect(uri)``: the tables of one directory."""

    def __init__(self, uri: Union[str, os.PathLike], *, read_consistency_interval: Optional[timedelta] = None,
                 storage_options: Optional[Dict[str, str]] = None):
        self._root = _Root(os.fspath(uri), storage_options)
        self.read_consistency_interval = read_consistency_interval
        self._open = True

    @property
    def uri(self) -> str:
        return self._root.uri

    def __repr__(self) -> str:
        return f"ListingDatabase(uri={self.uri}, read_consistency_interval={self.read_consistency_interval})"

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self) -> None:
        self._open = False

    def is_open(self) -> bool:
        return self._open

    def _check_name(self, name: str) -> None:
        if not name or not _NAME.match(name):
            raise ValueError(f"Invalid table name {name!r}: table names can only contain alphanumeric characters, "
                             "underscores, hyphens, and periods")

    def table_names(self, page_token: Optional[str] = None, limit: Optional[int] = None, *,
                    start_after: Optional[str] = None, namespace_path=None, **_) -> List[str]:
        names = self._root.names()
        start = start_after if start_after is not None else page_token
        if start is not None:
            names = [n for n in names if n > start]
        return names[:limit] if limit else names

    def list_tables(self, namespace_path=None, page_token: Optional[str] = None,
                    limit: Optional[int] = None) -> ListTablesResponse:
        names = self.table_names(start_after=page_token, limit=limit)
        more = bool(limit) and len(self._root.names()) > len(names) + (0 if page_token is None else len(
            [n for n in self._root.names() if n <= page_token]))
        return ListTablesResponse(tables=names, page_token=names[-1] if more and names else None)

    def __contains__(self, name: str) -> bool:
        return self._root.exists(name)

    def __len__(self) -> int:
        return len(self._root.names())

    def _table(self, name: str, storage_options=None) -> LanceTable:
        options = {**self._root.storage_options, **(storage_options or {})} or None
        return LanceTable(self, name, self._root.table_uri(name), options)

    def create_table(self, name: str, data=None, schema=None, mode: Optional[str] = None,
                     exist_ok: Optional[bool] = None, on_bad_vectors: Optional[str] = None,
                     fill_value: Optional[float] = None, embedding_functions=None, *,
                     storage_options: Optional[Dict[str, str]] = None, **_) -> LanceTable:
        self._check_name(name)
        if hasattr(schema, "to_arrow_schema"):
            schema = schema.to_arrow_schema()
        if data is None and schema is None:
            raise ValueError("Either data or schema must be provided")
        mode = mode or "create"
        if mode not in ("create", "overwrite"):
            raise ValueError(f"mode must be either 'create' or 'overwrite', got {mode!r}")
        table = self._table(name, storage_options)
        exists = self._root.exists(name)
        if exists and mode == "create":
            if exist_ok:
                return table
            raise ValueError(f"Table '{name}' already exists")
        if data is None:
            arrow = schema.empty_table()
        else:
            from nanolance.lancedb.table import _handle_bad_vectors

            arrow = _handle_bad_vectors(_to_arrow(data, schema), on_bad_vectors or "error", fill_value or 0.0)
        self._root.ensure()
        table._write(arrow, "overwrite" if exists else "create")
        return table

    def open_table(self, name: str, *, storage_options: Optional[Dict[str, str]] = None,
                   index_cache_size: Optional[int] = None, version: Optional[int] = None, **_) -> LanceTable:
        if not self._root.exists(name):
            raise ValueError(f"Table '{name}' was not found")
        table = self._table(name, storage_options)
        if version is not None:
            table.checkout(version)
        return table

    def drop_table(self, name: str, *, ignore_missing: bool = False, namespace_path=None) -> None:
        if not self._root.exists(name):
            if ignore_missing:
                return
            raise ValueError(f"Table '{name}' was not found")
        self._root.remove(name)

    def drop_all_tables(self, namespace_path=None) -> None:
        for name in self._root.names():
            self._root.remove(name)

    drop_database = drop_all_tables

    def rename_table(self, cur_name: str, new_name: str, *_, **__) -> None:
        raise NotImplementedError("rename_table is not supported in LanceDB OSS")


DBConnection = LanceDBConnection


# ------------------------------------------------------------------------------------------------ async


class AsyncConnection:
    """``await lancedb.connect_async(uri)``: the same database, its methods coroutines."""

    def __init__(self, connection: LanceDBConnection):
        self._inner = connection

    @property
    def uri(self) -> str:
        return self._inner.uri

    def __repr__(self) -> str:
        return repr(self._inner)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self) -> None:
        self._inner.close()

    def is_open(self) -> bool:
        return self._inner.is_open()

    async def table_names(self, *, namespace_path=None, start_after: Optional[str] = None,
                          limit: Optional[int] = None) -> Iterable[str]:
        return self._inner.table_names(start_after=start_after, limit=limit)

    async def list_tables(self, namespace_path=None, page_token: Optional[str] = None,
                          limit: Optional[int] = None) -> ListTablesResponse:
        return self._inner.list_tables(page_token=page_token, limit=limit)

    async def create_table(self, name: str, data=None, schema=None, mode: Optional[str] = None,
                           exist_ok: Optional[bool] = None, on_bad_vectors: Optional[str] = None,
                           fill_value: Optional[float] = None, storage_options: Optional[Dict[str, str]] = None,
                           **kwargs) -> AsyncTable:
        return AsyncTable(self._inner.create_table(name, data, schema, mode, exist_ok, on_bad_vectors, fill_value,
                                                   storage_options=storage_options))

    async def open_table(self, name: str, *, storage_options: Optional[Dict[str, str]] = None,
                         index_cache_size: Optional[int] = None, version: Optional[int] = None,
                         **kwargs) -> AsyncTable:
        return AsyncTable(self._inner.open_table(name, storage_options=storage_options, version=version))

    async def drop_table(self, name: str, *, namespace_path=None, ignore_missing: bool = False) -> None:
        self._inner.drop_table(name, ignore_missing=ignore_missing)

    async def drop_all_tables(self, namespace_path=None) -> None:
        self._inner.drop_all_tables()

    def drop_database(self) -> None:
        self._inner.drop_all_tables()

    async def rename_table(self, cur_name: str, new_name: str, *_, **__) -> None:
        self._inner.rename_table(cur_name, new_name)

    async def get_read_consistency_interval(self) -> Optional[timedelta]:
        return self._inner.read_consistency_interval
