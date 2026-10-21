#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write the same table to S3 with nanolance and with pylance and print the best of three times.

    python3 bench/s3_upload_speed.py s3://bucket/scratch/prefix [--endpoint URL --allow-http] [--sizes 16,128,512]

Credentials come from the AWS environment (AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_REGION). Each
write goes to a fresh sub-prefix, which is left behind (point it at a scratch prefix). On moto, 2026-10-21,
512 MiB: nanolance 4.7 s, pylance 4.3 s (8 parts in flight; with 4 it was 6.7 s).
"""

import argparse
import os
import time
import uuid

import numpy as np
import pyarrow as pa

import nanolance.lance as nl


def table(mib: int) -> pa.Table:
    """id, a 16-float vector and 128 random bytes per row: about 200 bytes a row, incompressible."""
    rng = np.random.default_rng(0)
    rows = mib * 1024 * 1024 // 200
    vectors = pa.array(rng.standard_normal(rows * 16).astype(np.float32))
    return pa.table({"id": pa.array(np.arange(rows), pa.int64()),
                     "v": pa.FixedSizeListArray.from_arrays(vectors, 16),
                     "b": pa.array([os.urandom(128) for _ in range(rows)], pa.binary())})


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix")
    ap.add_argument("--endpoint")
    ap.add_argument("--region")
    ap.add_argument("--allow-http", action="store_true")
    ap.add_argument("--sizes", default="16,128,512", help="MiB per write, comma separated")
    args = ap.parse_args()
    options = {}
    if args.endpoint:
        options["endpoint"] = args.endpoint
    if args.region:
        options["region"] = args.region
    if args.allow_http:
        options["allow_http"] = "true"
    engines = [("nanolance", nl)]
    try:
        import lance

        engines.append(("pylance", lance))
    except ImportError:
        print("pylance not installed: nanolance only")
    for mib in (int(s) for s in args.sizes.split(",")):
        t = table(mib)
        best = {}
        for name, engine in engines:
            best[name] = min(_timed(engine, t, f"{args.prefix.rstrip('/')}/{name}-{uuid.uuid4().hex}.lance", options)
                             for _ in range(3))
        line = "  ".join(f"{name} {s:.2f} s ({mib / s:.0f} MiB/s)" for name, s in best.items())
        print(f"{mib:5d} MiB  {line}", flush=True)


def _timed(engine, t: pa.Table, uri: str, options: dict) -> float:
    start = time.perf_counter()
    engine.write_dataset(t, uri, storage_options=options)
    return time.perf_counter() - start


if __name__ == "__main__":
    main()
