#!/usr/bin/env python3
"""Write the packed-struct golden files that tests/file/test_packed_struct_read.cpp reads.

Lance 2.1 and 2.2 store a struct whose fixed-width children are all packed (field metadata
`lance-encoding:packed` = "true") as ONE column: each row's child values interleaved, row-major.
pylance writes them this way. This writes small datasets with pylance, then copies each dataset's
single data file into tests/golden/packed_struct/ so the test needs no Python at run time.

The values are a fixed formula, so the test checks them without a reader from Python:
  id = i, s.a = 3 * i (uint64), s.b = i + 7 (uint32), and a row with i % 7 == 3 has a null s
  in the nullable case.

    python3 tests/generate_golden_packed_struct.py            # write the files
    python3 tests/generate_golden_packed_struct.py --check-only   # only check pylance is there
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

ROWS = 1300
ROOT = Path(__file__).resolve().parent
OUT = ROOT / "golden" / "packed_struct"


def cases():
    # (file name, Lance data storage version, nullable struct)
    return [
        ("v21_packed.lance", "2.1", False),
        ("v22_packed.lance", "2.2", False),
        ("v21_packed_nullable.lance", "2.1", True),
    ]


def write_case(lance, pa, directory: Path, version: str, nullable: bool, keep_dataset: Path | None = None) -> Path:
    meta = {b"lance-encoding:packed": b"true"}
    struct_type = pa.struct([pa.field("a", pa.uint64(), nullable=False),
                             pa.field("b", pa.uint32(), nullable=False)])
    rows = []
    for i in range(ROWS):
        if nullable and i % 7 == 3:
            rows.append(None)
        else:
            rows.append({"a": 3 * i, "b": i + 7})
    structs = pa.array(rows, type=struct_type)
    schema = pa.schema([
        pa.field("id", pa.int64(), nullable=False),
        pa.field("s", struct_type, nullable=nullable, metadata=meta),
    ])
    table = pa.Table.from_arrays([pa.array(range(ROWS), pa.int64()), structs], schema=schema)
    dataset_dir = directory / "dataset"
    lance.write_dataset(table, str(dataset_dir), data_storage_version=version, max_rows_per_file=ROWS * 2)
    data_files = sorted((dataset_dir / "data").glob("*.lance"))
    if len(data_files) != 1:
        sys.exit(f"expected one data file for {version}, found {len(data_files)}")
    if keep_dataset is not None:
        shutil.copytree(dataset_dir, keep_dataset, dirs_exist_ok=True)
    return data_files[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check-only", action="store_true", help="only check that pylance and pyarrow import")
    args = parser.parse_args()
    try:
        import pyarrow as pa
        import lance
    except ImportError as exc:  # CTest skips (code 77) when the tools are missing
        print(f"packed-struct golden: {exc}", file=sys.stderr)
        return 77
    if args.check_only:
        return 0
    OUT.mkdir(parents=True, exist_ok=True)
    for name, version, nullable in cases():
        with tempfile.TemporaryDirectory() as tmp:
            keep = OUT / "v21_dataset" if name == "v21_packed.lance" else None
            if keep is not None and keep.exists():
                shutil.rmtree(keep)
            data = write_case(lance, pa, Path(tmp), version, nullable, keep_dataset=keep)
            shutil.copyfile(data, OUT / name)
            print(f"wrote {OUT / name} ({version}, nullable struct: {nullable})")
    print(f"wrote {OUT / 'v21_dataset'} (a dataset of the 2.1 file)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
