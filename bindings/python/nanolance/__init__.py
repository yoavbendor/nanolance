"""Fast zero-copy Python bindings for the nanolance Lance writer/reader.

This is the C++ nanolance library — not the official Rust Lance SDK. For stock
Lance semantics (blob fetch, scans, etc.) install ``pylance`` and ``import lance``.
The two packages coexist: ``import nanolance`` does not shadow ``import lance``.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Union

from nanolance import _nanolance


@dataclass
class WriteOptions:
    compression_level: int = 3
    compression: bool = False
    blob_uri_dictionary: bool = False
    ignore_nullability: bool = True
    append: bool = False


def write_table(
    table,
    path: Union[str, os.PathLike],
    *,
    options: WriteOptions | None = None,
    compression_level: int | None = None,
    compression: bool | None = None,
    append: bool | None = None,
) -> None:
    """Write an Arrow table to a Lance dataset directory."""
    opts = options or WriteOptions()
    if compression_level is not None:
        opts.compression_level = compression_level
    if compression is not None:
        opts.compression = compression
    if append is not None:
        opts.append = append
    native = _nanolance.WriteOptions()
    native.compression_level = opts.compression_level
    native.compression = opts.compression
    native.blob_uri_dictionary = opts.blob_uri_dictionary
    native.ignore_nullability = opts.ignore_nullability
    native.append = opts.append
    _nanolance.write_table(table, Path(path), native)


def read_table(path: Union[str, os.PathLike]):
    """Read a nanolance-written Lance dataset.

    Returns an Arrow-exportable handle. Pass to ``pyarrow.table()`` or
    ``polars.from_arrow()`` for a zero-copy view.
    """
    return _nanolance.read_table(Path(path))


__all__ = ["write_table", "read_table", "WriteOptions"]
