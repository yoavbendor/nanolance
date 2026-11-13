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

Measured on this machine, not a general claim. One run on 2026-11-13 at 11:48 UTC, default size
(250,000 nodes, 1,000,000 edges, seed 20260101), `--sample 20 --reps 5 --warmup 3`
(100 timed calls per row). The 1-minute load was 1.2 when the run started and about 2.5 while it ran: the
machine is shared with other builds, so treat differences of up to about 1.5 times as noise. Raw JSON:
`benchmark_script`, `machine`, `versions` and `results` in the `--json` output.

**Machine:** Intel(R) Xeon(R) Processor @ 2.10GHz, 4 logical cores, 15.7 GB RAM,
Linux 6.18, Python 3.13.18. Single process, one query at a time, warm OS page cache,
no tuning.

**Versions:** pylance 12.0.0, nanolance 0.2.0 built from commit `1b205dd` into a target directory (the JSON
records no commit; comment-only edits to headers were made while the build ran), ladybug 0.19.0, pyarrow 26.0.1,
numpy 2.5.3.

**Data sizes:** nodes.lance 86 MB, edges.lance 242 MB (both zstd, pylance-written), ladybug.db
569 MB. The parquet files used for the Ladybug COPY (207 MB) were deleted after loading.

**Graph shape:** max degree 30,336. The top-20 hubs have degree at least 4,496. Median degree 3.
23,104 nodes (9%) have no edges.

**Build times:** generate 6.8 s, pylance write 2.6 s, indexes 2.8 s (edge_key BTREE the slowest at
1.1 s), Ladybug DDL and COPY 25.5 s.

**Warm-up:** before timing a query, the bench runs its sample list once untimed (3 calls, then one
full pass), after a correctness pass that runs every sample on every engine, and the timed passes repeat the same IN lists. So the BTREE index-window cache
(`NANOLANCE_INDEX_PAGE_CACHE_MB`), the dictionary-block cache and the take cache are warm for every timed call. A cold first
call costs more. The bench does not record first-call timings, so that cost is not measured here.

| Query | nanolance p50 / p95 / mean (ms) | pylance p50 / p95 / mean (ms) | Ladybug p50 / p95 / mean (ms) | avg rows |
|---|---|---|---|---|
| one_hop_single | 2.10 / 3.80 / 2.18 | 4.17 / 7.17 / 4.62 | 4.28 / 10.88 / 5.11 | 5.9 |
| one_hop_hub | 152.75 / 490.66 / 196.19 | 440.17 / 1336.77 / 541.77 | 57.10 / 81.10 / 58.74 | 7717.8 |
| edges_in_batch_50 | 2.08 / 2.98 / 2.20 | 9.14 / 14.95 / 9.66 | 52.11 / 68.09 / 54.79 | 127.5 |
| edges_in_batch_500 | 12.26 / 18.12 / 13.69 | 55.25 / 72.45 / 55.05 | 327.96 / 410.59 / 341.21 | 1683.5 |
| nodes_in_batch_50 | 2.47 / 3.22 / 2.60 | 7.89 / 9.33 / 7.99 | 65.67 / 83.21 / 68.83 | 50.0 |
| nodes_in_batch_500 | 15.91 / 19.21 / 15.82 | 34.13 / 40.88 / 34.47 | 332.04 / 420.18 / 343.46 | 500.0 |
| in_unindexed_50 | 23.29 / 32.15 / 25.14 | 16.44 / 19.84 / 16.70 | 49.61 / 68.94 / 53.80 | 50.0 |
| in_unindexed_500 | 31.17 / 39.95 / 32.79 | 21.78 / 26.98 / 22.10 | 311.42 / 396.24 / 320.61 | 500.0 |
| type_filter_count | 0.012 / 0.020 / 0.015 | 0.518 / 0.759 / 0.558 | 9.79 / 12.33 / 10.09 | 21936.3 |
| full_scan_count_edges_rel3 | 0.014 / 0.049 / 0.021 | 2.06 / 3.23 / 2.09 | 56.53 / 78.80 / 60.24 | 81729.0 |
| one_hop_single_cached_keys | 27.76 / 40.98 / 29.88 | n/a | n/a | 5.9 |
| one_hop_hub_cached_keys | 66.71 / 145.77 / 76.84 | n/a | n/a | 7717.8 |

Correctness: 0 mismatches. Every sample of every query was checked: nanolance equals pylance on full
rows, the cached-keys path equals the plain nanolance path, and Ladybug equals pylance on ids.

### History

Headline rows, nanolance p50 in ms. Each column is a separate run; the first is measured before the
changes listed below it.

| Query | 2026-11-10 | 2026-11-12 | 2026-11-13 |
|---|---:|---:|---:|
| one_hop_single | 4.40 | 3.15 | 2.10 |
| one_hop_hub | 314.49 | 269.89 | 152.75 |
| edges_in_batch_50 | 29.33 | 4.01 | 2.08 |
| nodes_in_batch_500 | 26.17 | 17.78 | 15.91 |
| in_unindexed_50 | 108.18 | 21.30 | 23.29 |
| in_unindexed_500 | 1012.34 | 29.88 | 31.17 |
| one_hop_single_cached_keys | 37.18 | 41.41 | 27.76 |
| one_hop_hub_cached_keys | 134.30 | 125.23 | 66.71 |

The changes between the 2026-11-12 and 2026-11-13 runs: decoded dictionary blocks are kept in a process-wide cache
(`7545882`, so a BTREE lookup decodes each dictionary block once, not once per window), and a dense take keeps its
column's decoded copy up to 64 MB (`8346692`, which made a hub's one-hop edge fetch 5 to 8 ms warm). The
`one_hop_hub` row moved most. The unindexed IN rows did not move: both still scan `name` with no index.

## Observations

- Hub one-hop is faster on nanolance than on pylance (153 ms against 440 ms p50), but Ladybug is still
  fastest (57 ms), since it reads the adjacency lists directly.
- Batch-by-id queries are faster on nanolance than on pylance and Ladybug: `edges_in_batch_50` 2.1 ms
  against 9.1 ms (pylance) and 52 ms (Ladybug); `nodes_in_batch_500` 15.9 ms against 34.1 ms and 332 ms.
- Unindexed IN lists are slower than pylance by about 1.4 times: `in_unindexed_50` 23.3 ms against 16.4 ms,
  `in_unindexed_500` 31.2 ms against 21.8 ms. Both scan `name` with no index; the gap is not profiled.
- For ordinary nodes, the adapter's cached-key path is about 13 times slower than the filter path
  (27.8 ms against 2.1 ms p50): the cache saves only reading the key columns once, and each call still runs
  `is_in` over all 1M rows. For hub nodes the cached path is faster than the filter path (66.7 ms against 152.8 ms).
- Counts answered from the BITMAP index (`type_filter_count`, `full_scan_count_edges_rel3`) take about
  0.01 to 0.02 ms on nanolance, from the index alone, against 0.5 and 2.1 ms on pylance.
- Ladybug is the slowest of the three on every batch-by-id and unindexed-name query here. Its rows are higher
  than in the 2026-11-12 run on most rows (up to about 1.5 times), which is consistent with the shared machine.
- Caveats: 4 cores, one process, one query at a time, warm cache, and a shared machine (see the load
  note above). Results are for this graph shape and these query sizes, and may not match other machines or
  workloads. There is one run this time, not two as before; treat differences smaller than about 1.5 times as noise.

## Known gaps

**Hub 1-hop (deferred, 2026-11-13).** On the default graph, `one_hop_hub` takes 153 ms p50 against
Ladybug's 57 ms (pylance: 440 ms). Fetching a hub's edges is no longer the cost: it takes 5 to 8 ms
warm, since the take cache keeps a dense take's column copy (up to 64 MB) and BTREE dictionary blocks
are decoded once. What remains is fetching the hub's ~20.8k neighbour nodes. A
`to_table(filter="id IN (...)")` over those ids costs about 170 ms, while a `take` of the same row ids
costs 5 ms. Measured phases: filter parse 8 to 13 ms, index candidates 45 to 55 ms, candidate decode
and filter about 6 ms. About 100 ms is not located yet. The 5 to 8 ms, 170 ms, 5 ms and phase figures are
the 2026-11-12 measurements; they were not re-measured in the 2026-11-13 run.

Next steps, in order:

1. In nanolance, take an index-resolved IN's candidate rows directly instead of decoding them
   through the scan path, then find the unlocated ~100 ms.
2. Re-measure cppognee's `cognee_nanograph` adapter through the index. The adapter reads edges by
   node id with `pc.is_in` over cached `source_id` / `target_id` columns. An indexed filter is 3x
   faster for single nodes (7 ms against 22 ms), but `get_neighborhood` was 1.6 to 3.4x slower that
   way, because its induced-edge step issues IN lists of thousands of ids. Step 1 should change that.
   The adapter test `test_node_edge_lookups_read_both_directions_with_quoted_ids` pins the behaviour
   the switch must keep.
