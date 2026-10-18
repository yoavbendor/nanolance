# Performance gate

CI fails when nanolance loses ground against Rust Lance (the `lance` crate, version 12.0.0, the one
pylance 12.0.0 is built from) on the benchmarks that show its lead. Two gates run in
`.github/workflows/linux-bench.yml`:

| Gate | What it compares | Tool | Baseline |
|---|---|---|---|
| Matrix | 27 data types, reads and writes, 1 core and all cores | `tools/bench_compare.py` | `bench/results/ci/<cpu>.json` |
| Index | scalar builds and queries, vector builds and searches (IVF_FLAT, IVF_PQ, IVF_HNSW_SQ), full-text build and searches, recall | `tools/perf_gate.py` | `bench/results/perf/<cpu>-<cores>c.json` |

Both engines run **without Python in the timed process**. nanolance's index benchmarks run on its C++
library (`build/nlbench_index`, from `tools/nlbench_index.cpp`); Rust Lance runs on the lance crate
(`tools/lance_rs_bench`, the `index` subcommand). Python generates the data and reads the results. pylance
writes the base datasets, which is data generation, not timed. The matrix still compares with pylance, as
before; moving it to Rust Lance is the next step.

Each timed aspect is a **speed**: Rust Lance's time over nanolance's, both measured in the same run. 2.0
means nanolance takes half of Rust Lance's time; 0.5 means it takes twice as long. Ratios are compared, not
times, so a slower runner does not fail by itself.

## How samples are taken

Every measurement repeats until its **timed total reaches 10 seconds** (`--min-seconds`), with a minimum
number of samples:

- a **build** runs on a fresh copy of the unindexed data (the copy is untimed), at least three builds;
- a **scalar query** is one sample per call, at least 20 calls;
- a **vector or full-text search** is one sample per pass over its query set (50 queries), divided by 50, at least 5 passes.

The reported number is the median, with the minimum and the sample count. Both engines get the same
workload file (`tools/index_bench_common.py` writes it) and the same data, and both use every core.

Before any time is reported, the two engines must agree: scalar row counts match, full-text rows and BM25
scores match (ties at the cut-off may differ), and vector recall@10 of nanolance is within 0.03 of Rust Lance's.

## What fails

Against the baseline recorded on the same CPU model and core count:

- **A metric** whose speed falls below 0.6 x its baseline speed: a loss of more than 40% relative to Rust Lance.
  Where nanolance led 2x, it must still lead 1.2x; where it lagged 0.5x, the lag may not grow past 0.3x.
- **A family** (scalar, vector, full-text) whose geometric-mean speed falls below 0.85 x its baseline.
- **Recall**: nanolance's recall@10 below Rust Lance's by more than 0.03.
- **A missing metric**: a baseline metric not measured in this run.

A baseline for another CPU is not used: the ratios differ between CPU models. Such a run still checks
correctness and prints how to record one.

**The floors were set before this change**, from runs against pylance with five-sample medians. The
spread that set them was mostly sampling noise, so with 10-second samples the floors are looser than they
need to be. Tighten them after the first baselines are recorded on the CI runner, from the spread in its
runs.

## Coverage and time

Quick scale, in CI:

- scalar: 500,000 rows, 5 fragments, BTREE / BITMAP / LABEL_LIST builds and seven queries
- vector: 50,000 rows, 128 dimensions, 256 partitions, three index types, four searches each
- full-text: 100,000 rows, six searches, plus one all-matches search

At 10 seconds per aspect and per engine, the index suite takes about 15 minutes on a 4-core runner, plus the
Rust build (cached by its lockfile). Full scale (`--rows 5000000` and the defaults in each script) takes longer.

Not covered by the gate, though measured in the repo: the full-scale runs, the multimodal benchmark, the
COCO and Speech Commands runs, memory footprint, and the pylance and Rust suites (tested for correctness by their own jobs).

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

GitHub's `ubuntu-latest` pool mixes CPU models, so one recording covers only the CPU it ran on. The gate
reports which CPU it found and whether a baseline matched. Baselines recorded against pylance before this
change do not match the current metrics and must be removed and recorded again.

## Running locally

Build the two drivers first:

```sh
cmake --build build --target nlbench_index
cargo build --release --manifest-path tools/lance_rs_bench/Cargo.toml
```

Then, one benchmark at a time, on an otherwise idle machine:

```sh
python tools/bench_scalar_index.py --rows 500000 --work /tmp/nlb-idx --out /tmp/perf-scalar.json
python tools/bench_vector_index.py --rows 50000  --work /tmp/nlb-idx --out /tmp/perf-vector.json
python tools/bench_fts.py          --rows 100000 --work /tmp/nlb-idx --out /tmp/perf-fts.json
python tools/perf_gate.py --scalar /tmp/perf-scalar.json --vector /tmp/perf-vector.json \
    --fts /tmp/perf-fts.json --baseline-dir bench/results/perf
```

Run them one at a time. Concurrent runs share the cores and swing the ratios by 2x. Local baselines
compare only with the same machine.
