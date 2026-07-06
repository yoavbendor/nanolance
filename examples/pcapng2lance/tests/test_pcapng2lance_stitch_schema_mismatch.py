#!/usr/bin/env python3
"""Stitch negative: merging datasets with DIFFERENT schemas must be rejected, not silently corrupt.

nlance-stitch's precondition is that every source has an identical schema (Option A relocates data files
under one manifest — incompatible schemas would yield an unreadable dataset). This builds two datasets with
deliberately different schemas — a pcapng2lance L1 packets table and an arrowipc2lance (id:int64,
score:float64) table — names them as stitch inputs, and asserts the stitch FAILS (non-zero exit) rather
than producing a bad master.

Skips (77) if pyarrow is not installed (needed to synthesize the second, different-schema dataset).

argv: <pcapng2lance> <nlance_stitch> <arrowipc2lance> <fixture.pcapng>"""

import subprocess
import sys
import tempfile
from pathlib import Path


def run(*cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def run_ok(*cmd, **kw):
    r = run(*cmd, **kw)
    if r.returncode != 0:
        print(f"setup command failed ({r.returncode}): {' '.join(str(c) for c in cmd)}\n{r.stderr}", file=sys.stderr)
        raise SystemExit(1)
    return r


def main(argv):
    if len(argv) < 5:
        print("usage: test_pcapng2lance_stitch_schema_mismatch.py <pcapng2lance> <nlance_stitch> "
              "<arrowipc2lance> <fixture>", file=sys.stderr)
        return 2
    try:
        import pyarrow as pa
        import pyarrow.ipc as ipc
    except ImportError:
        print("stitch schema-mismatch skipped: pyarrow not installed", file=sys.stderr)
        return 77

    conv, stitch, arrowipc, fixture = argv[1], argv[2], argv[3], Path(argv[4])
    if not fixture.exists():
        print(f"fixture not found: {fixture}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        parts = t / "parts"; parts.mkdir()

        # results_0: a normal pcapng2lance L1 packets table.
        run_ok(conv, fixture, parts / "results_0.lance", "-c", 10)

        # results_1: a totally different schema (id:int64, score:float64) via arrowipc2lance.
        ipc_path = t / "input.arrow"
        schema = pa.schema([pa.field("id", pa.int64(), nullable=False),
                            pa.field("score", pa.float64(), nullable=False)])
        table = pa.Table.from_arrays(
            [pa.array([1, 2, 3], type=pa.int64()), pa.array([1.0, 2.0, 3.0], type=pa.float64())], schema=schema)
        with open(ipc_path, "wb") as sink, ipc.new_stream(sink, schema) as w:
            w.write_table(table)
        with open(ipc_path, "rb") as stdin:
            run_ok(arrowipc, "-o", str(parts / "results_1.lance"), "-c", "-l", "3", stdin=stdin)

        # Stitching the two incompatible schemas must FAIL.
        r = run(stitch, parts, t / "master.lance", "--prefix", "results_")
        if r.returncode == 0:
            print("schema-mismatch: nlance-stitch SUCCEEDED on incompatible schemas (should have failed)",
                  file=sys.stderr)
            return 1

    print("pcapng2lance stitch schema-mismatch ok: nlance-stitch rejected datasets with different schemas")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
