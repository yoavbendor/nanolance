#!/usr/bin/env python3
"""Compare a benchmark matrix run with a stored baseline; fail on a regression.

    python tools/bench_compare.py BASELINE.json NEW.json [--dataset-floor 0.5] [--mean-floor 0.75]
    python tools/bench_compare.py BASELINE_DIR/ NEW.json      # one baseline per machine (CI)

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
    all-cores metrics only on the baseline's machine (the same CPU and core count). On another one
    the all-cores ratios measure the machine: the baseline's 2.9x write mean is 2.0x on every CI
    runner, a 4-core machine too, commit after commit.

BASELINE may be a directory of baselines, one per machine (CI keeps bench/results/ci/, one file per
runner CPU: GitHub's ubuntu-latest pool mixes AMD and Intel CPUs, and even the 1-core ratios differ
between them -- 2.5x vs 3.4x on reads at the same commit). The one whose CPU and core count match
the new run is used. With no match, the correctness and per-type checks still run against the
first baseline, and the means are only reported; the output says how to add that machine.

The floors are loose on purpose: a CI runner is noisy, has another core count, and runs the quick
matrix (a tenth of the rows). What they catch is a real step back -- a fast path gone, a page
decoded whole again -- not a 10% wobble. CI compares with bench/results/ci/, one baseline per runner
CPU, each taken on that runner by the linux-bench workflow (run it by hand with matrix_runs=7 and
commit the MATRIX_JSON line of its log as bench/results/ci/<cpu>-<cores>c.json). When a change moves
performance on purpose, re-record them that way. bench/results/matrix-quick.json is the dev-machine
baseline for local runs: `python tools/bench_matrix.py --quick --runs 3 --out bench/results/matrix-quick.json`.
"""

from __future__ import annotations

import argparse
import json
import math
import os
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


def machine(env):
    return (env.get("cpu"), env.get("cores"))


def pick_baseline(path, new):
    """The baseline to compare `new` with, and whether it was measured on the same machine. `path` is
    a baseline file, or a directory of them (one per machine)."""
    if not os.path.isdir(path):
        with open(path) as f:
            base = json.load(f)
        return base, machine(base.get("environment", {})) == machine(new.get("environment", {})), path
    candidates = []
    for name in sorted(os.listdir(path)):
        if name.endswith(".json"):
            with open(os.path.join(path, name)) as f:
                candidates.append((os.path.join(path, name), json.load(f)))
    if not candidates:
        raise SystemExit(f"no baseline (*.json) in {path}")
    for file, base in candidates:
        if machine(base.get("environment", {})) == machine(new.get("environment", {})):
            return base, True, file
    return candidates[0][1], False, candidates[0][0]


def compare(base, new, dataset_floor, mean_floor, min_ms=0.0, same_machine=None):
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
    # The same machine, as far as the environment says: all-cores ratios depend on the cores and on
    # how they share memory -- a 4-core CI runner reads 2.0x on the write mean where the 4-core
    # machine the baseline came from reads 2.9x, run after run.
    be, ne = base.get("environment", {}), new.get("environment", {})
    same_cores = machine(be) == machine(ne)
    if same_machine is None:
        same_machine = True  # a single baseline file: its 1-core means are held to the floor as before
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
        if (metric.endswith("all cores") and not same_cores) or not same_machine:
            lines[-1] += "  (informational: another machine than the baseline's)"
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
    with open(args.new) as f:
        new = json.load(f)
    base, same, file = pick_baseline(args.baseline, new)
    cpu, cores = machine(new.get("environment", {}))
    print(f"this run: {cpu}, {cores} cores; baseline: {file}")
    if os.path.isdir(args.baseline) and not same:
        print(f"  no baseline for this machine in {args.baseline}: means are informational. To add one, run")
        print("  linux-bench by hand (matrix_runs=7) on such a runner and commit its MATRIX_JSON there.")
    problems, lines = compare(base, new, args.dataset_floor, args.mean_floor, args.min_ms,
                              same_machine=same if os.path.isdir(args.baseline) else None)
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
