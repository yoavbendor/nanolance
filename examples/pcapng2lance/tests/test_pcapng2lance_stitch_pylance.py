#!/usr/bin/env python3
"""Stitch + stock-Lance interop: a stitched dataset must be readable by upstream `lance` (pylance), not just
our own reader, and return the same rows as the single full dataset.

nlance-stitch writes a plain multi-fragment Lance manifest (Option A). This opens the stitched dataset and
the full dataset with the real `lance` Python package and asserts the read-back tables are equal — proving
the stitched manifest is valid for the broader Lance ecosystem.

Skips (77) if pylance is not installed.

argv: <pcapng2lance> <nlance_stitch> <fixture.pcapng>"""

import subprocess
import sys
import tempfile
from pathlib import Path

N = 60


def run(*cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"command failed ({r.returncode}): {' '.join(str(c) for c in cmd)}\n{r.stderr}", file=sys.stderr)
        raise SystemExit(1)
    return r


def main(argv):
    if len(argv) < 4:
        print("usage: test_pcapng2lance_stitch_pylance.py <pcapng2lance> <nlance_stitch> <fixture>", file=sys.stderr)
        return 2
    try:
        import lance  # noqa: F401
    except ImportError:
        print("stitch/pylance interop skipped: pylance not installed", file=sys.stderr)
        return 77

    conv, stitch, fixture = argv[1], argv[2], Path(argv[3])
    if not fixture.exists():
        print(f"fixture not found: {fixture}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        parts = t / "parts"; parts.mkdir()
        for k in range(3):
            run(conv, fixture, parts / f"results_{k}.lance", "-d", k * N, "-c", N)
        run(conv, fixture, t / "full.lance", "-d", 0, "-c", 3 * N)
        run(stitch, parts, t / "stitched.lance", "--prefix", "results_")

        import lance
        stitched = lance.dataset(str(t / "stitched.lance")).to_table()
        full = lance.dataset(str(t / "full.lance")).to_table()

        if stitched.num_rows != full.num_rows:
            print(f"row count: stitched {stitched.num_rows} vs full {full.num_rows}", file=sys.stderr)
            return 1
        if stitched.schema != full.schema:
            print(f"schema mismatch:\n stitched={stitched.schema}\n full={full.schema}", file=sys.stderr)
            return 1
        if stitched.to_pylist() != full.to_pylist():
            print("stitched rows != full rows (read via pylance)", file=sys.stderr)
            return 1

    print(f"pcapng2lance stitch/pylance ok: pylance reads the stitched dataset ({full.num_rows} rows) "
          f"identically to the full dataset")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
