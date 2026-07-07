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
    # Structural, lossless re-encodings (integer bitpacking, ConstantLayout, RLE, string dictionary and
    # dictionary+RLE). On by default and independent of ``compression`` (which controls only zstd).
    structural_encoding: bool = True
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
    structural_encoding: bool | None = None,
    append: bool | None = None,
) -> None:
    """Write an Arrow table to a Lance dataset directory."""
    opts = options or WriteOptions()
    if compression_level is not None:
        opts.compression_level = compression_level
    if compression is not None:
        opts.compression = compression
    if structural_encoding is not None:
        opts.structural_encoding = structural_encoding
    if append is not None:
        opts.append = append
    native = _nanolance.WriteOptions()
    native.compression_level = opts.compression_level
    native.compression = opts.compression
    native.structural_encoding = opts.structural_encoding
    native.blob_uri_dictionary = opts.blob_uri_dictionary
    native.ignore_nullability = opts.ignore_nullability
    native.append = opts.append
    _nanolance.write_table(table, Path(path), native)


class LanceWriter:
    """Streaming, chunked Lance writer with a context-manager API.

    Feed one Arrow ``RecordBatch`` at a time with :meth:`write_batch` so only a
    single chunk needs to live in memory; :meth:`close` (or leaving the ``with``
    block) commits any pending rows and finalizes the dataset::

        import pyarrow as pa
        import nanolance

        schema = pa.schema([("id", pa.int64()), ("value", pa.float64())])
        with nanolance.LanceWriter("big.lance", max_rows_per_fragment=1_000_000) as w:
            for chunk_id in range(10_000):
                batch = pa.record_batch(
                    {
                        "id": pa.array(range(chunk_id * 5_000, (chunk_id + 1) * 5_000)),
                        "value": pa.array([float(i) for i in range(5_000)]),
                    },
                    schema=schema,
                )
                w.write_batch(batch)
                del batch  # only one chunk in memory at a time

    A fragment is the Lance analogue of a Parquet row group. By default all
    batches are committed as a single fragment on ``close()``; pass
    ``max_rows_per_fragment`` > 0 to flush a fragment (and free the writer's
    internal buffer) once that many rows accumulate, bounding memory for very
    large writes. The resulting multi-fragment dataset reads back as one table.
    """

    def __init__(
        self,
        path: Union[str, os.PathLike],
        *,
        options: WriteOptions | None = None,
        compression: bool | None = None,
        compression_level: int | None = None,
        structural_encoding: bool | None = None,
        ignore_nullability: bool | None = None,
        blob_uri_dictionary: bool | None = None,
        append: bool | None = None,
        max_rows_per_fragment: int = 0,
    ) -> None:
        opts = options or WriteOptions()
        if compression is not None:
            opts.compression = compression
        if compression_level is not None:
            opts.compression_level = compression_level
        if structural_encoding is not None:
            opts.structural_encoding = structural_encoding
        if ignore_nullability is not None:
            opts.ignore_nullability = ignore_nullability
        if blob_uri_dictionary is not None:
            opts.blob_uri_dictionary = blob_uri_dictionary
        if append is not None:
            opts.append = append

        native = _nanolance.WriteOptions()
        native.compression_level = opts.compression_level
        native.compression = opts.compression
        native.structural_encoding = opts.structural_encoding
        native.blob_uri_dictionary = opts.blob_uri_dictionary
        native.ignore_nullability = opts.ignore_nullability
        native.append = opts.append
        self._writer = _nanolance.LanceWriter(Path(path), native, int(max_rows_per_fragment))

    def write_batch(self, batch) -> None:
        """Append one Arrow-exportable ``RecordBatch`` (pyarrow, polars, nanom, ...)."""
        self._writer.write_batch(batch)

    def flush(self) -> None:
        """Commit buffered rows as a fragment, forcing a fragment boundary."""
        self._writer.flush()

    def close(self) -> None:
        """Commit any pending rows and finalize the dataset."""
        self._writer.close()

    def __enter__(self) -> "LanceWriter":
        self._writer.__enter__()
        return self

    def __exit__(self, exc_type, exc, tb) -> bool:
        return self._writer.__exit__(exc_type, exc, tb)


def read_table(path: Union[str, os.PathLike]):
    """Read a nanolance-written Lance dataset.

    Returns an Arrow-exportable handle. Pass to ``pyarrow.table()`` or
    ``polars.from_arrow()`` for a zero-copy view. The handle exports an Arrow C
    stream that yields one batch per fragment, so a reader can consume it chunk
    by chunk (e.g. ``for batch in pa.RecordBatchReader.from_stream(handle): ...``).
    """
    return _nanolance.read_table(Path(path))


__all__ = ["write_table", "read_table", "WriteOptions", "LanceWriter"]
