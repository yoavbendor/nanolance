# Performance gate

CI fails when nanolance loses ground against pylance (Rust Lance, pinned to 12.0.0) on the benchmarks
that show its lead. Two gates run in `.github/workflows/linux-bench.yml`:

| Gate | What it compares | Tool | Baseline |
|---|---|---|---|
| Matrix | 27 data types, reads and writes, 1 core and all cores | `tools/bench_compare.py` | `bench/results/ci/<cpu>.json` |
| Index | scalar builds and queries, vector builds and searches (IVF_FLAT, IVF_PQ, IVF_HNSW_SQ), full-text build and searches, recall | `tools/perf_gate.py` | `bench/results/perf/<cpu>-<cores>c.json` |

Each timed aspect is a **speed**: pylance's time over nanolance's, both measured in the same run. 2.0
means nanolance takes half of pylance's time; 0.5 means it takes twice as long. Ratios are compared,
not times, so a slower runner does not fail by itself.

## What fails

Against the baseline recorded on the same CPU model and core count:

- **A metric** whose speed falls below 0.6 x its baseline speed: a loss of more than 40% relative to
  pylance. Where nanolance led 2x, it must still lead 1.2x; where it lagged 0.5x, the lag may not grow past 0.3x.
- **A family** (scalar, vector, full-text) whose geometric-mean speed falls below 0.85 x its baseline:
  many small slips that each pass the per-metric floor still fail together.
- **Recall**: nanolance's recall@10 below pylance's by more than 0.03.
- **A missing metric**: a baseline metric not measured in this run.

A baseline for another CPU is not used: the ratios differ between CPU models (the matrix baselines show
2.5x vs 3.4x on reads at the same commit). Such a run still checks correctness and prints how to record one.

## Why these floors

Measured on the dev machine (Intel Xeon 2.1 GHz, 4 cores) with the quick scale, running the benchmarks
one after another. Running them concurrently swings the ratios 2x, so the gate never does that.

Two sequential runs of identical code:

- Family geometric means moved by under 8% (scalar 2.46x -> 2.57x, vector 2.83x -> 2.72x, full-text 3.69x -> 3.59x).
- Builds moved by about 10-20%, except one scalar build (a BTREE on an integer column) that moved 1.58x.
- Sub-millisecond queries moved by up to about 25%.
- IVF_PQ recall moved by about 0.015.

The floors sit outside that noise: 0.6 per metric and 0.85 per family leave about two-times margin on
the spread observed. A genuine step back, such as a fast path gone or a page decoded whole again,
moves a family geometric mean by more than 15%.

The gate measures on shared CI runners, which are noisier than the dev machine. The first runs on a
new runner type can tell whether these floors need widening; the spread is in the job log.

## Coverage and time

Quick scale, about 2 minutes on a 4-core runner:

- scalar: 500,000 rows, 5 fragments, BTREE / BITMAP / LABEL_LIST builds and seven queries, 5 runs each
- vector: 50,000 rows, 128 dimensions, 256 partitions, three index types, four searches each
- full-text: 100,000 rows, six searches, 5 runs each

Not covered by the gate, though measured in the repo: the full-scale runs (`bench/results/scalar_index.json`,
`vector_index.json`, `fts_index.json` at 5M / 200k / 500k rows), the multimodal benchmark, the COCO and
Speech Commands runs, memory footprint, and the pylance and Rust suites (tested for correctness by their own jobs).

## Skipping

When time is pressing, the gate's cost can be dropped:

- **Manual run:** dispatch `linux-bench` with `skip_perf` set to true.
- **A push:** put `[skip perf]` in the commit message.
- **Locally:** `python tools/perf_gate.py --skip ...` or `NANOLANCE_PERF_SKIP=1`.

A skipped run still builds, tests and runs the matrix. Only the index benchmarks and the gate are skipped.

## Recording a baseline

A baseline is the record of a known-good commit on one runner CPU. Re-record after a change that moves
performance on purpose, and for each new runner CPU:

1. Push the branch.
2. Dispatch `linux-bench` with `record_perf_baseline` set to true. The job measures the index benchmarks
   and commits `bench/results/perf/<cpu>-<cores>c.json` to the branch with `[skip ci]`.
3. Later pushes compare against that file.

GitHub's `ubuntu-latest` pool mixes CPU models, so one recording covers only the CPU it ran on. The
gate reports which CPU it found and whether a baseline matched.

## Running locally

```sh
python tools/bench_scalar_index.py --rows 500000 --runs 5 --out /tmp/perf-scalar.json
python tools/bench_vector_index.py --rows 50000 --runs 5 --out /tmp/perf-vector.json
python tools/bench_fts.py --rows 100000 --runs 5 --out /tmp/perf-fts.json
python tools/perf_gate.py --scalar /tmp/perf-scalar.json --vector /tmp/perf-vector.json \
    --fts /tmp/perf-fts.json --baseline-dir bench/results/perf
```

Run them one at a time on an otherwise idle machine. Local baselines compare only with the same machine.
