#!/usr/bin/env python3
"""Performance gate for the index benchmarks: nanolance against Rust Lance, run in the same job.

    python tools/perf_gate.py --scalar S.json --vector V.json --fts F.json --baseline-dir bench/results/perf
    python tools/perf_gate.py --scalar S.json --vector V.json --fts F.json --record bench/results/perf/CPU.json
    python tools/perf_gate.py ... --skip        # or NANOLANCE_PERF_SKIP=1: a pressed clock, runs nothing

The inputs are the outputs of tools/bench_scalar_index.py, tools/bench_vector_index.py and
tools/bench_fts.py. Every timed aspect (each index build, each query or search) becomes a metric:
its speed is Rust Lance's time over nanolance's, so 2.0 means nanolance takes half of Rust Lance's time
and 0.5 means it takes twice as long. The matrix (reads and writes) is gated by tools/bench_compare.py.

A machine's baseline is the metrics recorded on it with --record (CI keeps one per runner CPU under
bench/results/perf/, as bench_compare.py does). Against it, a run fails when

  * a metric's speed falls below FLOOR x its baseline speed -- where nanolance led, it must still
    lead by most of that margin; where it lagged, the lag must not grow by more than 1 / FLOOR;
  * the geometric mean of a family (scalar, vector, fts) falls below FAMILY_FLOOR x its baseline,
    so many small slips that each pass the per-metric floor still fail together;
  * a nanolance recall@10 is below Rust Lance's by more than RECALL_SLACK, or a baseline metric is missing.

Metrics where either engine takes under MIN_MS are left out: below that, a shared runner's scheduling
is most of the time. With no baseline for this machine, only the correctness checks run, and the
output says how to record one. The floors are set from the run-to-run spread measured on the dev
machine (see docs/PERFORMANCE_GATE.md), not from a guess.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import re
import sys
from pathlib import Path

FLOOR = 0.60
FAMILY_FLOOR = 0.85
RECALL_SLACK = 0.03  # IVF_PQ's recall moves by ~0.015 between identical runs (the codebook is trained afresh)
MIN_MS = 0.25
FAMILIES = ("scalar", "vector", "fts")


def cpu_name() -> str:
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def cpu_slug(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def machine_key(env: dict) -> tuple:
    return (env.get("cpu"), env.get("cores"))


def metric(family: str, key: str, rust_ms: float, nanolance_ms: float) -> dict:
    return {
        "family": family,
        "rust_ms": rust_ms,
        "nanolance_ms": nanolance_ms,
        "speed": rust_ms / nanolance_ms if nanolance_ms else None,
    }


def extract(scalar: dict | None, vector: dict | None, fts: dict | None) -> tuple[dict, dict]:
    """Flat metrics and recalls from the three benchmark outputs. Each aspect records Rust Lance's and
    nanolance's median time in milliseconds; speed is Rust's time over nanolance's. An aspect Rust Lance's
    driver does not measure (an index kind it cannot build) has no Rust time and is left out."""
    metrics, recall = {}, {}

    def add(key: str, family: str, name: str, rec: dict) -> None:
        if rec.get("rust") and rec.get("nanolance"):
            metrics[key] = metric(family, name, rec["rust"]["median_ms"], rec["nanolance"]["median_ms"])

    if scalar:
        for name, rec in scalar["builds"].items():
            add(f"scalar build {name}", "scalar", name, rec)
        for name, rec in scalar["queries"].items():
            add(f"scalar query {name}", "scalar", name, rec)
    if vector:
        for kind, rec in vector["builds"].items():
            add(f"vector {kind} build", "vector", kind, rec)
        for key, rec in vector["searches"].items():
            kind, name = key.split("|", 1)
            add(f"vector {kind} search {name}", "vector", kind, rec)
        for kind, rec in vector.get("recall@10", {}).items():
            if rec.get("rust") is not None and rec.get("nanolance") is not None:
                recall[f"vector {kind}"] = rec
    if fts:
        add("fts build", "fts", "build", fts["builds"]["INVERTED"])
        for name, rec in fts["searches"].items():
            add(f"fts search {name}", "fts", name, rec)
    return metrics, recall


def family_geomean(metrics: dict, family: str) -> float | None:
    speeds = [m["speed"] for m in metrics.values() if m["family"] == family and m["speed"]]
    return math.exp(sum(math.log(s) for s in speeds) / len(speeds)) if speeds else None


def timed_enough(m: dict) -> bool:
    return min(m["rust_ms"], m["nanolance_ms"]) >= MIN_MS


def compare(baseline: dict | None, metrics: dict, recall: dict) -> tuple[list[str], list[str]]:
    problems, lines = [], []
    for key, rec in recall.items():
        if rec["nanolance"] < rec["rust"] - RECALL_SLACK:
            problems.append(f"{key}: recall@10 {rec['nanolance']:.3f}, Rust Lance {rec['rust']:.3f}")
    if baseline is None:
        return problems, lines
    base_metrics, base_recall = baseline["metrics"], baseline.get("recall", {})
    for key in base_metrics:
        if key not in metrics:
            problems.append(f"{key}: in the baseline but not measured this run")
    for key in base_recall:
        if key not in recall:
            problems.append(f"{key}: recall in the baseline but not measured this run")
    for key, m in metrics.items():
        base = base_metrics.get(key)
        if base is None or not timed_enough(m) or not timed_enough(base):
            continue
        if m["speed"] < FLOOR * base["speed"]:
            problems.append(
                f"{key}: nanolance {m['speed']:.2f}x Rust Lance's speed (baseline {base['speed']:.2f}x, "
                f"floor {FLOOR * base['speed']:.2f}x): nanolance {m['nanolance_ms']:.2f} ms, Rust Lance {m['rust_ms']:.2f} ms")
    for family in FAMILIES:
        timed = {k: v for k, v in metrics.items() if k in base_metrics and timed_enough(v)}
        common = {k: v for k, v in timed.items() if timed_enough(base_metrics[k])}
        in_family = [k for k in common if metrics[k]["family"] == family]
        now = family_geomean({k: metrics[k] for k in in_family}, family)
        was = family_geomean({k: base_metrics[k] for k in in_family}, family)
        if now and was:
            lines.append(f"{family:8s} baseline {was:.2f}x  now {now:.2f}x  ({len(in_family)} metrics)")
            if now < FAMILY_FLOOR * was:
                problems.append(f"{family}: geometric mean {now:.2f}x Rust Lance's speed, baseline {was:.2f}x "
                                f"(floor {FAMILY_FLOOR * was:.2f}x)")
    return problems, lines


def load(path: str | None) -> dict | None:
    if not path:
        return None
    with open(path) as f:
        return json.load(f)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scalar")
    ap.add_argument("--vector")
    ap.add_argument("--fts")
    ap.add_argument("--baseline-dir", type=Path, help="directory of per-CPU baselines (bench/results/perf)")
    ap.add_argument("--record", type=Path, help="write this run as the baseline for this machine, and exit")
    ap.add_argument("--skip", action="store_true", help="run nothing (also NANOLANCE_PERF_SKIP=1)")
    args = ap.parse_args(argv)

    if args.skip or os.environ.get("NANOLANCE_PERF_SKIP") == "1":
        print("performance gate skipped (--skip or NANOLANCE_PERF_SKIP=1)")
        return 0

    scalar, vector, fts = load(args.scalar), load(args.vector), load(args.fts)
    metrics, recall = extract(scalar, vector, fts)
    if not metrics:
        ap.error("no benchmark results given (--scalar, --vector, --fts)")
    cpu = cpu_name()
    cores = os.cpu_count()
    env = {"cpu": cpu, "cores": cores, "machine": platform.machine()}
    print(f"machine: {cpu}, {cores} cores; {len(metrics)} timed metrics, {len(recall)} recall checks")

    if args.record:
        args.record.parent.mkdir(parents=True, exist_ok=True)
        args.record.write_text(json.dumps({"environment": env, "metrics": metrics, "recall": recall}, indent=1))
        print(f"recorded baseline: {args.record}")
        return 0

    baseline = None
    if args.baseline_dir and args.baseline_dir.is_dir():
        for path in sorted(args.baseline_dir.glob("*.json")):
            base = json.loads(path.read_text())
            if machine_key(base["environment"]) == machine_key(env):
                baseline = base
                print(f"baseline: {path}")
                break
    if baseline is None:
        print("no baseline for this machine: correctness checks only. Record one with\n"
              "  python tools/perf_gate.py ... --record bench/results/perf/" + cpu_slug(cpu) + f"-{cores}c.json")

    # Every metric, slowest relative to Rust Lance first, so each run's log shows where nanolance stands.
    print("speed vs Rust Lance (Rust's time / nanolance's):")
    for key, m in sorted(metrics.items(), key=lambda kv: kv[1]["speed"] or 0.0):
        if m["speed"]:
            print(f"  {m['speed']:6.2f}x  {key}  (nanolance {m['nanolance_ms']:.2f} ms, Rust Lance {m['rust_ms']:.2f} ms)")

    problems, lines = compare(baseline, metrics, recall)
    for line in lines:
        print(line)
    for p in problems:
        print("FAIL", p)
    if problems:
        print(f"performance gate: {len(problems)} problem(s)")
        return 1
    print("performance gate: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
