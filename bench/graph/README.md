# Graph workload benchmark (nanolance vs pylance vs Ladybug)

`graph_bench.py` times the query shapes of cognee's nanolance graph adapter
(`cognee_nanograph/adapter.py`) on one synthetic graph, loaded three ways:

- **nanolance**: the engine under test (`from nanolance import lance`).
- **pylance**: the reference Lance reader (`import lance`). Writes the files and builds
  the indexes. Both engines read the same single copy of the Lance tables.
- **Ladybug** (`ladybug` 0.19.0, a Kuzu fork): the same graph as a node table with
  `PRIMARY KEY id` and a rel table `Edge`, loaded with `COPY FROM parquet`.

Every query sample is also checked. nanolance must return the same rows as pylance
(compared after sorting all columns). Ladybug must return the same ids. A difference
prints a `MISMATCH` line and the script exits non-zero.

## How to run

```bash
# quick check, about 1 second to generate and load
python3 bench/graph/graph_bench.py --small

# default size: 250k nodes, 1M edges (7 to 11 minutes on the machine below, with other jobs running)
python3 bench/graph/graph_bench.py --json results.json

# options: --nodes N --edges M --seed S --sample 20 --reps 5 --warmup 3
#          --workdir /tmp/graphbench --keep-data
```

Requires `numpy`, `pyarrow`, `pylance` (`lance`), `nanolance` and `ladybug`. The run
directory is created under `--workdir` and deleted at the end unless `--keep-data` is set.

## Data

- **Nodes** (`nodes`): `id` (uuid4 strings), `name` (unique, not indexed), `type` (20
  types, Zipf-weighted), `created_at` / `updated_at` (UTC timestamps), `properties`
  (JSON, about 200 to 500 bytes, zstd per the adapter's schema), and the four
  `list<string>` provenance columns (one element each).
- **Edges** (`edges`): `source_id`, `target_id`, `relationship_name` (30 names, Zipf
  weighted), timestamps, `properties` (JSON, about 60 to 130 bytes), provenance lists,
  and `edge_key = source|relationship|target`. Duplicate triples and self-loops are
  removed, so the edge count is exact.
- **Degree distribution**: endpoints are drawn with weight `rank^-0.9` over independently
  permuted nodes, so there are hubs. Edge counts per hub are in the results below.
- **Indexes**, built by pylance on the shared copy: `nodes.id` BTREE, `nodes.type` BITMAP,
  `edges.source_id` BTREE, `edges.target_id` BTREE, `edges.edge_key` BTREE,
  `edges.relationship_name` BITMAP. nanolance lists them and uses them (the plan check
  shows `ScalarIndexQuery` for `source_id`, `relationship_name` and `id` filters, and no
  index for `name`).
- **Ladybug**: `Node` and `Edge` tables, with the same columns. Ladybug has only its
  primary-key index, plus CSR adjacency for rel tables. No BITMAP index is created.

## Queries

"Sample" means a fixed seeded list of ids or values. Each query runs every sample
`--reps` times after `--warmup` untimed calls. p50, p95 and mean are over all timed
calls, in milliseconds. Timings include the result materialised as an Arrow table.

| Query | What it does | nanolance / pylance | Ladybug |
|---|---|---|---|
| `one_hop_single` | Edges touching one ordinary node (non-hub, degree at least 1), then the neighbour nodes | `edges`: `source_id = X OR target_id = X`; then `nodes`: `id IN (...)` in slices of 500 | `MATCH (a:Node {id:$id})-[r:Edge]-(b:Node)` |
| `one_hop_hub` | Same, for each of the top-20 degree nodes | same | same |
| `edges_in_batch_50` / `_500` | Outgoing edges of 50 / 500 random nodes | `source_id IN (...)` | `WHERE a.id IN $ids` |
| `nodes_in_batch_50` / `_500` | Node rows for 50 / 500 random ids | `id IN (...)` | `WHERE n.id IN $ids` |
| `in_unindexed_50` / `_500` | Nodes whose `name` is in 50 / 500 names (no index on `name`) | `name IN (...)` | `WHERE n.name IN $names` |
| `type_filter_count` | Count of nodes of one type (BITMAP index) | `count_rows(type = ...)` | `count(n)` with `n.type = $t` |
| `full_scan_count_edges_rel3` | Count of edges with one of 3 relationship names (BITMAP index) | `count_rows(relationship_name IN (...))` | `count(r)` with `r.relationship_name IN $rels` |
| `one_hop_single_cached_keys` | The adapter's current one-hop path, nanolance only. `source_id`, `target_id` and `id` columns are read once and cached. Each call does `pc.is_in`, `indices_nonzero`, then `take`. | cached keys | n/a |
| `one_hop_hub_cached_keys` | Same, for the hub sample | cached keys | n/a |

The `full_scan_*` and `type_filter` counts are answered from the BITMAP index, so the
name is historical: they do not touch rows. The 0.01 ms figures reflect that.

## Results

Measured on this machine, not a general claim. Run on 2026-11-12 at 20:02 UTC, default size
(250,000 nodes, 1,000,000 edges, seed 20260101), `--sample 20 --reps 5 --warmup 3`
(100 timed calls per row, 20 for the hub rows). The machine was shared with other builds, so
two runs were made and the one with the lower load before it started is kept (load 9.3, against
12.5 for the other run; the other run's numbers are higher on most rows). Raw JSON:
`benchmark_script`, `machine`, `versions` and `results` in the `--json` output.

**Machine:** Intel(R) Xeon(R) Processor @ 2.10GHz, 4 logical cores, 15.7 GB RAM,
Linux 6.18, Python 3.13.18. Single process, one query at a time, warm OS page cache,
no tuning.

**Versions:** pylance 12.0.0, nanolance 0.2.0 (built from commit `e250ea9` into a target directory,
so the JSON records no commit), ladybug 0.19.0, pyarrow 26.0.1, numpy 2.5.3.

**Data sizes:** nodes.lance 86 MB, edges.lance 242 MB (both zstd, pylance-written), ladybug.db
569 MB. The parquet files used for the Ladybug COPY (207 MB) were deleted after loading.

**Graph shape:** max degree 30,336. The top-20 hubs have degree at least 4,496. Median degree 3.
23,104 nodes (9%) have no edges.

**Build times:** generate 6.3 s, pylance write 3.8 s, indexes 2.5 s (edge_key BTREE the slowest at
1.1 s), Ladybug DDL and COPY 38.4 s.

**Warm-up:** before timing a query, the bench runs its sample list once untimed (3 calls, then one
full pass), after a correctness pass that runs every sample on every engine, and the timed passes repeat the same IN lists. So the BTREE index-window cache
(`NANOLANCE_INDEX_PAGE_CACHE_MB`) and the OS page cache are warm for every timed call. A cold first
call costs more. The bench does not record first-call timings, so that cost is not measured here.

| Query | nanolance p50 / p95 / mean (ms) | pylance p50 / p95 / mean (ms) | Ladybug p50 / p95 / mean (ms) | avg rows |
|---|---|---|---|---|
| one_hop_single | 3.15 / 4.83 / 3.40 | 3.79 / 7.08 / 4.35 | 4.74 / 13.49 / 5.87 | 5.9 |
| one_hop_hub | 269.89 / 691.00 / 316.02 | 419.45 / 1277.70 / 526.88 | 54.88 / 87.44 / 58.38 | 7717.8 |
| edges_in_batch_50 | 4.01 / 6.00 / 4.25 | 9.60 / 15.89 / 10.22 | 43.67 / 70.04 / 47.82 | 127.5 |
| edges_in_batch_500 | 37.17 / 90.80 / 43.45 | 64.48 / 88.96 / 65.79 | 268.71 / 345.47 / 274.69 | 1683.5 |
| nodes_in_batch_50 | 2.29 / 3.89 / 2.55 | 7.12 / 10.09 / 7.56 | 61.96 / 82.62 / 64.05 | 50.0 |
| nodes_in_batch_500 | 17.78 / 25.71 / 17.83 | 36.37 / 50.25 / 37.94 | 255.59 / 395.14 / 264.83 | 500.0 |
| in_unindexed_50 | 21.30 / 28.89 / 21.83 | 17.25 / 23.21 / 18.04 | 58.06 / 88.04 / 59.73 | 50.0 |
| in_unindexed_500 | 29.88 / 40.34 / 30.87 | 24.03 / 30.29 / 24.41 | 203.22 / 286.98 / 214.44 | 500.0 |
| type_filter_count | 0.01 / 0.01 / 0.01 | 0.50 / 0.73 / 0.54 | 6.58 / 8.99 / 7.01 | 21936.2 |
| full_scan_count_edges_rel3 | 0.02 / 0.03 / 0.02 | 1.69 / 2.19 / 1.66 | 50.02 / 67.35 / 52.66 | 81728.9 |
| one_hop_single_cached_keys | 41.41 / 46.56 / 42.15 | n/a | n/a | 5.9 |
| one_hop_hub_cached_keys | 125.23 / 181.73 / 130.34 | n/a | n/a | 7717.8 |

Correctness: 0 mismatches. Every sample of every query was checked: nanolance equals pylance on full
rows, the cached-keys path equals the plain nanolance path, and Ladybug equals pylance on ids.

### History

Headline rows, nanolance p50 in ms. The "before" column is the first table, measured on 2026-11-10,
before the changes listed below it.

| Query | Before (2026-11-10) | Now (2026-11-12) |
|---|---:|---:|
| one_hop_single | 4.40 | 3.15 |
| one_hop_hub | 314.49 | 269.89 |
| edges_in_batch_50 | 29.33 | 4.01 |
| nodes_in_batch_500 | 26.17 | 17.78 |
| in_unindexed_50 | 108.18 | 21.30 |
| in_unindexed_500 | 1012.34 | 29.88 |
| one_hop_single_cached_keys | 37.18 | 41.41 |
| one_hop_hub_cached_keys | 134.30 | 125.23 |

The changes between the two runs: unindexed IN lists use a hash set (`1636abe`, from `dd3dc04`);
dictionary pages expand only the rows a read asks for (`a0f0ca3`, from `73e1b9c`; `31c9097` fixed the
copy that change first made); and the BTREE index-window cache. The `in_unindexed_*` and
`edges_in_batch_50` rows moved most. The cached-keys rows are about where they were (within about 12%).

## Observations

- Unindexed IN lists are now close to pylance: `in_unindexed_500` is 29.9 ms against 24.0 ms (about
  1.2 times), `in_unindexed_50` 21.3 ms against 17.3 ms. Before the hash set it was 1.0 s, about 50 times
  pylance. Both still scan `name` with no index; the remaining gap is not profiled.
- Batch-by-id queries are faster on nanolance than on pylance in this run: `edges_in_batch_50` 4.0 ms
  against 9.6 ms, `nodes_in_batch_500` 17.8 ms against 36.4 ms. Before, `edges_in_batch_50` was 3.4 times
  slower on nanolance.
- Hub one-hop is faster on nanolance than on pylance (270 ms against 419 ms p50), but Ladybug is still
  fastest (55 ms), since it reads the adjacency lists directly.
- For ordinary nodes, the adapter's cached-key path is still about 13 times slower than the filter path
  (41 ms against 3.1 ms p50): the cache saves only reading the key columns once, and each call still runs
  `is_in` over all 1M rows. For hub nodes the cached path is faster than the filter path (125 ms against
  270 ms).
- Counts answered from the BITMAP index (`type_filter_count`, `full_scan_count_edges_rel3`) take about
  0.01 to 0.02 ms on nanolance, from the index alone, against 0.5 and 1.7 ms on pylance.
- Ladybug is the slowest of the three on every batch-by-id and unindexed-name query here. It has no secondary
  index on `source_id`, `type` or `name` in this benchmark.
- Caveats: 4 cores, one process, one query at a time, warm cache, and a shared machine (see the load
  note above). Results are for this graph shape and these query sizes, and may not match other machines or
  workloads. The two runs differed by up to 2 times on nanolance rows (`edges_in_batch_500`, and
  `one_hop_hub` at 515 ms p50 in the other run) and up to 2.5 times on Ladybug rows. Treat differences
  smaller than that as noise.
