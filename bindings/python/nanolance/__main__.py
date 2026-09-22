"""Command line entry point: ``python -m nanolance ...`` (also installed as ``nanolance``).

The one demo that answers "why would I switch?" for a pyarrow.parquet user is converting a file they
already have and looking at the two sizes, so that is what ``convert`` prints.

pyarrow is imported lazily, inside the commands that need it. The library itself has no runtime
pyarrow dependency (it speaks the Arrow PyCapsule interface), and importing it at module scope would
turn ``nanolance --help`` into an import error on an install without it.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional, Sequence

from . import LanceWriter, WriteOptions, __version__

# Conversion reads and writes a batch at a time, so a 50 GB parquet file converts in bounded memory.
# 64Ki rows is pyarrow's own neighbourhood for iter_batches and is small enough that the peak is
# dominated by the fragment being built, not by one batch.
DEFAULT_BATCH_ROWS = 65_536
DEFAULT_ROWS_PER_FRAGMENT = 1_000_000

PARQUET_SUFFIXES = {".parquet", ".pq", ".parq"}


def _require_pyarrow():
    try:
        import pyarrow  # noqa: F401
        import pyarrow.parquet as pq
    except ImportError as exc:  # pragma: no cover - depends on the install
        raise SystemExit(
            "this command needs pyarrow to read parquet: pip install pyarrow"
        ) from exc
    return pq


def _dataset_size_bytes(path: Path) -> int:
    """Total bytes on disk for a Lance dataset -- data files AND manifests.

    Counting only `data/*.lance` would flatter the comparison; the manifests are part of what you
    have to keep.
    """
    return sum(p.stat().st_size for p in path.rglob("*") if p.is_file())


def _human(size: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if size < 1024 or unit == "TiB":
            return f"{size:.1f} {unit}" if unit != "B" else f"{int(size)} B"
        size /= 1024
    raise AssertionError("unreachable")


def _convert(args: argparse.Namespace) -> int:
    pq = _require_pyarrow()

    source = Path(args.input)
    dest = Path(args.output)
    if not source.is_file():
        raise SystemExit(f"input file not found: {source}")
    if source.suffix.lower() not in PARQUET_SUFFIXES:
        # Guessing at the format of an unknown extension would fail deep inside pyarrow with a much
        # worse message than this one.
        raise SystemExit(
            f"expected a parquet file (.parquet/.pq/.parq), got: {source.name}"
        )
    if dest.exists() and not args.overwrite:
        raise SystemExit(f"output already exists (pass --overwrite to replace): {dest}")
    if dest.exists():
        import shutil

        shutil.rmtree(dest)

    reader = pq.ParquetFile(source)
    options = WriteOptions(
        compression=args.compress,
        compression_level=args.compression_level,
        structural_encoding=not args.no_structural,
    )

    rows = 0
    columns = args.columns or None
    with LanceWriter(
        dest, options=options, max_rows_per_fragment=args.rows_per_fragment
    ) as writer:
        for batch in reader.iter_batches(batch_size=args.batch_size, columns=columns):
            if batch.num_rows == 0:
                continue
            writer.write_batch(batch)
            rows += batch.num_rows
            if args.progress:
                print(f"\r  {rows:,} rows", end="", file=sys.stderr, flush=True)
    if args.progress:
        print(file=sys.stderr)

    if rows == 0:
        # LanceWriter refuses to close with nothing written, so this is unreachable via a normal
        # path; keep the check so a future change cannot silently produce an empty dataset.
        raise SystemExit("input file contained no rows")

    source_bytes = source.stat().st_size
    dest_bytes = _dataset_size_bytes(dest)
    print(f"{rows:,} rows  {source.name} -> {dest.name}")
    print(f"  parquet : {_human(source_bytes):>10}")
    if source_bytes > 0:
        ratio = dest_bytes / source_bytes
        verdict = f"{1 / ratio:.2f}x smaller" if ratio < 1 else f"{ratio:.2f}x larger"
        print(f"  lance   : {_human(dest_bytes):>10}   ({verdict})")
    else:
        print(f"  lance   : {_human(dest_bytes):>10}")
    if not args.compress:
        # Worth saying, because the comparison above is against parquet's default compression and
        # this one is not a like-for-like unless the reader knows which knobs were on.
        print(
            "  (structural encodings only; add --compress for zstd on string and float columns)",
            file=sys.stderr,
        )
    return 0


def _inspect(args: argparse.Namespace) -> int:
    import pyarrow as pa

    from . import open_stream

    path = Path(args.dataset)
    if not path.is_dir():
        raise SystemExit(f"not a Lance dataset directory: {path}")
    # The stream's schema is available before any batch is decoded, which is the whole point of
    # asking it rather than reading the table.
    reader = pa.RecordBatchReader.from_stream(open_stream(path))
    schema = reader.schema
    rows = sum(batch.num_rows for batch in reader)

    print(f"{path}")
    print(f"  rows   : {rows:,}")
    print(f"  bytes  : {_human(_dataset_size_bytes(path))}")
    print("  schema :")
    for field in schema:
        null = "" if field.nullable else " not null"
        print(f"    {field.name}: {field.type}{null}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="nanolance",
        description="Convert and inspect Lance datasets.",
    )
    parser.add_argument("--version", action="version", version=f"nanolance {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    convert = sub.add_parser(
        "convert",
        help="convert a parquet file to a Lance dataset",
        description=(
            "Convert a parquet file to a Lance dataset, a batch at a time, and print the two "
            "sizes. Memory use tracks one fragment, not the file, so a file larger than RAM "
            "converts fine."
        ),
    )
    convert.add_argument("input", help="source .parquet file")
    convert.add_argument("output", help="destination .lance dataset directory")
    convert.add_argument(
        "--compress",
        action="store_true",
        help="zstd on string/binary columns and byte-stream-split+zstd on floats (off by default)",
    )
    convert.add_argument(
        "-l",
        "--compression-level",
        type=int,
        default=3,
        help="zstd level; only affects --compress (default: 3)",
    )
    convert.add_argument(
        "--no-structural",
        action="store_true",
        help="disable bitpacking/constant/RLE/dictionary encodings (emit plain pages)",
    )
    convert.add_argument(
        "--columns",
        nargs="+",
        metavar="NAME",
        help="convert only these columns",
    )
    convert.add_argument(
        "--batch-size",
        type=int,
        default=DEFAULT_BATCH_ROWS,
        help=f"rows read from parquet per batch (default: {DEFAULT_BATCH_ROWS})",
    )
    convert.add_argument(
        "--rows-per-fragment",
        type=int,
        default=DEFAULT_ROWS_PER_FRAGMENT,
        help=(
            "rows per Lance fragment, the analogue of a parquet row group "
            f"(default: {DEFAULT_ROWS_PER_FRAGMENT}; 0 = one fragment for everything)"
        ),
    )
    convert.add_argument(
        "--overwrite", action="store_true", help="replace an existing output dataset"
    )
    convert.add_argument(
        "--progress", action="store_true", help="print a running row count to stderr"
    )
    convert.set_defaults(func=_convert)

    inspect = sub.add_parser(
        "inspect",
        help="print a Lance dataset's row count, size and schema",
    )
    inspect.add_argument("dataset", help=".lance dataset directory")
    inspect.set_defaults(func=_inspect)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
