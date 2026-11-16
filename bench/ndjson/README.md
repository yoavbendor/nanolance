# ndjson2lance benchmark

Compares `examples/ndjson2lance` with the plain Python route (`baseline.py`: `pyarrow.json.read_json`
per group, `lance.write_dataset` per table). Both read the same generated mixed JSON-lines stream.

## How to run

```
bench/ndjson/run.sh NDJSON2LANCE_BINARY [WORKDIR]
```

or `NDJSON2LANCE=/path/to/ndjson2lance bench/ndjson/run.sh`. Needs `pyarrow` and `pylance` for the
baseline, and Python 3 for the generator. Environment:

- `INPUT=FILE` uses an existing realistic NDJSON file; without it the script generates
  `gen_logs.py --profile realistic --seed ${SEED:-2} --lines ${LINES:-900000}` (about 1 GB).
- `RUNS=3` runs per configuration (medians are shown).
- `GH_GZ=FILE` adds the GH Archive section: one `.json.gz` hour, decompressed to WORKDIR, read with
  `--route-key type`. It is skipped when unset.

The binary is built with `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
-DNANOLANCE_BUILD_NDJSON2LANCE=ON && ninja -C build ndjson2lance`. Outputs go to `WORKDIR/out/`
and are deleted after each run's measurement. Logs and one JSON line per run go to `WORKDIR/logs/`
and `WORKDIR/results.jsonl`. The script ends with the table and the assertions, and exits 1 when
any assertion differs.

## What the baseline does and does not do

It does: parse each line with `json.loads`, reject non-objects and malformed lines, group by the sorted
top-level key tuple (or by the value of `--route-key`), read each group with one `read_json` call
(schema inferred from the whole group), and write one Lance dataset per group. If a group fails to
read, it bisects to find lines that fail alone, rejects them, and retries the rest.

It does not: lock a schema after a learning window, so a type that changes later is never rejected
early. It cannot isolate a conflict between two good lines, so a group with a number in one row and a
string in another fails as a whole and is counted unwritable. It does not stage or flush (each group
is held in memory), detect duplicate keys, or use multiprocessing. It renames top-level keys with a
`.` to `_`, because pylance refuses such names; without the rename most tables do not write (on the
small input, 10,555 of 11,959 rows were lost before the rename was added; 10,200 are lost with it,
because of the type conflict described below).

## Results

Medians of 3 runs. Machine: 4 vCPU Intel Xeon @ 2.10 GHz, 15 GB RAM, Linux 6.18 VM. Load average
(1 min) before each run was 2.5 to 3.6 on this shared VM (per run in `results.jsonl`). Input: 989,133,346 bytes
(900,000 lines, 9,000 blank), generated with `gen_logs.py --profile realistic --seed 2 --lines
900000`. MB = 10^6 bytes. Rows are the rows written; rejects and unwritable are counted separately.

| config | wall s | input MB/s | rows/s | peak RSS MB | output MB | out/in | tables | rows | rejects | unwritable |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ndjson2lance, key mode | 5.818 | 170.0 | 151,434 | 141.6 | 816.8 | 0.826 | 7 | 880,999 | 19,001 | 0 |
| ndjson2lance, `--route-key event` | 5.754 | 171.9 | 153,121 | 129.8 | 817.6 | 0.827 | 6 | 880,999 | 19,001 | 0 |
| ndjson2lance, key mode, `--threads 4` | 5.956 | 166.1 | 147,906 | 174.4 | 816.8 | 0.826 | 7 | 880,999 | 19,001 | 0 |
| baseline, key mode | 48.812 | 20.3 | 13,800 | 2,119.4 | 122.7 | 0.124 | 6 | 673,623 | 1,837 | 224,540 |
| baseline, `--route-key event` | 48.499 | 20.4 | 13,889 | 2,162.9 | 122.8 | 0.124 | 5 | 673,623 | 1,837 | 224,540 |

GH Archive hour `2026-11-15-12.json.gz` (7.6 MB gzipped, 19,151 lines), `--route-key type`:

| config | wall s | input MB/s | rows/s | peak RSS MB | output MB | out/in | tables | rows | rejects | unwritable |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ndjson2lance | 0.523 | 65.0 | 36,647 | 120.1 | 17.6 | 0.519 | 16 | 19,151 | 0 | 0 |
| baseline | 2.289 | 14.8 | 7,013 | 380.6 | 6.5 | 0.191 | 13 | 16,055 | 0 | 3,096 |

Assertions (run.sh output): the tool and the baseline do **not** agree. Key mode: tables 7 vs 6,
rows 880,999 vs 673,623. Event mode: tables 6 vs 5, rows 880,999 vs 673,623. GH: tables 16 vs 13,
rows 19,151 vs 16,055. Each differing count is explained above: in the 1 GB input the baseline leaves
unwritable exactly the 224,540 metric records (all of them, the metric key set), because their
`sample_count` changes from a number to a string partway through, so the whole group fails to infer.
The tool learns its schema, rejects those later rows (19,001 rejects in total) and writes the rest.
Both sides account for every non-blank line: 673,623 + 224,540 + 1,837 = 900,000, and
880,999 + 19,001 = 900,000. The tool agrees with itself across modes: key mode and `--threads 4` give
equal tables and rows.

## Interpretation

- On this input the tool is about 8 times faster in wall time (5.8 s against 48.8 s) and uses about
  15 times less peak memory (130 to 170 MB against 2.1 GB). The baseline holds every group in memory;
  the tool streams flushes.
- The baseline writes less data (122.7 MB against 816.8 MB), but it writes 24% fewer rows, so the
  bytes per row are not comparable. Per written row: about 182 bytes for the baseline against about
  927 bytes for the tool. The tool's default has zstd off (`--compress` is off), and the pylance
  writer's encodings were not examined, so the size gap is not explained here.
- `--threads 4` gave no speedup on this run (5.96 s against 5.82 s median). The tool's threads
  only help reads of large flushes, and this file is read once, so this does not show a
  scaling limit.
- Event routing and key routing cost the same.

## Not done

- Caches were not dropped between runs; the first run was not treated as cold.
- One GH Archive hour only; no truth file exists for real data, so its output was not checked against
  content.
- The GH baseline's 3,096 unwritable rows were not examined; the same type-conflict cause is likely
  but unverified.
- The baseline is one reasonable implementation, not the fastest possible pyarrow route.
