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

# default size: 250k nodes, 1M edges (about 4 minutes on the machine below)
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

Measured on this machine, not a general claim. Run on 2026-11-10, default size
(250,000 nodes, 1,000,000 edges, seed 20260101), `--sample 20 --reps 5 --warmup 3`
(100 timed calls per row, 20 for the hub rows).

**Machine:** Intel(R) Xeon(R) Processor @ 2.30GHz, 4 logical cores, 15.7 GB RAM,
Linux 6.18, Python 3.13.18. Single process, one query at a time, warm
OS page cache, no tuning.

**Versions:** pylance 12.0.0, nanolance 0.2.0 (editable install from the main nanolance
tree, commit 2c7d090), ladybug 0.19.0, pyarrow 26.0.1, numpy 2.5.3.

**Data sizes:** nodes.lance 86 MB, edges.lance 242 MB (both zstd, pylance-written),
ladybug.db 569 MB. The parquet files used for the Ladybug COPY (207 MB) were deleted
after loading.

**Graph shape:** max degree 30,336. The top-20 hubs have degree at least 4,496. Median
degree 3. 23,104 nodes (9%) have no edges.

**Build times:** generate 5.0 s, pylance write 2.7 s, indexes 2.5 s (edge_key BTREE the
slowest at 1.3 s), Ladybug DDL and COPY 29.8 s.

| Query | nanolance p50 / p95 / mean (ms) | pylance p50 / p95 / mean (ms) | Ladybug p50 / p95 / mean (ms) | avg rows |
|---|---|---|---|---|
| one_hop_single | 4.40 / 10.97 / 5.43 | 2.70 / 5.48 / 3.06 | 3.48 / 12.06 / 4.48 | 5.9 |
| one_hop_hub | 314.49 / 716.89 / 366.11 | 463.07 / 1437.10 / 589.28 | 65.70 / 103.43 / 73.61 | 7717.8 |
| edges_in_batch_50 | 29.33 / 46.38 / 32.08 | 8.58 / 17.31 / 9.89 | 63.05 / 107.39 / 67.51 | 127.5 |
| edges_in_batch_500 | 79.31 / 162.91 / 86.41 | 58.41 / 99.09 / 63.77 | 291.92 / 415.96 / 299.33 | 1683.5 |
| nodes_in_batch_50 | 8.41 / 14.47 / 9.35 | 6.35 / 7.68 / 6.25 | 92.44 / 132.10 / 98.01 | 50.0 |
| nodes_in_batch_500 | 26.17 / 34.88 / 27.29 | 32.16 / 46.70 / 34.43 | 363.84 / 442.83 / 374.00 | 500.0 |
| in_unindexed_50 | 108.18 / 129.93 / 110.53 | 14.43 / 18.37 / 15.19 | 57.24 / 98.04 / 63.06 | 50.0 |
| in_unindexed_500 | 1012.34 / 1171.80 / 1020.97 | 19.80 / 28.62 / 20.77 | 277.33 / 376.57 / 287.75 | 500.0 |
| type_filter_count | 0.01 / 0.01 / 0.01 | 0.45 / 0.66 / 0.47 | 9.33 / 17.84 / 10.31 | 21936.2 |
| full_scan_count_edges_rel3 | 0.01 / 0.01 / 0.01 | 1.44 / 1.87 / 1.41 | 78.89 / 107.76 / 80.36 | 81728.9 |
| one_hop_single_cached_keys | 37.18 / 44.79 / 37.81 | n/a | n/a | 5.9 |
| one_hop_hub_cached_keys | 134.30 / 202.64 / 140.77 | n/a | n/a | 7717.8 |

Correctness: 0 mismatches. Every sample of every query was checked: nanolance equals
pylance on full rows, the cached-keys path equals the plain nanolance path, and Ladybug
equals pylance on ids.

## Observations

- `in_unindexed_500` is about 50 times slower on nanolance (1.0 s) than on pylance
  (20 ms). `in_unindexed_50` is about 7 times slower. Ladybug takes 277 ms here. The
  plans differ: pylance filters on `name` alone and then takes the other columns, while
  nanolance's plan is one scan that projects `id, name, type` with the filter applied
  inside it. That is a likely cause, but it is not confirmed by profiling.
- `edges_in_batch_50` is about 3.4 times slower on nanolance than on pylance, and the
  gap narrows at 500 ids. Both use the same BTREE index on `source_id`. Not investigated
  further.
- For ordinary nodes, the adapter's cached-key path is about 8 times slower than the
  filter path (37 ms vs 4.4 ms p50). The cache only saves reading the key columns once;
  each call still runs `is_in` over all 1M rows. For hub nodes the cached path is faster
  than the filter path (134 ms vs 314 ms p50).
- Hub one-hop is faster on nanolance than on pylance (314 ms vs 463 ms p50), but both
  are slower than Ladybug (66 ms), which reads the adjacency lists directly.
- Ladybug is the fastest of the three on `one_hop_hub`, and it is slower than pylance on
  the batch-by-id and unindexed-name queries. It is the slowest on the batch-by-id
  queries. It has no secondary index on `source_id`, `type` or `name` in this benchmark.
- Caveats: 4 cores, one process, one query at a time, and warm cache. Results are for
  this graph shape and these query sizes, and may not match other machines or workloads.
