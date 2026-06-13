#!/usr/bin/env python3
"""Dataset-stitch equivalence: stitching N contiguous packet slices of a capture must reproduce, bit-exact,
the single dataset built from the whole range.

Drives pcapng2lance's --drop/--count packet slicing to cut the same capture into three contiguous slices
([0,N), [N,2N), [2N,3N)) as free-standing datasets, plus one full dataset of [0,3N). Because --drop keeps
the GLOBAL packet_id (a skipped packet still advances the id) and payload offsets are absolute, the slices
are exact pieces of the full run. nlance-stitch merges the three (Option A: relocate fragments + one
manifest, no re-encode), and the stitched dataset, read back and dumped via nlance2table, must equal the
full dataset row-for-row. Also checks packet_id is the contiguous 0..3N-1 after stitching.

argv: <pcapng2lance> <nlance_stitch> <nlance2table> <fixture.pcapng>"""

import subprocess
import sys
import tempfile
from pathlib import Path

N = 70  # per-slice packet count; 3N must be <= packets in the fixture (SRL_front_left_51_short has 224)


def run(*cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"command failed ({r.returncode}): {' '.join(str(c) for c in cmd)}\n{r.stderr}", file=sys.stderr)
        raise SystemExit(1)
    return r


def dump(n2t, dataset, out):
    run(n2t, dataset, "-f", "csv", "-o", out)
    return Path(out).read_text()


def main(argv):
    if len(argv) < 5:
        print("usage: test_pcapng2lance_stitch_equiv.py <pcapng2lance> <nlance_stitch> <nlance2table> <fixture>",
              file=sys.stderr)
        return 2
    conv, stitch, n2t, fixture = argv[1], argv[2], argv[3], Path(argv[4])
    if not fixture.exists():
        print(f"fixture not found: {fixture}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        parts = t / "parts"
        parts.mkdir()

        # three contiguous slices as free-standing datasets, named for nlance-stitch's <prefix>*_<item> order
        for k in range(3):
            run(conv, fixture, parts / f"results_{k}.lance", "-d", k * N, "-c", N)
        # the reference: one dataset spanning the whole [0, 3N) range
        run(conv, fixture, t / "full.lance", "-d", 0, "-c", 3 * N)

        # stitch the three slices (moves fragments + writes one manifest; consumes the source folders)
        run(stitch, parts, t / "stitched.lance", "--prefix", "results_")

        stitched = dump(n2t, t / "stitched.lance", t / "stitched.csv")
        full = dump(n2t, t / "full.lance", t / "full.csv")

        if stitched != full:
            sl, fl = stitched.splitlines(), full.splitlines()
            print(f"STITCH MISMATCH: stitched {len(sl)} rows vs full {len(fl)} rows", file=sys.stderr)
            for i, (a, b) in enumerate(zip(sl, fl)):
                if a != b:
                    print(f"  first diff at line {i}:\n    stitched: {a}\n    full:     {b}", file=sys.stderr)
                    break
            return 1

        # packet_id is column 0; after stitching it must be the contiguous 0..3N-1, in order
        rows = stitched.splitlines()
        header = rows[0].split(",")
        assert header[0] == "packet_id", f"expected packet_id first, got {header[0]!r}"
        pids = [int(r.split(",", 1)[0]) for r in rows[1:]]
        assert pids == list(range(3 * N)), "packet_id is not the contiguous 0..3N-1 after stitching"

    print(f"pcapng2lance stitch-equiv ok: stitch(3 x {N}) == full({3*N}) bit-exact, packet_id 0..{3*N-1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
