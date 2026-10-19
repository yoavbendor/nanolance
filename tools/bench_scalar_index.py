#!/usr/bin/env python3
"""Scalar indexes: nanolance's C++ library against Rust Lance (the lance crate, 12.0.0, no Python in the process).

    python tools/bench_scalar_index.py [--rows 5000000] [--min-seconds 10] [--work DIR]
                                     # writes bench/results/scalar_index.json

The data is generated (seeded): `id` (a permutation of 0..n), `f` (normal floats), `s` (user ids,
~1M distinct strings), `cat` (50 values), `tags` (two of 200 labels per row). pylance writes the base
dataset in fragments of 1M rows; that is data generation, not timed. Each engine builds BTREE on id, f and s,
BITMAP on cat and LABEL_LIST on tags on its own copy (the build is timed on fresh copies of the unindexed
data), then queries the dataset whose indexes it built, returning one column or all of them. Every query's
row count must agree between the two engines before its times are reported. Samples: a build repeats until
its timed total reaches --min-seconds; a query repeats until its timed total reaches it too.
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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=5_000_000)
    ap.add_argument("--min-seconds", type=float, default=10.0)
    ap.add_argument("--work", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=ROOT / "bench" / "results" / "scalar_index.json")
    args = ap.parse_args()
    warnings.filterwarnings("ignore")
    check_drivers()

    work = Path(args.work or tempfile.mkdtemp(prefix="nl_si_"))
    work.mkdir(parents=True, exist_ok=True)
    base = work / "scalar_base.lance"
    if not base.exists():
        import lance  # data generation only
        lance.write_dataset(table(args.rows), str(base), max_rows_per_file=1_000_000)

    lines = [f"scalar_build\t{column}\t{kind}" for column, kind in BUILDS]
    for columns in (None, ["id"]):
        for q in QUERIES:
            name = f"{'all' if columns is None else ','.join(columns)}|{q}"
            lines.append(f"scalar_query\t{name}\t{q}\t{'*' if columns is None else ','.join(columns)}")
    workload = write_workload(work / "scalar.txt", lines, args.min_seconds)

    rust = run_driver(RUST_CMD, "scalar", workload, work / "rust", base, work / "rust_scalar.json")["scalar"]
    cpp = run_driver(CPP_CMD, "scalar", workload, work / "cpp", base, work / "cpp_scalar.json")["scalar"]

    queries = {}
    for name, rec in cpp["queries"].items():
        if rust["queries"][name]["rows"] != rec["rows"]:
            raise SystemExit(f"{name}: nanolance returned {rec['rows']} rows, Rust Lance {rust['queries'][name]['rows']}")
        queries[name] = {"rows": rec["rows"], **combine_stats(rust["queries"][name], rec)}
    builds = {name: combine_stats(rust["builds"][name], rec) for name, rec in cpp["builds"].items()}

    out = {
        "environment": environment("scalar"),
        "rows": args.rows, "fragments": (args.rows + 999_999) // 1_000_000, "min_seconds": args.min_seconds,
        "builds": builds, "queries": queries,
    }
    args.out.write_text(json.dumps(out, indent=1))
    print(f"wrote {args.out}")
    if args.work is None:
        shutil.rmtree(work, ignore_errors=True)  # the copies are large


if __name__ == "__main__":
    main()
