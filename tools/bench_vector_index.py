#!/usr/bin/env python3
"""Vector indexes: nanolance against Rust Lance (pylance), building IVF_FLAT / IVF_PQ / IVF_HNSW_SQ and searching.

    python tools/bench_vector_index.py [--rows 200000] [--dim 128] [--runs 5] [--work DIR]
                                       # writes bench/results/vector_index.json

The data is generated (seeded): `dim`-dimensional float32 vectors around 1,000 random centres,
written by pylance. Each engine builds each index (256 partitions; IVF_PQ with dim / 8 sub-vectors)
on its own copy of the unindexed data, then searches the index it built: 50 queries near data
points, Lance's default probing and fixed nprobes. Before timing, every search on the index pylance
built must return what pylance returns (rows, order, distances), and recall@10 against an exact
search is measured for both engines' indexes. Search times are the median of --runs passes over the
50 queries (warm, files in the page cache), per query; builds run once.
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
SEARCHES = [("k=10", {"k": 10}), ("k=100", {"k": 100}), ("k=10, nprobes=32", {"k": 10, "nprobes": 32}),
            ("k=10, refine_factor=4", {"k": 10, "refine_factor": 4})]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=200_000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--partitions", type=int, default=256)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--work", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=ROOT / "bench" / "results" / "vector_index.json")
    args = ap.parse_args()
    warnings.filterwarnings("ignore")
    import lance
    import nanolance.lance as nl

    rng = np.random.default_rng(0)
    centres = rng.standard_normal((1000, args.dim)).astype(np.float32) * 2
    v = centres[rng.integers(0, 1000, args.rows)] + rng.standard_normal((args.rows, args.dim)).astype(np.float32)
    queries = v[rng.integers(0, args.rows, 50)] + rng.standard_normal((50, args.dim)).astype(np.float32) * 0.3
    truth = [set(np.argsort(((v - q) ** 2).sum(1))[:10].tolist()) for q in queries]

    work = Path(args.work or tempfile.mkdtemp(prefix="nl_vi_"))
    base = work / "base.lance"
    if not base.exists():
        lance.write_dataset(pa.table({"id": pa.array(np.arange(args.rows)), "vec": pa.FixedSizeListArray.from_arrays(
            pa.array(v.ravel()), args.dim)}), str(base))

    results = {}
    for kind, kw in (("IVF_FLAT", {}), ("IVF_PQ", {"num_sub_vectors": args.dim // 8}), ("IVF_HNSW_SQ", {})):
        rec = {"build_s": {}, "recall@10": {}, "search_ms": {}}
        paths = {}
        for who, mod in (("pylance", lance), ("nanolance", nl)):
            path = work / f"{who}_{kind}.lance"
            shutil.rmtree(path, ignore_errors=True)
            shutil.copytree(base, path)
            paths[who] = str(path)
            t0 = time.perf_counter()
            mod.dataset(str(path)).create_index("vec", kind, num_partitions=args.partitions, **kw)
            rec["build_s"][who] = time.perf_counter() - t0
            ds = lance.dataset(str(path))
            rec["recall@10"][who] = float(np.mean([
                len(set(ds.to_table(nearest={"column": "vec", "q": q, "k": 10}, columns=["id"]).column("id").to_pylist())
                    & t) / 10 for q, t in zip(queries, truth)]))
        # nanolance answers as pylance does on pylance's index.
        theirs, ours = lance.dataset(paths["pylance"]), nl.dataset(paths["pylance"])
        for name, extra in SEARCHES:
            for q in queries[:10]:
                nearest = {"column": "vec", "q": q, **extra}
                a = theirs.to_table(nearest=nearest, columns=["id"])
                b = ours.to_table(nearest=nearest, columns=["id"])
                if a.column("id").to_pylist() != b.column("id").to_pylist() or not np.allclose(
                        a.column("_distance").to_numpy(), b.column("_distance").to_numpy(), rtol=1e-4, atol=1e-4):
                    raise SystemExit(f"nanolance answered {kind} {name} otherwise than pylance")
        for name, extra in SEARCHES:
            row = {}
            for who, mod in (("pylance", lance), ("nanolance", nl)):
                ds = mod.dataset(paths[who])
                run = lambda: [ds.to_table(nearest={"column": "vec", "q": q, **extra}, columns=["id"]) for q in queries]
                run()
                xs = []
                for _ in range(args.runs):
                    t0 = time.perf_counter()
                    run()
                    xs.append((time.perf_counter() - t0) / len(queries))
                row[who] = statistics.median(xs) * 1000
            rec["search_ms"][name] = row
        results[kind] = rec
        print(kind, json.dumps(rec))

    commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True,
                            text=True).stdout.strip()
    if subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"],
                      capture_output=True, text=True).stdout.strip():
        commit += " with uncommitted changes"
    out = {
        "environment": {"pylance": lance.__version__, "nanolance_commit": commit, "cores": os.cpu_count(),
                        "machine": platform.processor() or platform.machine(),
                        "date": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")},
        "rows": args.rows, "dim": args.dim, "partitions": args.partitions, "runs": args.runs, "indexes": results,
    }
    args.out.write_text(json.dumps(out, indent=1))
    print(f"wrote {args.out}")
    if args.work is None:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
