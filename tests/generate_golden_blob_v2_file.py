#!/usr/bin/env python3
"""Write the blob v2 data-file golden that tests/file/test_blob_v2_file_read.cpp reads.

A Lance 2.2 blob v2 column with an inline row, a null row, an external row (a URI with a position and size) and a
second inline row, written by pylance 12. The data file is copied to tests/golden/blob_v2_file/ so the test needs no
Python at run time. The file's descriptor is the one Lance writes: a struct marked `lance-encoding:blob` whose children
are kind, position, size, blob_id and blob_uri, with no ids of their own.

The values are fixed here, and the test checks them: row 0 is the inline bytes INLINE_A (kind 0, size len(INLINE_A),
its bytes at `position` in the file), row 1 is null, row 2 is external at EXTERNAL_URI (kind 3, position 100, size 64),
row 3 is inline bytes INLINE_B.

    python3 tests/generate_golden_blob_v2_file.py              # write the file
    python3 tests/generate_golden_blob_v2_file.py --check-only # only check pylance is there
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

INLINE_A = b"hello blob v2"
INLINE_B = b"x" * 300
EXTERNAL_URI = "s3://lance-golden/capture.bin"
ROOT = Path(__file__).resolve().parent
OUT = ROOT / "golden" / "blob_v2_file"
FILE_NAME = "v22_blob_v2.lance"


def write_file(lance, pa, directory: Path) -> Path:
    from lance.blob import Blob, blob_array, blob_field

    values = [INLINE_A, None, Blob.from_uri(EXTERNAL_URI, 100, 64), INLINE_B]
    payload = blob_array(values)
    field = blob_field("payload")
    schema = pa.schema([pa.field("id", pa.int64(), nullable=False), field])
    table = pa.Table.from_arrays([pa.array(range(len(values)), pa.int64()), payload], schema=schema)
    dataset_dir = directory / "dataset"
    lance.write_dataset(table, str(dataset_dir), data_storage_version="2.2", max_rows_per_file=len(values) * 2,
                        allow_external_blob_outside_bases=True)
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
        import lance.blob  # noqa: F401
    except ImportError as exc:  # CTest skips (code 77) when the tools are missing
        print(f"blob v2 file golden: {exc}", file=sys.stderr)
        return 77
    if args.check_only:
        return 0
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        data = write_file(lance, pa, Path(tmp))
        shutil.copyfile(data, OUT / FILE_NAME)
        print(f"wrote {OUT / FILE_NAME} (2.2 blob v2, 4 rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
