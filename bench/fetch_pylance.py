#!/usr/bin/env python3
"""pylance side of the blob-fetch benchmark: open a lance.blob.v2 dataset and fetch each external blob via
the native `take_blobs()` API (the Rust core, with on-demand S3/file range reads), MD5 each payload, and
print one line per blob: `index <TAB> md5 <TAB> fetch_nanos`. Only the `.read()` (the actual fetch) is
timed, so the number is comparable to nanolance's nano_lance_fetch_external_blob. Same output format as
nlance_blobfetch, so the orchestrator can diff md5 columns and aggregate times.

usage: fetch_pylance.py <dataset.lance> [--indices i,j,k]   (default: all rows)
Exit 77 if pylance is unavailable (so CI treats it as skipped)."""

import argparse
import hashlib
import sys
import time


def main(argv) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dataset")
    ap.add_argument("--indices", default="")
    ap.add_argument("--column", default="payload_ref")
    args = ap.parse_args(argv[1:])

    try:
        import lance  # noqa: F401
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"pylance fetch skipped: {exc}", file=sys.stderr)
        return 77

    ds = lance.dataset(args.dataset)
    n = ds.count_rows()
    if args.indices:
        indices = [int(x) for x in args.indices.split(",") if x != ""]
    else:
        indices = list(range(n))

    # take_blobs returns lazy BlobFile handles; the fetch happens on .read(). Time each .read() only.
    blobs = ds.take_blobs(args.column, indices=indices)
    for idx, bf in zip(indices, blobs):
        t0 = time.perf_counter_ns()
        with bf as f:
            data = f.read()
        nanos = time.perf_counter_ns() - t0
        md5 = hashlib.md5(data).hexdigest()
        print(f"{idx}\t{md5}\t{nanos}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
