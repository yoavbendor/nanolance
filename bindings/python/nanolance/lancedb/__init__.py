# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""LanceDB's Python API (``lancedb`` 0.39: ``connect`` / ``connect_async``, tables, query builders, merge_insert,
``lancedb.pydantic``, ``lancedb.index``) over nanolance's Lance datasets -- no Rust core.

    import nanolance.lancedb as lancedb
    db = await lancedb.connect_async("./db")

or ``nanolance.lancedb.install_as_lancedb()``, after which ``import lancedb`` (and ``import lance``, which
``Table.to_lance()`` hands out) load nanolance. Tables are plain Lance datasets: LanceDB and pylance open them.

Local directories and object stores (``s3://...`` with ``storage_options``). LanceDB Cloud (``db://``), embedding
functions, rerankers and hybrid search are not implemented.
"""

from __future__ import annotations

import importlib
import importlib.abc
import importlib.machinery
import importlib.util
import os
import sys
from datetime import timedelta
from typing import Dict, Optional, Union

from nanolance.lancedb.db import AsyncConnection, DBConnection, LanceDBConnection, ListTablesResponse
from nanolance.lancedb.merge import AsyncMergeInsertBuilder, LanceMergeInsertBuilder
from nanolance.lancedb.query import AsyncQuery, AsyncVectorQuery, LanceQueryBuilder
from nanolance.lancedb.table import AsyncTable, LanceTable, Table

__version__ = "0.39.0"  # the lancedb API this mirrors

__all__ = ["connect", "connect_async", "AsyncConnection", "AsyncTable", "DBConnection", "LanceDBConnection",
           "LanceTable", "Table", "ListTablesResponse", "AsyncQuery", "AsyncVectorQuery", "LanceQueryBuilder",
           "LanceMergeInsertBuilder", "AsyncMergeInsertBuilder", "install_as_lancedb", "uninstall_as_lancedb"]


def _local_or_store(uri, api_key) -> str:
    uri = os.fspath(uri)
    if str(uri).startswith("db://"):
        raise NotImplementedError("LanceDB Cloud (db://) is not supported by nanolance.lancedb")
    return uri


def connect(uri: Union[str, os.PathLike], *, api_key: Optional[str] = None, region: str = "us-east-1",
            host_override: Optional[str] = None, read_consistency_interval: Optional[timedelta] = None,
            request_thread_pool=None, client_config=None, storage_options: Optional[Dict[str, str]] = None,
            **kwargs) -> LanceDBConnection:
    """A database at ``uri``: a local directory or an object-store prefix."""
    return LanceDBConnection(_local_or_store(uri, api_key), read_consistency_interval=read_consistency_interval,
                             storage_options=storage_options)


async def connect_async(uri: Union[str, os.PathLike], *, api_key: Optional[str] = None, region: str = "us-east-1",
                        host_override: Optional[str] = None,
                        read_consistency_interval: Optional[timedelta] = None, client_config=None,
                        storage_options: Optional[Dict[str, str]] = None, **kwargs) -> AsyncConnection:
    """``connect``, for the async API."""
    return AsyncConnection(connect(uri, api_key=api_key, read_consistency_interval=read_consistency_interval,
                                   storage_options=storage_options))


# ------------------------------------------------------------------------------------------------ the alias


class _AliasFinder(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    """Resolves ``lancedb`` and ``lancedb.<module>`` to ``nanolance.lancedb[.<module>]``."""

    def find_spec(self, fullname, path=None, target=None):
        if fullname == "lancedb" or fullname.startswith("lancedb."):
            try:
                if importlib.util.find_spec("nanolance." + fullname) is None:
                    return None
            except ModuleNotFoundError:
                return None
            return importlib.machinery.ModuleSpec(fullname, self, is_package=fullname == "lancedb")
        return None

    def create_module(self, spec):
        return importlib.import_module("nanolance." + spec.name)

    def exec_module(self, module):
        pass


_finder = _AliasFinder()


def install_as_lancedb() -> None:
    """Make ``import lancedb`` load nanolance's implementation -- and ``import lance`` too, since
    ``Table.to_lance()`` returns a nanolance dataset and code that uses it imports ``lance``.

    Affects this process only. Call it before anything imports ``lancedb`` or ``lance``.
    """
    import nanolance.lance

    nanolance.lance.install_as_lance()
    if _finder not in sys.meta_path:
        sys.meta_path.insert(0, _finder)
    for name in [n for n in sys.modules if n == "lancedb" or n.startswith("lancedb.")]:
        if not getattr(sys.modules[name], "__name__", "").startswith("nanolance."):
            del sys.modules[name]
    for name, module in list(sys.modules.items()):
        if name == "nanolance.lancedb" or name.startswith("nanolance.lancedb."):
            sys.modules.setdefault(name[len("nanolance."):], module)


def uninstall_as_lancedb() -> None:
    """Undo install_as_lancedb (and its install_as_lance)."""
    import nanolance.lance

    if _finder in sys.meta_path:
        sys.meta_path.remove(_finder)
    for name in [n for n in sys.modules if n == "lancedb" or n.startswith("lancedb.")]:
        if getattr(sys.modules[name], "__name__", "").startswith("nanolance."):
            del sys.modules[name]
    nanolance.lance.uninstall_as_lance()
