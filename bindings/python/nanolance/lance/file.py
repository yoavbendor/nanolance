# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.file`` as nanolance implements it: reading and writing single Lance files."""

from __future__ import annotations

import os
import shutil
import struct
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Union

import pyarrow as pa

from nanolance import _nanolance
from nanolance.lance._errors import native, unsupported

#: The file format version nanolance writes, and the one pylance 12 calls stable.
_VERSION = "2.2"
_VERSIONS = {None, "stable", "next", "2.2"}


def stable_version() -> str:
    return _VERSION


@dataclass
class LanceBufferDescriptor:
    position: int
    size: int


@dataclass
class LancePageMetadata:
    buffers: List[LanceBufferDescriptor]
    encoding: str


@dataclass
class LanceColumnMetadata:
    column_buffers: List[LanceBufferDescriptor]
    pages: List[LancePageMetadata]


@dataclass
class LanceFileMetadata:
    schema: pa.Schema
    num_rows: int
    num_data_bytes: int
    num_column_metadata_bytes: int
    num_global_buffer_bytes: int
    global_buffers: List[LanceBufferDescriptor]
    columns: List[LanceColumnMetadata]
    major_version: int = 2
    minor_version: int = 2


@dataclass
class LanceColumnStatistics:
    num_pages: int
    size_bytes: int


@dataclass
class LanceFileStatistics:
    columns: List[LanceColumnStatistics] = field(default_factory=list)


class ReaderResults:
    def __init__(self, reader: pa.RecordBatchReader):
        self._reader = reader

    def to_batches(self) -> pa.RecordBatchReader:
        return self._reader

    def to_table(self) -> pa.Table:
        return self._reader.read_all()


def _local(path: Union[str, Path]) -> str:
    text = os.fspath(path)
    if text.startswith("file://"):
        text = text[len("file://"):]
    elif "://" in text:
        raise unsupported(f"the storage scheme of {text!r} (local paths only)")
    return text


def _footer(path: str):
    """Lance v2's 40-byte footer: column metadata start, column / global buffer offset tables, their
    counts, the format version and the magic."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        f.seek(size - 40)
        tail = f.read(40)
        cm_start, cmo_start, gbo_start, n_global, n_cols, major, minor = struct.unpack("<QQQIIHH", tail[:36])
        if tail[36:] != b"LANC":
            raise ValueError(f"{path} is not a Lance file")
        f.seek(gbo_start)
        raw = f.read(16 * n_global)
    globals_ = [LanceBufferDescriptor(*struct.unpack_from("<QQ", raw, 16 * i)) for i in range(n_global)]
    return cm_start, cmo_start, gbo_start, globals_, major, minor, size


class LanceFileReader:
    """Mirrors ``lance.file.LanceFileReader``."""

    def __init__(self, path: str, storage_options: Optional[Dict[str, str]] = None, columns: Optional[List[str]] = None,
                 *, namespace_client=None, table_id=None, _inner_reader=None):
        self._path = _local(path)
        self._columns = None if columns is None else [str(c) for c in columns]
        with native():
            self._rows, _, schema, self._pages = _nanolance._file_info(self._path)
        self._schema = pa.schema(schema)

    def _results(self, table: pa.Table, batch_size: int) -> ReaderResults:
        if self._columns is not None:
            table = table.select(self._columns)
        batches = table.to_batches(max_chunksize=max(int(batch_size), 1)) if table.num_rows else []
        return ReaderResults(pa.RecordBatchReader.from_batches(table.schema, batches))

    def read_all(self, *, batch_size: int = 1024, batch_readahead=16) -> ReaderResults:
        with native():
            table = pa.table(_nanolance._file_read(self._path, self._columns, 0, -1))
        return self._results(table, batch_size)

    def read_range(self, start: int, num_rows: int, *, batch_size: int = 1024, batch_readahead=16) -> ReaderResults:
        if start < 0 or num_rows < 0 or start + num_rows > self._rows:
            raise ValueError(f"range {start}..{start + num_rows} is out of bounds for {self._rows} rows")
        with native():
            table = pa.table(_nanolance._file_read(self._path, self._columns, int(start), int(num_rows)))
        return self._results(table, batch_size)

    def take_rows(self, indices, *, batch_size: int = 1024, batch_readahead=16) -> ReaderResults:
        wanted = [int(i) for i in (indices.to_pylist() if isinstance(indices, pa.Array) else indices)]
        for a, b in zip(wanted, wanted[1:]):
            if a > b:
                raise ValueError(f"Indices must be sorted in ascending order for file API, got {a} > {b}")
        for i in wanted:
            if i < 0 or i >= self._rows:
                raise ValueError(f"row {i} is out of bounds for {self._rows} rows")
        distinct = sorted(set(wanted))
        with native():
            table = pa.table(_nanolance._file_take(self._path, distinct, self._columns))
        if wanted != distinct:
            position = {row: k for k, row in enumerate(distinct)}
            table = table.take(pa.array([position[i] for i in wanted], pa.int64()))
        return self._results(table, batch_size)

    def metadata(self) -> LanceFileMetadata:
        cm_start, cmo_start, gbo_start, globals_, major, minor, _ = _footer(self._path)
        columns = [
            LanceColumnMetadata(
                column_buffers=[],
                pages=[LancePageMetadata([LanceBufferDescriptor(o, s) for o, s in buffers], encoding)
                       for _, buffers, encoding in pages],
            )
            for pages in self._pages
        ]
        return LanceFileMetadata(
            schema=self._schema,
            num_rows=self._rows,
            num_data_bytes=cm_start,
            num_column_metadata_bytes=gbo_start - cm_start,
            num_global_buffer_bytes=sum(b.size for b in globals_),
            global_buffers=globals_,
            columns=columns,
            major_version=major,
            minor_version=minor,
        )

    def file_statistics(self) -> LanceFileStatistics:
        return LanceFileStatistics([
            LanceColumnStatistics(len(pages), sum(s for _, buffers, _ in pages for _, s in buffers))
            for pages in self._pages
        ])

    def read_global_buffer(self, index: int) -> bytes:
        _, _, _, globals_, _, _, _ = _footer(self._path)
        buffer = globals_[index]
        with open(self._path, "rb") as f:
            f.seek(buffer.position)
            return f.read(buffer.size)

    def num_rows(self) -> int:
        return self._rows


class LanceFileWriter:
    """Mirrors ``lance.file.LanceFileWriter``: batches in, one Lance file out at close().

    The rows are written through nanolance's dataset writer into a scratch directory next to the
    file and moved into place at close(); a writer that is never closed leaves nothing behind.
    """

    def __init__(self, path: str, schema: Optional[pa.Schema] = None, *, data_cache_bytes: Optional[int] = None,
                 version: Optional[str] = None, storage_options: Optional[Dict[str, str]] = None,
                 namespace_client=None, table_id=None, keep_original_array: Optional[bool] = None,
                 max_page_bytes: Optional[int] = None, _inner_writer=None, **kwargs):
        if version not in _VERSIONS:
            raise unsupported(f"writing Lance file version {version!r} (nanolance writes {_VERSION})")
        self._path = _local(path)
        self._schema = schema
        self._rows = 0
        self._closed = False
        self.size_bytes: Optional[int] = None
        parent = os.path.dirname(os.path.abspath(self._path))
        os.makedirs(parent, exist_ok=True)
        self._scratch = os.path.join(parent, f".{os.path.basename(self._path)}.{uuid.uuid4().hex}.nanolance")
        self._writer = None

    def _open(self, schema: pa.Schema):
        if self._writer is None:
            with native():
                self._writer = _nanolance._StagedWriter(self._scratch, _nanolance.WriteOptions(), False, 0, 0)
            self._schema = schema

    def write_batch(self, batch: Union[pa.RecordBatch, pa.Table]) -> None:
        if self._closed:
            raise ValueError("the writer is closed")
        batches = batch.to_batches() if isinstance(batch, pa.Table) else [batch]
        if isinstance(batch, pa.Table) and not batches:
            batches = [pa.RecordBatch.from_pylist([], schema=batch.schema)]
        for b in batches:
            if self._schema is not None and not b.schema.equals(self._schema, check_metadata=False):
                b = b.cast(self._schema)
            self._open(b.schema)
            with native():
                self._writer.write_batch(b)
            self._rows += b.num_rows

    def close(self) -> Optional[int]:
        if self._closed:
            return self._rows
        if self._writer is None:
            if self._schema is None:
                self._abort()
                raise ValueError("Schema is unknown and file cannot be created")
            self.write_batch(pa.RecordBatch.from_pylist([], schema=self._schema))
        with native():
            self._writer.finish(_nanolance.COMMIT_CREATE, self._rows == 0)
        data = os.path.join(self._scratch, "data")
        files = [f for f in os.listdir(data) if f.endswith(".lance")]
        if len(files) != 1:
            self._abort()
            raise RuntimeError(f"expected one data file, found {len(files)}")
        os.replace(os.path.join(data, files[0]), self._path)
        self._abort()
        self._closed = True
        self.size_bytes = os.path.getsize(self._path)
        return self._rows

    def _abort(self) -> None:
        self._writer = None
        shutil.rmtree(self._scratch, ignore_errors=True)

    def add_schema_metadata(self, key: str, value: str) -> None:
        raise unsupported("LanceFileWriter.add_schema_metadata")

    def add_global_buffer(self, data: bytes) -> int:
        raise unsupported("LanceFileWriter.add_global_buffer")

    def __enter__(self) -> "LanceFileWriter":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        if exc_type is not None:
            self._abort()
            self._closed = True
            return
        self.close()

    def __del__(self):
        if not getattr(self, "_closed", True):
            self._abort()


class LanceFileSession:
    """Mirrors ``lance.file.LanceFileSession`` for a local directory."""

    def __init__(self, base_path: str, storage_options: Optional[Dict[str, str]] = None, namespace_client=None,
                 table_id=None):
        self._base = _local(base_path)

    def _p(self, path: str) -> str:
        return os.path.join(self._base, path)

    def open_reader(self, path: str, columns: Optional[List[str]] = None) -> LanceFileReader:
        return LanceFileReader(self._p(path), columns=columns)

    def open_writer(self, path: str, *, schema: Optional[pa.Schema] = None, data_cache_bytes=None, version=None,
                    keep_original_array=None, max_page_bytes=None) -> LanceFileWriter:
        return LanceFileWriter(self._p(path), schema, version=version)

    def contains(self, path: str) -> bool:
        return os.path.exists(self._p(path))

    def list(self, path: Optional[str] = None) -> List[str]:
        root = self._p(path) if path else self._base
        out = []
        for dirpath, _, files in os.walk(root):
            for f in files:
                out.append(os.path.relpath(os.path.join(dirpath, f), self._base))
        return sorted(out)

    def read_range(self, path: str, offset: int, length: int) -> bytes:
        with open(self._p(path), "rb") as f:
            f.seek(offset)
            return f.read(length)

    def delete_file(self, path: str) -> None:
        os.remove(self._p(path))

    def upload_file(self, local_path, remote_path: str) -> None:
        os.makedirs(os.path.dirname(self._p(remote_path)) or ".", exist_ok=True)
        shutil.copyfile(os.fspath(local_path), self._p(remote_path))

    def download_file(self, remote_path: str, local_path) -> None:
        shutil.copyfile(self._p(remote_path), os.fspath(local_path))


def __getattr__(name: str):
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.file.{name}")
    globals()[name] = value
    return value
