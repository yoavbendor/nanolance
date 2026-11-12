#!/usr/bin/env python3
"""Write the legacy blob golden file that tests/file/test_legacy_blob_v21_read.cpp reads.

A `lance-encoding:blob` LargeBinary column (the legacy blob marker, without the Blob v2 extension) in a
format 2.1 file. Lance 2.1 stores its values as a BlobLayout over a packed (position, size) descriptor;
from 2.2 on pylance refuses the marker, so 2.1 is the only version this layout is in. The data file is
copied to tests/golden/legacy_blob_v21/ so the test needs no Python at run time.

The values are a fixed formula, so the test checks them without a reader from Python:
  id = i; a row with i % 7 == 3 is null; a row with i % 5 == 1 is empty (b""); the rest hold
  ((i * 13 + k * 7) & 255) for k in range(len), with len = (i * 17) % 61 + 1, and a row with i % 100 == 0
  holds 70000 bytes (larger than the 64 KiB inline threshold, so the layout is not an inline one by size).

    python3 tests/generate_golden_legacy_blob.py              # write the file
    python3 tests/generate_golden_legacy_blob.py --check-only # only check pylance is there
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

ROWS = 300
ROOT = Path(__file__).resolve().parent
OUT = ROOT / "golden" / "legacy_blob_v21"
FILE_NAME = "v21_legacy_blob.lance"


def value(i: int) -> bytes | None:
    """The formula the test checks: None, empty, a short run of bytes, or one large blob."""
    if i % 7 == 3:
        return None
    if i % 5 == 1:
        return b""
    if i % 100 == 0:
        n = 70000
    else:
        n = (i * 17) % 61 + 1
    return bytes((i * 13 + k * 7) & 255 for k in range(n))


def write_file(lance, pa, directory: Path) -> Path:
    meta = {b"lance-encoding:blob": b"true"}
    blobs = pa.array([value(i) for i in range(ROWS)], pa.large_binary())
    schema = pa.schema([
        pa.field("id", pa.int64(), nullable=False),
        pa.field("b", pa.large_binary(), nullable=True, metadata=meta),
    ])
    table = pa.Table.from_arrays([pa.array(range(ROWS), pa.int64()), blobs], schema=schema)
    dataset_dir = directory / "dataset"
    lance.write_dataset(table, str(dataset_dir), data_storage_version="2.1", max_rows_per_file=ROWS * 2)
    data_files = sorted((dataset_dir / "data").glob("*.lance"))
    if len(data_files) != 1:
        sys.exit(f"expected one data file, found {len(data_files)}")
    return data_files[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check-only", action="store_true", help="only check that pylance and pyarrow import")
    args = parser.parse_args()
    try:
        import pyarrow as pa
        import lance
    except ImportError as exc:  # CTest skips (code 77) when the tools are missing
        print(f"legacy blob golden: {exc}", file=sys.stderr)
        return 77
    if args.check_only:
        return 0
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        data = write_file(lance, pa, Path(tmp))
        shutil.copyfile(data, OUT / FILE_NAME)
        print(f"wrote {OUT / FILE_NAME} (2.1 legacy blob, {ROWS} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
