#!/usr/bin/env python3
"""Scalar indexes: nanolance against Rust Lance (pylance), building them and querying with them.

    python tools/bench_scalar_index.py [--rows 5000000] [--runs 5] [--work DIR]
                                     # writes bench/results/scalar_index.json

The data is generated (seeded): `id` (a permutation of 0..n), `f` (normal floats), `s` (user ids,
~1M distinct strings), `cat` (50 values), `tags` (two of 200 labels per row), written by pylance in
fragments of 1M rows. Each engine builds BTREE on id, f and s, BITMAP on cat and LABEL_LIST on tags
on its own copy; then each engine queries the dataset whose indexes it built, returning one column or
all of them, and nanolance also without its index. Every query's rows are checked against pylance's
before it is timed. Median of --runs runs after a warm-up, files in the page cache.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import tempfile
import time
import warnings
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import pyarrow as pa

ROOT = Path(__file__).resolve().parents[1]

BUILDS = [("id", "BTREE"), ("f", "BTREE"), ("s", "BTREE"), ("cat", "BITMAP"), ("tags", "LABEL_LIST")]
QUERIES = [
    "id = 123456", "id < 1000", "id BETWEEN 1000000 AND 1100000", "s = 'user_00012345'", "cat = 'c7'",
    "array_has_any(tags, ['t5'])", "cat = 'c7' AND id < 50000",
]


def table(n: int) -> pa.Table:
    rng = np.random.default_rng(0)
    return pa.table({
        "id": pa.array(rng.permutation(n)),
        "f": pa.array(rng.standard_normal(n)),
        "s": pa.array([f"user_{x:08d}" for x in rng.integers(0, 1_000_000, n)]),
        "cat": pa.array([f"c{x}" for x in rng.integers(0, 50, n)]),
        "tags": pa.ListArray.from_arrays(pa.array(np.arange(0, 2 * n + 1, 2, dtype=np.int32)),
                                         pa.array([f"t{x}" for x in rng.integers(0, 200, 2 * n)])),
    })


def timed(fn, runs: int):
    fn()
    xs = []
    for _ in range(runs):
        t0 = time.perf_counter()
        fn()
        xs.append(time.perf_counter() - t0)
    return statistics.median(xs) * 1000


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=5_000_000)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--work", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=ROOT / "bench" / "results" / "scalar_index.json")
    args = ap.parse_args()
    warnings.filterwarnings("ignore")
    import lance
    import nanolance.lance as nl

    work = Path(args.work or tempfile.mkdtemp(prefix="nl_si_"))
    base = work / "base.lance"
    if not base.exists():
        lance.write_dataset(table(args.rows), str(base), max_rows_per_file=1_000_000)

    paths = {}
    builds = {}
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        path = work / f"{who}.lance"
        shutil.rmtree(path, ignore_errors=True)
        shutil.copytree(base, path)
        paths[who] = str(path)
        for column, kind in BUILDS:
            # Each build on a fresh copy of the unindexed data, then once more to keep.
            xs = []
            for _ in range(args.runs):
                scratch = work / f"{who}_scratch.lance"
                shutil.rmtree(scratch, ignore_errors=True)
                shutil.copytree(base, scratch)
                ds = mod.dataset(str(scratch))
                t0 = time.perf_counter()
                ds.create_scalar_index(column, kind)
                xs.append(time.perf_counter() - t0)
            builds.setdefault(f"{column} {kind}", {})[who] = statistics.median(xs) * 1000
            mod.dataset(str(path)).create_scalar_index(column, kind)
        shutil.rmtree(work / f"{who}_scratch.lance", ignore_errors=True)

    queries = {}
    for columns in (["id"], None):
        for q in QUERIES:
            truth = sorted(lance.dataset(paths["pylance"]).to_table(filter=q, columns=["id"]).column("id").to_pylist())
            rec = {"rows": len(truth)}
            for who, mod, kwargs in (("pylance", lance, {}), ("nanolance", nl, {}),
                                     ("nanolance_no_index", nl, {"use_scalar_index": False})):
                path = paths["pylance" if who == "pylance" else "nanolance"]
                ds = mod.dataset(path)
                got = ds.to_table(filter=q, columns=["id"], **kwargs).column("id").to_pylist()
                if sorted(got) != truth:
                    raise SystemExit(f"{who} returned other rows for {q}")
                rec[who] = timed(lambda: ds.to_table(filter=q, columns=columns, **kwargs), args.runs)
            queries[f"{'all' if columns is None else ','.join(columns)}|{q}"] = rec

    commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True,
                            text=True).stdout.strip()
    if subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"],
                      capture_output=True, text=True).stdout.strip():
        commit += " with uncommitted changes"

    out = {
        "environment": {"pylance": lance.__version__, "nanolance_commit": commit, "cores": os.cpu_count(),
                        "machine": platform.processor() or platform.machine(),
                        "date": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")},
        "rows": args.rows, "fragments": (args.rows + 999_999) // 1_000_000, "runs": args.runs,
        "builds": builds, "queries": queries,
    }
    args.out.write_text(json.dumps(out, indent=1))
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
