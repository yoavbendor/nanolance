#!/usr/bin/env python3
"""Generate an Arrow IPC stream with a lance.blob.v2 column for the fuselance demo.

Three source text files are written next to the output, each a cyclic repetition of
an alphabet:

    letters.txt          "abc...xyz" repeated
    capital_letters.txt  "ABC...XYZ" repeated
    numerals.txt         "0123456789" repeated

The IPC stream has two columns:

    name         string         -> becomes the fuselance file name (--filename-col name)
    payload_ref  lance.blob.v2  -> external references into the three files

Rows are *interleaved* (letters, capital_letters, numerals, letters, ...) and each
file is split into several segments, so the blob references for one file are scattered
across many non-adjacent rows. fuselance groups rows by the `name` value and
concatenates each group's segments back in order, reconstructing the original files.

Why IPC and not a Lance dataset directly? nanolance's manifest reader targets the
Lance 2.2 manifest layout; current pylance writes a newer manifest variant nanolance
does not decode. So we emit the *write-side* blob.v2 Arrow IPC layout here and let the
native `arrowipc2lance` tool produce a nanolance-readable .lance (see gen_demo.sh).

Usage:
    python3 make_demo_arrow.py [--output-dir DIR] [--segments N]
"""

from __future__ import annotations

import argparse
import string
import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", type=Path, default=Path("/tmp/fuselance_demo"))
    parser.add_argument("--segments", type=int, default=4,
                        help="number of interleaved segments each file is split into")
    args = parser.parse_args(argv)

    try:
        import pyarrow as pa
        import pyarrow.ipc as ipc
        from lance.blob import Blob, blob_array, blob_field
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"fuselance demo skipped: needs pyarrow + pylance ({exc})", file=sys.stderr)
        print("install with: pip install pyarrow pylance", file=sys.stderr)
        return 77

    out_dir: Path = args.output_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    # name (path-like) -> (alphabet, repetition count, filename for storage)
    specs = {
        "/chars/lower":    (string.ascii_lowercase, 40,  "lower.txt"),
        "/chars/upper":    (string.ascii_uppercase, 40,  "upper.txt"),
        "/chars/numerals": ("0123456789",           100, "numerals.txt"),
    }
    files: dict[str, tuple[str, int]] = {}
    for name, (alphabet, cycles, fname) in specs.items():
        path = out_dir / fname
        path.write_text(alphabet * cycles)
        files[name] = (path.as_uri(), path.stat().st_size)
        print(f"  {fname}: {files[name][1]} bytes")

    segs = max(1, args.segments)
    names: list[str] = []
    blobs = []
    for i in range(segs):
        for name, (uri, size) in files.items():
            seg = size // segs
            pos = i * seg
            length = size - pos if i == segs - 1 else seg  # last segment takes the remainder
            names.append(name)
            blobs.append(Blob.from_uri(uri, position=pos, size=length))

    schema = pa.schema([
        pa.field("name", pa.string(), nullable=False),
        blob_field("payload_ref"),
    ])
    table = pa.table({"name": names, "payload_ref": blob_array(blobs)}, schema=schema)

    arrow_path = out_dir / "demo.arrow"
    with arrow_path.open("wb") as sink:
        with ipc.new_stream(sink, schema) as writer:
            writer.write_table(table)

    print(f"wrote {arrow_path} ({table.num_rows} interleaved rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
