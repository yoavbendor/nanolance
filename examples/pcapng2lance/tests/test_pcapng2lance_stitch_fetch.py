#!/usr/bin/env python3
"""Stitch + external-blob fetch: a stitched dataset's payload references must still resolve to real bytes.

The whole point of nanolance is that a row keeps its payload OUT of the table (a lance.blob.v2 external
ref: uri + position + size). nlance-stitch relocates fragment *data files*; it must not break those external
refs. This test stitches 3 packet slices, then for every row follows payload_ref -> reads `size` bytes at
`position` from the `uri` file -> and checks (a) the read succeeds and is exactly `size` bytes, and (b) the
bytes are identical to what the same packet_id resolves to in the single full dataset. So the stitched
dataset is independently usable for blob retrieval, byte-for-byte.

argv: <pcapng2lance> <nlance_stitch> <nlance2table> <fixture.pcapng>"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import urlparse
from urllib.request import url2pathname

N = 60


def run(*cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"command failed ({r.returncode}): {' '.join(str(c) for c in cmd)}\n{r.stderr}", file=sys.stderr)
        raise SystemExit(1)
    return r


def rows_by_pid(n2t, dataset):
    nd = run(n2t, dataset, "-f", "ndjson").stdout
    return {o["packet_id"]: o for o in (json.loads(ln) for ln in nd.splitlines() if ln)}


def fetch(ref):
    """Follow a payload_ref {uri, position, size} -> the bytes on disk."""
    path = url2pathname(urlparse(ref["uri"]).path)
    with open(path, "rb") as f:
        f.seek(int(ref["position"]))
        return f.read(int(ref["size"]))


def main(argv):
    if len(argv) < 5:
        print("usage: test_pcapng2lance_stitch_fetch.py <pcapng2lance> <nlance_stitch> <nlance2table> <fixture>",
              file=sys.stderr)
        return 2
    conv, stitch, n2t, fixture = argv[1], argv[2], argv[3], Path(argv[4])
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

        stitched = rows_by_pid(n2t, t / "stitched.lance")
        full = rows_by_pid(n2t, t / "full.lance")
        assert set(stitched) == set(full) == set(range(3 * N)), "packet_id set mismatch"

        total_bytes = 0
        for pid in range(3 * N):
            ref = stitched[pid]["payload_ref"]
            data = fetch(ref)
            if len(data) != int(ref["size"]):
                print(f"pkt{pid}: short read {len(data)} != size {ref['size']}", file=sys.stderr)
                return 1
            if data != fetch(full[pid]["payload_ref"]):
                print(f"pkt{pid}: stitched payload bytes != full payload bytes", file=sys.stderr)
                return 1
            total_bytes += len(data)

    print(f"pcapng2lance stitch-fetch ok: followed {3*N} external payload refs ({total_bytes} bytes) through "
          f"the stitched dataset; all resolve and match the full dataset")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
