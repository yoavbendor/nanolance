#!/usr/bin/env python3
"""The datasets lance-c's C and C++ tests take as arguments, as its own harness builds them
(lance-c/tests/compile_and_run_test.rs): written by pylance, or by nanolance.lance.

    python tools/lance_c_fixtures.py OUT_DIR --writer pylance|nanolance

Creates OUT_DIR/c_test_ds (id int32, name utf8, embedding fixed_size_list<float32, 8>; ten rows
written, ten appended) and OUT_DIR/blob_ds (a Blob v2 column of 8-, 128- and 1024-byte, empty and
null values in two fragments; pylance only -- nanolance writes Blob v2 through its own API).
"""

from __future__ import annotations

import argparse
from pathlib import Path

import pyarrow as pa

NAMES = [
    ["alice", "bob", "carol", "dave", "eve", "frank", "grace", "heidi", "ivan", "judy"],
    ["kate", "leo", "maya", "nick", "olga", "paul", "quinn", "ruth", "sam", "tina"],
]
SCHEMA = pa.schema([
    pa.field("id", pa.int32(), nullable=False),
    pa.field("name", pa.utf8()),
    pa.field("embedding", pa.list_(pa.field("item", pa.float32(), nullable=False), 8), nullable=False),
])


def batch(start: int, names) -> pa.Table:
    ids = list(range(start, start + len(names)))
    values = pa.array([i * 0.1 + c for i in ids for c in range(8)], pa.float32())
    vectors = pa.FixedSizeListArray.from_arrays(values, type=SCHEMA.field("embedding").type)
    return pa.table([pa.array(ids, pa.int32()), pa.array(names), vectors], schema=SCHEMA)


def blob_dataset(lance, uri: str) -> None:
    schema = pa.schema([
        pa.field("id", pa.uint32(), nullable=False),
        lance.blob_field("blob", nullable=True, inline_size_threshold=16, dedicated_size_threshold=256),
        pa.field("raw", pa.binary()),
    ])
    for first, mode in ((0, "create"), (100, "append")):
        payloads = [bytes((i * 7 + 3) % 256 for i in range(n)) for n in (8, 128, 1024)] + [b"", None]
        table = pa.table([
            pa.array(range(first, first + 5), pa.uint32()),
            lance.blob_array(payloads),
            pa.array([b"raw"] * 4 + [None], pa.binary()),
        ], schema=schema)
        lance.write_dataset(table, uri, mode=mode, data_storage_version="2.2")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path)
    ap.add_argument("--writer", choices=["pylance", "nanolance"], default="pylance")
    args = ap.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    if args.writer == "pylance":
        import lance
    else:
        import nanolance.lance as lance
    uri = str(args.out / "c_test_ds")
    lance.write_dataset(batch(1, NAMES[0]), uri)
    lance.write_dataset(batch(11, NAMES[1]), uri, mode="append")
    import lance as pylance  # blob v2 fixtures come from pylance either way

    blob_dataset(pylance, str(args.out / "blob_ds"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
