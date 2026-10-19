#!/usr/bin/env python3
"""Vector indexes: nanolance's C++ library against Rust Lance (the lance crate, 12.0.0, no Python in the process).

    python tools/bench_vector_index.py [--rows 200000] [--dim 128] [--min-seconds 10] [--builds 5] [--work DIR]
                                       # writes bench/results/vector_index.json

The data is generated (seeded): `dim`-dimensional float32 vectors around 1,000 random centres, written
by pylance as one fragment (data generation, not timed). Each engine builds each index (256 partitions;
IVF_PQ with dim / 8 sub-vectors) --builds times, each on its own copy of the unindexed data: the training is
random, so every build is a different index. Recall@10 against an exact search is measured on every build
and averaged (the spread is recorded too), and each search's time is pooled over the builds (the passes go
round-robin over them). Before the times are reported, the row addresses must be row ids (one fragment, so
the address is the offset), or the run stops. tools/perf_gate.py fails a run where nanolance's mean recall
falls more than 0.03 below Rust Lance's.
"""

from __future__ import annotations

import argparse
import json
import shutil
import tempfile
import warnings
from pathlib import Path

import numpy as np
import pyarrow as pa

from index_bench_common import (CPP_CMD, RUST_CMD, check_drivers, combine_stats, environment, run_driver,
                                write_workload)

ROOT = Path(__file__).resolve().parents[1]
KINDS = ["IVF_FLAT", "IVF_PQ", "IVF_HNSW_SQ"]
SEARCHES = [("k=10", 10, None, None), ("k=100", 100, None, None), ("k=10, nprobes=32", 10, 32, None),
            ("k=10, refine_factor=4", 10, None, 4)]


def recall(hits: list[list[int]], truth: list[set[int]]) -> float:
    return float(np.mean([len(set(h[:10]) & t) / 10 for h, t in zip(hits, truth)]))


def recalls_by_build(hits_by_build: list, truth: list[set[int]]) -> list[float]:
    """Recall@10 of each build's answers: the drivers return one list of query answers per build."""
    return [recall(hits, truth) for hits in hits_by_build]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=200_000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--partitions", type=int, default=256)
    ap.add_argument("--min-seconds", type=float, default=10.0)
    ap.add_argument("--builds", type=int, default=5,
                    help="builds per index kind, each on its own copy: the training is random, so the recall and "
                         "the search times are averaged over them")
    ap.add_argument("--work", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=ROOT / "bench" / "results" / "vector_index.json")
    args = ap.parse_args()
    warnings.filterwarnings("ignore")
    check_drivers()

    rng = np.random.default_rng(0)
    centres = rng.standard_normal((1000, args.dim)).astype(np.float32) * 2
    v = centres[rng.integers(0, 1000, args.rows)] + rng.standard_normal((args.rows, args.dim)).astype(np.float32)
    queries = v[rng.integers(0, args.rows, 50)] + rng.standard_normal((50, args.dim)).astype(np.float32) * 0.3
    truth = [set(np.argsort(((v - q) ** 2).sum(1))[:10].tolist()) for q in queries]

    work = Path(args.work or tempfile.mkdtemp(prefix="nl_vi_"))
    work.mkdir(parents=True, exist_ok=True)
    base = work / "vector_base.lance"
    if not base.exists():
        import lance  # data generation only
        lance.write_dataset(pa.table({"id": pa.array(np.arange(args.rows)), "vec": pa.FixedSizeListArray.from_arrays(
            pa.array(v.ravel()), args.dim)}), str(base))

    lines = [f"vector_config\tvec\t{args.partitions}\t{args.dim}\t{args.builds}"]
    lines += [f"vector_kind\t{kind}\t{kind}" for kind in KINDS]
    lines += [f"vector_query\t" + ",".join(f"{x:.7g}" for x in q) for q in queries]
    for name, k, nprobes, refine in SEARCHES:
        lines.append(f"vector_search\t{name}\t{k}\t{nprobes or 0}\t{refine or 0}")
    workload = write_workload(work / "vector.txt", lines, args.min_seconds)

    rust = run_driver(RUST_CMD, "vector", workload, work / "rust", base, work / "rust_vector.json")["vector"]
    cpp = run_driver(CPP_CMD, "vector", workload, work / "cpp", base, work / "cpp_vector.json")["vector"]

    builds = {kind: combine_stats(rust["builds"][kind], cpp["builds"][kind]) for kind in KINDS}
    searches, recalls = {}, {}
    for key, rust_stat in rust["searches"].items():
        cpp_hits = cpp["hits"][key]
        if any(addr >= 2**32 for build in cpp_hits for hits in build for addr in hits):
            raise SystemExit(f"{key}: nanolance returned a row address outside fragment 0")
        searches[key] = combine_stats(rust_stat, cpp["searches"][key])
    for kind in KINDS:
        # Recall@10 of the default search against the exact answers, per build, averaged over the builds.
        per = {}
        for engine, result in (("rust", rust), ("nanolance", cpp)):
            values = recalls_by_build(result["hits"]["%s|k=10" % kind], truth)
            per[engine] = values
        r_rust, r_cpp = float(np.mean(per["rust"])), float(np.mean(per["nanolance"]))
        recalls[kind] = {
            "rust": r_rust, "nanolance": r_cpp,
            "rust_std": float(np.std(per["rust"], ddof=1)), "nanolance_std": float(np.std(per["nanolance"], ddof=1)),
            "builds": args.builds,
        }
        if r_cpp < r_rust - 0.03:
            # Recorded, not fatal: the performance gate (tools/perf_gate.py) fails on it.
            print(f"warning: {kind} nanolance recall@10 {r_cpp:.3f} is below Rust Lance's {r_rust:.3f} "
                  f"(mean of {args.builds} builds)")

    out = {
        "environment": environment("vector"),
        "rows": args.rows, "dim": args.dim, "partitions": args.partitions, "min_seconds": args.min_seconds,
        "build_count": args.builds,
        "builds": builds, "searches": searches, "recall@10": recalls,
    }
    args.out.write_text(json.dumps(out, indent=1))
    print(f"wrote {args.out}")
    if args.work is None:
        shutil.rmtree(work, ignore_errors=True)  # a temporary copy of every dataset: hundreds of MB


if __name__ == "__main__":
    main()
