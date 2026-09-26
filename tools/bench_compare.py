#!/usr/bin/env python3
"""Compare a benchmark matrix run with a stored baseline; fail on a regression.

    python tools/bench_compare.py BASELINE.json NEW.json [--dataset-floor 0.5] [--mean-floor 0.75]

Times differ from machine to machine, so what is compared is each result as a ratio to Rust Lance
(pylance) measured in the same run: Rust's time over nanolance's, for reads and writes, with both on
one core and both on all cores. A regression is

  * any read that returned wrong data or failed, or a cross read (each writer's file read by the
    other side) that is missing -- correctness, no tolerance;
  * a data type whose read (or write) ratio fell below --dataset-floor x its baseline (default 0.5:
    twice as slow relative to Rust as it was) both on one core and on all cores -- for operations
    that take at least --min-ms (default 2 ms) for nanolance: below that, at the quick scale, a
    shared runner's scheduling is most of the time (a 0.3 ms write fell 3x on every CI run with no
    code change);
  * a geometric mean over data types below --mean-floor x its baseline (default 0.75), for the
    all-cores metrics only when the machine has the baseline's core count (on another one, the
    all-cores ratios measure the machine: the 4-core baseline's 2.9x write mean is 2.0x on every
    CI runner, commit after commit).

The floors are loose on purpose: a CI runner is noisy, has another core count, and runs the quick
matrix (a tenth of the rows). What they catch is a real step back -- a fast path gone, a page
decoded whole again -- not a 10% wobble. bench/results/matrix-quick.json is the baseline CI uses:
regenerate it with `python tools/bench_matrix.py --quick --runs 3 --out bench/results/matrix-quick.json`
when a change moves performance on purpose.
"""

from __future__ import annotations

import argparse
import json
import math
import sys

METRICS = {
    # name: (what, Rust's key, nanolance's key)
    "read, 1 core": ("read_ms", "rust-lance-1c <- rust-lance", "nanolance-cpp <- nanolance"),
    "read, all cores": ("read_ms", "rust-lance <- rust-lance", "nanolance-cpp-mt <- nanolance"),
    "write, 1 core": ("write_ms", "rust-lance-1c", "nanolance-cpp"),
    "write, all cores": ("write_ms", "rust-lance", "nanolance-cpp-mt"),
}
CROSS = ["nanolance-cpp <- rust-lance", "nanolance-py <- rust-lance", "rust-lance <- nanolance"]


def ratio(rec, what, rust, nl):
    a, b = rec.get(what, {}).get(rust), rec.get(what, {}).get(nl)
    return a / b if a and b else None


def geomean(xs):
    xs = [x for x in xs if x]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else None


def nl_ms(rec, what, nl):
    return rec.get(what, {}).get(nl) or 0.0


def compare(base, new, dataset_floor, mean_floor, min_ms=0.0):
    problems, lines = [], []
    B, N = base["datasets"], new["datasets"]
    for name, rec in N.items():
        for key, why in rec.get("errors", {}).items():
            problems.append(f"{name}: {key}: {why}")
        for key in CROSS:
            if rec.get("read_ms", {}).get(key) is None:
                problems.append(f"{name}: cross read '{key}' missing")
    # A data type regresses when an operation fell below the floor on one core AND on all cores: a
    # real step back (a fast path gone) shows in both, while Rust's own all-cores timings at the quick
    # scale swing by 2-3x between identical runs.
    for op in ("read", "write"):
        for name in N:
            if name not in B:
                continue
            fell = []
            for metric, (what, rust, nl) in METRICS.items():
                if not metric.startswith(op):
                    continue
                b, n = ratio(B[name], what, rust, nl), ratio(N[name], what, rust, nl)
                if min(nl_ms(B[name], what, nl), nl_ms(N[name], what, nl)) < min_ms:
                    continue  # too short to time on a shared machine
                if b is not None and n is not None:
                    fell.append((metric, b, n, n < dataset_floor * b))
            if fell and all(f[3] for f in fell):
                detail = ", ".join(f"{m} {n:.2f}x (baseline {b:.2f}x)" for m, b, n, _ in fell)
                problems.append(f"{name}: {op} fell below {dataset_floor}x its baseline everywhere: {detail}")
    same_cores = base.get("environment", {}).get("cores") == new.get("environment", {}).get("cores")
    for metric, (what, rust, nl) in METRICS.items():
        pairs = []
        for name in N:
            if name not in B:
                continue
            b, n = ratio(B[name], what, rust, nl), ratio(N[name], what, rust, nl)
            if b is not None and n is not None:
                pairs.append((b, n))
        if not pairs:
            continue
        gb, gn = geomean([b for b, _ in pairs]), geomean([n for _, n in pairs])
        lines.append(f"{metric:18s} baseline {gb:5.2f}x  now {gn:5.2f}x  ({len(pairs)} data types)")
        if metric.endswith("all cores") and not same_cores:
            lines[-1] += "  (informational: another core count than the baseline's)"
        elif gn < mean_floor * gb:
            problems.append(f"{metric}: mean {gn:.2f}x Rust, baseline {gb:.2f}x (floor {mean_floor * gb:.2f}x)")
    return problems, lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("baseline")
    ap.add_argument("new")
    ap.add_argument("--dataset-floor", type=float, default=0.5)
    ap.add_argument("--mean-floor", type=float, default=0.75)
    ap.add_argument("--min-ms", type=float, default=2.0)
    args = ap.parse_args(argv)
    with open(args.baseline) as f:
        base = json.load(f)
    with open(args.new) as f:
        new = json.load(f)
    problems, lines = compare(base, new, args.dataset_floor, args.mean_floor, args.min_ms)
    print("nanolance vs Rust Lance (Rust's time / nanolance's; above 1x nanolance is faster):")
    for line in lines:
        print("  " + line)
    if problems:
        print(f"\n{len(problems)} regression(s):")
        for p in problems:
            print("  - " + p)
        return 1
    print("\nno regression")
    return 0


if __name__ == "__main__":
    sys.exit(main())
