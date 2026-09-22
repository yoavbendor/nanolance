"""Fast zero-copy Python bindings for the nanolance Lance writer/reader.

This is the C++ nanolance library — not the official Rust Lance SDK. For stock
Lance semantics (blob fetch, scans, etc.) install ``pylance`` and ``import lance``.
The two packages coexist: ``import nanolance`` does not shadow ``import lance``.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Sequence, Union

from nanolance import _nanolance

# The installed distribution's version, so `nanolance.__version__` answers the first question anyone
# asks a wheel. Read from the package metadata rather than duplicated here: pyproject.toml already
# has to stay in step with NANOLANCE_VERSION_* in the top-level CMakeLists.txt, and a third copy
# would be a third thing to forget. "0+unknown" is what you get running from a source tree that was
# never installed.
try:  # pragma: no cover - trivial, and the fallback only fires outside an install
    from importlib.metadata import PackageNotFoundError, version as _dist_version

    __version__ = _dist_version("nanolance")
except (ImportError, PackageNotFoundError):  # pragma: no cover
    __version__ = "0+unknown"


@dataclass
class WriteOptions:
    compression_level: int = 3
    compression: bool = False
    # Structural, lossless re-encodings (integer bitpacking, ConstantLayout, RLE, string dictionary and
    # dictionary+RLE). On by default and independent of ``compression`` (which controls only zstd).
    structural_encoding: bool = True
    blob_uri_dictionary: bool = False
    # Deprecated no-op, kept so existing keyword arguments still work. Nullable-flagged fields are
    # accepted unconditionally now, and a null VALUE is stored rather than refused -- nanolance writes
    # Lance's definition-level layer. The few nulls that are still refused (a null struct, a null in a
    # lance.blob.v2 column) are refused either way, naming the column and row.
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


def _normalize_columns(columns: Optional[Sequence[str]]) -> Optional[list]:
    """Validate a `columns=` argument once, for every entry point that takes one."""
    if columns is None:
        return None
    # A bare string is the trap: `str` IS a Sequence[str] -- of its own characters -- so "abc" would
    # quietly become ["a", "b", "c"] and then fail as three unknown columns.
    if isinstance(columns, str):
        raise TypeError("columns must be a sequence of column names, not a single string")
    names = [str(name) for name in columns]
    if not names:
        raise ValueError("columns must name at least one column; pass columns=None to read them all")
    return names


def read_table(path: Union[str, os.PathLike], columns: Optional[Sequence[str]] = None):
    """Read a Lance dataset.

    Returns an Arrow-exportable handle. Pass to ``pyarrow.table()`` or
    ``polars.from_arrow()`` for a zero-copy view. The handle exports an Arrow C
    stream that yields one batch per fragment, so a reader can consume it chunk
    by chunk (e.g. ``for batch in pa.RecordBatchReader.from_stream(handle): ...``).

    ``columns`` names the top-level columns to read, the same way
    ``pyarrow.parquet.read_table(..., columns=[...])`` does::

        nanolance.read_table("events.lance", columns=["ts", "level"])

    This is a real projection, not a post-filter: the columns you do not ask for
    are skipped during decode rather than decoded and thrown away. Column
    materialization is where a read spends its time, so on a wide table read for
    a few columns that skipped work IS the cost. Naming a column that does not
    exist is an error, not a silently empty result, and so is an empty list --
    pass ``columns=None`` to read everything.

    One difference from ``pyarrow.parquet``: the result is in the dataset's own
    column order, not the order you listed. ``columns=["c", "a"]`` gives back
    ``["a", "c"]``. Reorder afterwards (``table.select([...])``) if it matters.
    """
    return _nanolance.read_table(Path(path), _normalize_columns(columns))


def open_stream(path: Union[str, os.PathLike], columns: Optional[Sequence[str]] = None):
    """Open a Lance dataset as a *streaming* Arrow handle.

    Same decode as :func:`read_table`, but one batch per fragment decoded when the
    consumer asks for it, instead of every batch before you get anything::

        import pyarrow as pa

        reader = pa.RecordBatchReader.from_stream(nanolance.open_stream("big.lance"))
        for batch in reader:
            ...  # peak memory tracks ONE fragment, not the dataset

    Measured on a 61 MiB dataset in 16 fragments: peak 5.6 MiB streamed vs
    61.4 MiB materialized, and 3.2 ms to the first batch vs 66.5 ms. That is
    what makes a larger-than-memory dataset readable.

    Two differences from :func:`read_table`, both inherent to streaming rather
    than incidental:

    * **Errors surface late.** Opening validates the manifest and the schema;
      a corrupt *data file* is only discovered when the batch containing it is
      pulled. ``read_table`` reports it at call time because it decodes
      everything there. The exception also comes from the consumer (pyarrow
      raises ``OSError``) rather than from nanolance.
    * **The handle is single-shot.** A stream is consumed, not copied, so
      exporting it twice raises. Call ``open_stream`` again for a second pass.

    ``columns`` projects exactly as it does for :func:`read_table`.
    """
    return _nanolance.open_stream(Path(path), _normalize_columns(columns))


__all__ = ["write_table", "read_table", "open_stream", "WriteOptions", "LanceWriter", "__version__"]
