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
and are deleted after each run's measurement. Logs and one JSON line per run (timings, peak RSS, load,
output bytes, and each output table's column set with its row count) go to `WORKDIR/logs/` and
`WORKDIR/results.jsonl`.

## Assertions

`run.sh` prints PASS or FAIL for each check and exits 1 if any check fails. It prints INFO for known
differences.

- **A. Every input line is accounted for.** For each side and configuration, rows + rejects +
  unwritable equals the number of non-blank input lines.
- **B. Tables both sides wrote agree.** Tables are matched by their set of column names, since the two
  sides name them differently. Every table the baseline wrote has a tool table with the same column set
  and the same row count. A column set can hold several tables (with `--route-key type`), so the check
  compares the baseline's row counts with the tool's as a sub-multiset. Tool tables the baseline did not
  write are INFO.
- **B2.** The default tool output (zstd on strings) and the `--no-compress` output have the same tables
  and row counts.
- **C. Threads do not change the output.** `--threads 4` writes the same tables and row counts as one
  thread.

The known difference is INFO only: the baseline leaves one group unwritable (see "Why the baseline
loses rows").

## What the baseline does and does not do

It does: parse each line with `json.loads`, reject non-objects and malformed lines, group by the sorted
top-level key tuple (or by the value of `--route-key`), read each group with one `read_json` call
(schema inferred from the whole group), and write one Lance dataset per group. Top-level keys with a
`.` are renamed to `_` first (the same rename the tool does), because pylance refuses such names. If a
group fails to read, it bisects to find lines that fail alone, rejects them, and retries the rest.
A table that still fails to write is removed and counted unwritable.

It does not: lock a schema after a learning window, so a type that changes later is never rejected
early. It cannot isolate a conflict between two good lines. It does not stage or flush (each group is
held in memory), detect duplicate keys, or use multiprocessing. Its column order is not stable: with
threaded reads, pyarrow merges per-block schemas in an order that changes between runs (three reads of
one group gave three different top-level orders); single-threaded reads give the first-seen order.

## Results

Medians of 3 runs, `RUNS=3 bench/ndjson/run.sh`. Machine: 4 vCPU Intel Xeon @ 2.10 GHz, 15 GB RAM,
Linux VM, shared; 1-minute load average 1.0 to 1.9 during the runs. Input: 989,133,346 bytes, 900,000
non-blank lines (`gen_logs.py --profile realistic --seed 2 --lines 900000`). MB = 10^6 bytes. Output
bytes are the size of all files under the output directory.

1 GB stream, key mode unless stated:

| config | wall s | input MB/s | rows/s | peak RSS MB | output MB | out/in | tables | rows | rejects | unwritable |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ndjson2lance (zstd on strings, default) | 6.823 | 145.0 | 129,120 | 147.9 | 170.1 | 0.172 | 7 | 880,999 | 19,001 | 0 |
| ndjson2lance `--no-compress` | 5.297 | 186.7 | 166,321 | 140.1 | 816.8 | 0.826 | 7 | 880,999 | 19,001 | 0 |
| ndjson2lance default, `--threads 4` | 6.804 | 145.4 | 129,476 | 173.7 | 170.1 | 0.172 | 7 | 880,999 | 19,001 | 0 |
| ndjson2lance default, `--route-key event` | 6.921 | 142.9 | 127,297 | 127.9 | 170.2 | 0.172 | 6 | 880,999 | 19,001 | 0 |
| baseline | 38.388 | 25.8 | 17,548 | 2,191.3 | 122.7 | 0.124 | 6 | 673,623 | 1,837 | 224,540 |
| baseline `--route-key event` | 40.364 | 24.5 | 16,689 | 2,237.6 | 122.8 | 0.124 | 5 | 673,623 | 1,837 | 224,540 |

zstd costs about 1.5 s of 6.8 s here (22%) and cuts the output 4.8 times. An earlier, more loaded run
of the same script gave slower and noisier timings on both sides (single baseline runs from 43 to
163 s); the ratios between rows were similar.

GH Archive hour `2026-11-15-12.json.gz` (7.6 MB gzipped, 19,151 lines), `--route-key type`:

| config | wall s | input MB/s | rows/s | peak RSS MB | output MB | out/in | tables | rows | rejects | unwritable | load1 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ndjson2lance (zstd default) | 0.433 | 78.3 | 44,187 | 123.5 | 15.1 | 0.445 | 16 | 19,151 | 0 | 0 | 1.17 |
| baseline | 2.290 | 14.8 | 7,010 | 373.3 | 6.5 | 0.191 | 13 | 16,055 | 0 | 3,096 | 1.38 |

## Assertion output (1 GB stream)

All A, B, B2 and C checks pass (`PASS` lines, 23 in total for the realistic run). The INFO lines are the
known difference: the baseline writes no table for the metric records (207,376 rows in the tool's
table with 8 columns) and for three GH event types (727, 812 and 1,557 rows). Those are exactly its
unwritable counts.

## Types and values

Checked on the 1 GB stream, on the six tables both sides wrote (matched by column set; the same
dotted-to-underscore rename on both sides). The tool's output is from the default build, the baseline's
from `baseline.py`; both were read back with pylance.

- **Schemas, field by field, nested children included.** Field name, Arrow type (including `list`
  item and `struct` children), nullable flag, and field count are identical in all six tables. No
  `timestamp`, `float64`/`int64`, `map` or nullable differences exist, so no cast was applied. The data
  has no timestamp values: `ts` is a string on both sides, and neither side parses it.
- **Top-level field order.** One table differs: `conditions_count` (the 8-field key set with
  `conditions` and `count`). The tool's order is the first-seen order of its first input line of that
  key set. The baseline's order changes between runs (threaded `read_json`, see above); a
  single-threaded read gives the same order as the tool. The order is not a value difference.
- **Values, row by row, in input order.** Every row of the six tables was compared as a Python dict
  (rows compared, equal, differing): `t01_db_detail_duration_ms` 134,725 / 134,725 / 0;
  `t05_conditions_count_detail` 134,724 / 134,724 / 0; `t04_client_ip_detail_duration_ms` 134,725 /
  134,725 / 0; `t03_code_context_detail` 134,724 / 134,724 / 0; `t06_actor_detail_event` 94,010 /
  94,010 / 0; `t07_actor_detail_event` 40,715 / 40,715 / 0. Total 673,623 rows, 0 differing, 0 examples
  to show. Both sides keep input order within each table, so no sorting was needed. The check is not
  vacuous: changing one `detail` value in one row was detected (1 differing row).
- **Caveat.** Python `==` treats `1` and `1.0` as equal. The schemas are identical, so the column types
  cannot differ, but the value check cannot see an int-versus-float difference on its own; the schema
  check covers that.

The comparison scripts are `schema_cmp.py` and `value_cmp.py` in this directory; run them on a tool
output directory and a baseline output directory (run.sh deletes its outputs, so write them by hand).

## Why the baseline loses rows

The metric records change type partway through: `sample_count` is a number in some rows and a
string in later ones (`gen_logs.py` case 7). pyarrow infers one schema for the whole group, so the
group fails. Bisection cannot isolate the conflict, because each line alone reads fine. The tool learns
its schema from the first rows, locks it, and rejects only the later conflicting rows (19,001 rejects
in total, against the baseline's 1,837). So row counts differ, and the size and value comparisons are
made only on tables both sides wrote.

## Output size and compression

The tool now compresses string columns with zstd by default (`--no-compress` turns it off). Before that
change, the default output was 816.8 MB, 6.7 times the baseline's 122.8 MB.

Per table, for the six tables both sides wrote (bytes per row, from the earlier run with the same
options: `--no-compress` was the old default, and the zstd column was `--compress`):

| table (tool name) | rows | tool `--no-compress` B/row | tool zstd (default) B/row | baseline B/row | tool data files | baseline data files |
|---|---:|---:|---:|---:|---:|---:|
| t01_db_detail_duration_ms | 134,725 | 905.5 | 192.2 | 188.3 | 18 | 1 |
| t05_conditions_count_detail | 134,724 | 904.0 | 187.9 | 181.4 | 19 | 1 |
| t04_client_ip_detail_duration_ms | 134,725 | 859.6 | 186.2 | 182.4 | 19 | 1 |
| t03_code_context_detail | 134,724 | 865.4 | 176.6 | 166.8 | 19 | 1 |
| t06_actor_detail_event | 94,010 | 947.2 | 198.0 | 193.1 | 14 | 1 |
| t07_actor_detail_event | 40,715 | 920.3 | 193.7 | 188.6 | 7 | 1 |

With zstd the tool is within about 6% of the baseline per table (largest gap t03, +5.9%).

Per column, for `t01_db_detail_duration_ms`, using the first data file of each side (the
`nlance-pagelayout -v` output summed over pages):

| column | tool `--no-compress` | baseline | tool zstd (default) |
|---|---|---|---|
| `detail` (string, long) | 876.3 B/row, FullZip, plain `Variable` | 154.0 B/row, FullZip, `Fsst` | 160.2 B/row, `General{ZSTD}` |
| `ts`, `query_hash` (strings) | 11.0 and 12.5, `Fsst` | 12.0 and 12.5, `Fsst` | 8.0 and 11.9, `ZSTD` |
| `shard.node.zone` (string) | 5.0, plain `Variable` | 5.0, plain `Variable` | 2.8, `ZSTD` |
| int and low-cardinality columns | 0.0 to 1.7 B/row, bit-packed or dictionary | 0.0 to 1.6 B/row, the same | the same as `--no-compress` |
| all other columns together | 33.2 B/row | 33.9 B/row | 27.6 B/row |

The cause of the earlier gap is the `detail` column: the long event detail text averages about 873
bytes per row, and the tool stored it as plain variable-length bytes, so the file held the raw text
(876 B/row). pylance codes the same column with FSST symbol compression (154 B/row). The tool now
compresses it with zstd (160 B/row), which closes the gap. Data file count does not explain it: the
tool writes one data file per flush (7 to 31 per table, about 6.8 MB each), while the baseline writes
one file per table. Outside `detail` the tool spends 33.2 bytes per row in its first file and the
baseline 33.9, so the small fragments add no visible overhead.

A possible follow-up (not applied): FSST for long strings in the FullZip path, as pylance does, would
match the baseline without zstd's extra CPU cost. Zstd on by default is the simpler change, and it is
what was made.

## Interpretation

- Speed and memory: the tool is 5.6 times faster than the baseline with zstd (6.8 s against 38.4 s) and
  7.2 times faster without it, with 13 to 17 times less peak memory (128 to 174 MB against 2.2 GB). The
  baseline holds every group in memory; the tool writes one flush at a time.
- Size: the default output is 170 MB, 1.4 times the baseline's 122.7 MB, and the tables both sides wrote
  are within about 6% per row. The remaining gap is the long `detail` column: pylance uses FSST, the tool
  zstd (see "Output size and compression"). Without zstd the output is 816.8 MB.
- Compression cost: about 1.5 s of 6.8 s, and about 8 MB more peak RSS, for a file 4.8 times smaller.
- `--threads 4` gave no speedup (6.80 s against 6.82 s); the router is one thread. The outputs are
  identical with one or four threads (C).
- Event routing writes 6 tables and the same rows at the same speed as key routing.

## Not done

- Caches were not dropped between runs.
- One GH Archive hour only, and no truth file exists for real data, so its output was checked by counts
  and not by content. The types-and-values check covers only the 1 GB stream.
- The GH baseline's 3,096 unwritable rows are the same kind of type conflict; the groups were not
  examined in detail.
- The FSST follow-up is described, not implemented.
- The baseline is one reasonable implementation, not the fastest possible pyarrow route.
