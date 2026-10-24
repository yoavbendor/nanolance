# Task: IVF_FLAT on binary vectors (Hamming distance)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP "Index plan for
AI users", **item 7** (Wave 2). Two sessions: **H1** (read and search pylance-built Hamming IVF_FLAT
indexes; partition info and clustering), **H2** (build them: k-modes training, `create_index`,
`optimize_indices` delta segments, `IvfModel` save / load). Do only the phase you are given.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines. Big ones: `src/index/vector/vector_search.cpp` (3k+), `src/index/vector/vector_index_build.cpp`,
`src/vector_index.cpp` (or wherever IVF files are decoded: `grep -ln "lance:ivf" src/*.cpp`),
`bindings/python/nanolance/lance/dataset.py` (5k+). Lance's Rust: only the files named here. Cut output:
`| tail -15` for pytest (add `-o addopts=`), `| tail -5` for `tools/pylance_suite.py`.

## 1. Goal

pylance builds IVF_FLAT on `fixed_size_list<uint8>` columns with `metric="hamming"` (binary embeddings,
perceptual hashes). nanolance searches such columns exactly (flat Hamming search is done: `binary_key`
in `src/index/vector/vector_search.cpp` ~2606 and ~3065) but refuses the index: "metric 'hamming' is not supported
(l2, cosine, dot)" / "does not support the hamming distance". After this task nanolance reads, searches,
builds and maintains Hamming IVF_FLAT indexes as pylance does, and each engine uses the other's.

Targets (`.deps/pylance-tests/v12.0.0/tests/`):
- H1: none alone (they build first); H1's own tests use pylance-built indexes.
- H2: `test_vector_index.py::test_ivf_flat_over_binary_vector`,
  `::test_ivf_flat_respects_index_metric_binary`, `test_indices.py::test_ivf_centroids_hamming`,
  `test_vector.py::test_hamming_clustering_multi_segment[8]`, `[16]`.

## 2. Facts (measured on pylance 12.0.0 -- do not re-derive)

Probe: `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/bin/m.py` (1000 rows,
`vector` fixed_size_list<uint8>[16], `id` int32; IVF_FLAT, 4 partitions, hamming).

Files under `_indices/<uuid>/`:
- `index.idx`: schema `__flat_marker: uint64 not null`, 0 rows; metadata `lance:ivf` = `1`,
  `lance:index` = `{"type":"IVF_FLAT","distance_type":"hamming"}`, `lance:flat` = `["","","",""]` (one
  empty string per partition). The IVF model (centroids, offsets, lengths) is where it is for float
  IVF_FLAT; the centroids tensor holds **uint8** values (dimension = bytes per vector, 16 here). Check
  the tensor's data type field with nanolance's IVF decoder on the probe's index and record it in your
  report.
- `auxiliary.idx`: `_rowid: uint64`, `flat: fixed_size_list<uint8>[16]`, rows grouped by partition;
  metadata `distance_type` = `hamming`, `lance:ivf` = `1`, `storage_metadata` = `["{\"dim\":16}"]`.

Search:
- Distance: number of differing bits (popcount of XOR), as `float` in `_distance` (0.0, 48.0, ...).
- The query's metric defaults to the index's (`test_ivf_flat_respects_index_metric_binary`: no
  `metric` given, the index's hamming is used; ids `[1, 0, 2]` for query `[128]` over `[0],[128],[255]`).
- Partition choice: the probed partitions are the centroids nearest the query by Hamming distance
  (Lance `lance-index-12.0.0/src/vector/kmeans.rs` `kmeans_find_partitions_binary`, ~line 1635); probes
  as for float IVF (minimum_nprobes 1, adaptive).
- Plan text: `ANNSubIndex: name=vector_idx, k=5, deltas=1, metric=Hamming` over `ANNIvfPartition:
  uuid=..., minimum_nprobes=1, maximum_nprobes=None, deltas=1`.
- `stats.index_stats`: `metric_type` `hamming` at the top level of each index entry and in
  `sub_index` (`{"dim": 16, "index_type": "FLAT", "metric_type": "hamming"}`); `centroids` as lists
  of floats of the uint8 values (`96.0, 44.0, ...`).

Python API (`lance.vector`):
- `get_ivf_partition_info(dataset, index_name, index_segments=None)` -> list of
  `{"partition_id": i, "size": n}` (summed over the selected segments).
- `hamming_clustering_for_ivf_partition(dataset, index_name, partition_id, threshold,
  index_segments=None)` -> RecordBatchReader of `representative: uint64 not null`,
  `duplicates: list<uint64> not null`: the partition's row ids and hashes concatenated across the
  selected segments, pairwise Hamming distances <= threshold, clustered by union-find
  (Lance `lance-12.0.0/src/index/vector/hamming.rs` ~220-340). nanolance's
  `hamming_clustering_for_sample` (`bindings/python/nanolance/lance/vector.py` ~63) already does the
  pairwise + union-find step: reuse it. Errors: an out-of-range partition ("Partition ID {p} is out of
  range (0..{n})"), an unknown segment ("invalid index segment uuid" -- the test matches this text).

Training (H2): Lance's k-modes (`kmeans.rs` `KModeAlgo`, ~644-725): assignment = nearest centroid by
Hamming distance (plus Lance's balance term, as in float k-means); update = per bit, 1 when more than
half of the cluster's vectors have it set (`c * 2 > cnt`; a tie is 0), bits least-significant first
within each byte. Initialization and iteration count as Lance's float k-means
(`train_kmeans`, ~987). Centroids are not reproducible bit for bit (Lance's k-means is unseeded):
tests compare behaviour (partition count = `round(sqrt(num_rows))` when `num_partitions` is unset,
search results), not centroids.

## 3. Where things are in nanolance

- Flat Hamming search: `src/index/vector/vector_search.cpp` (`hamming` ~503, `binary_key`, the uint8 branch ~3065).
- IVF read / search / build: `grep -n "IVF_FLAT\|lance:flat\|storage_metadata" src/*.cpp`; the float
  IVF_FLAT path is the model to follow. `NearestQuery` (`include/nanolance/vector_search.hpp` ~34)
  carries the key as `std::vector<float>`; the uint8 flat path already passes bytes beside it.
- The refusals to lift: `grep -rn "hamming" bindings/python/nanolance/lance/*.py src/api/lance_c.cpp`.
- `IndicesBuilder` / `IvfModel`: `bindings/python/nanolance/lance/indices/builder.py`.
- `optimize_indices` delta segments: `include/nanolance/index_optimize.hpp`.

**Design call (decided):** do not template the float vector path over the element type. Keep one
uint8 code path beside it: centroids and stored vectors as bytes, distances via `hamming`, partition
assignment and k-modes training in their own functions. Reuse the IVF file reading / writing and
partition bookkeeping unchanged.

## 4. Phases

- **H1 (read and search).** Decode pylance's Hamming IVF_FLAT (uint8 centroids); search it with
  `nearest` (metric from the index when the query gives none; a float metric on a uint8 column keeps
  pylance's error), prefilters and deletions as for float IVF_FLAT; `stats.index_stats`,
  `explain_plan` text; `get_ivf_partition_info` and `hamming_clustering_for_ivf_partition` (with
  `index_segments`) in `nanolance.lance.vector`; lance-c `LANCE_METRIC_HAMMING` searches over such an
  index where lance-c takes uint8 queries today (else leave lance-c as is and say so). Tests use
  indexes pylance builds.
- **H2 (build).** `train_ivf(distance_type="hamming")` (k-modes, uint8 centroids), `IvfModel`
  save / load with `distance_type` hamming (byte-compatible with pylance's file: each engine loads the
  other's), `create_index(index_type="IVF_FLAT", metric="hamming")` writing the files of section 2,
  `optimize_indices` delta segments on such an index (`num_indices_to_merge=0` gives a second segment,
  as `test_hamming_clustering_multi_segment` needs), and pylance searches nanolance's index with
  results equal to its own over the same data. The five targets pass.

## 5. Tests you write

`bindings/python/tests/test_binary_ivf.py` (`require_pylance()`): H1 -- pylance builds, nanolance
searches (k 1..50, with and without a filter, after a delete, nprobes 1 and all) with the same ids and
distances as pylance on the same index; stats and plan text equal; partition info and clustering
equal, across two segments. H2 -- nanolance builds, pylance searches with nanolance's results (recall
against brute force at least pylance's own on the same data with nprobes=all equal to brute force);
`IvfModel` files cross-loaded; delta segment after an append.

## 6. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. The
local native build has `-DNANOLANCE_WERROR=ON`: warnings fail it; delete helpers you stop using (the
wasm clang build fails on unused functions). Check the wasm build: `source /home/user/emsdk/emsdk_env.sh`,
configure as in `.github/workflows/wasm.yml` into a directory under /tmp, build, delete it. Do not add
fields to Python classes that mirror pylance's. Float vector search must not change (results and
speed: `tools/bench_vector_index.py` before / after on one float dataset, report both). Do not commit,
push, change git branches, or run `--update`. Scratch files only in
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/bin-work/`. Builds: `cd build &&
ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`. Disk is
small (~5 GB free): delete `/tmp/pytest-of-root` and `/tmp/tmp*` after test runs. Run each full suite
once, at the end; the bindings suite with `| grep -E "^(FAILED|ERROR)|passed|failed" | tail -15`.

## 7. Stop and ask (question first in the report)

- the centroids tensor of a pylance Hamming index is not uint8 (record what it is);
- a target needs a change to how float IVF indexes are written;
- pylance cannot open nanolance's index and the difference is not in section 2.

## 8. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; `python3 -m pytest -o addopts= -q
bindings/python/tests/test_binary_ivf.py | tail -2`; the targets through nanolance (`cd
.deps/pylance-tests/v12.0.0 && python3 -m pytest -o addopts= -q -p nanolance.lance._pytest_plugin
tests/test_vector_index.py tests/test_indices.py tests/test_vector.py -k "hamming or binary" | tail -5`);
`python3 tools/pylance_suite.py | tail -4`; last, once, the full bindings suite. Report under 40 lines:
questions first, then files, per-target status, tails, decisions.

**H1 outcome (2026-10-20):** nanolance searches pylance's Hamming IVF_FLAT indexes (centroids tensor
data_type UINT8 = 4; rows kept as bytes; Lance's global top-k heap over the probed partitions, so among
equal distances the row met first stays, as Lance's IVF_FLAT), with stats, describe_indices
("HAMMING"), VectorIndexReader, `get_ivf_partition_info` and `hamming_clustering_for_ivf_partition`
(segments, pylance's errors). `hamming_clustering_for_sample` now uses union-find as pylance's does (it
was greedy). `bindings/python/tests/test_binary_ivf.py` (22). Reviewer: the Hamming steps moved out of
line (`hamming_to_centroids`, `hamming_into_heap`) and a dead Hamming branch removed; float IVF_FLAT
search interleaved A/B (k=10, 100k x 128, 4 alternations): 1.12 vs 1.15 ms median, within noise.
Decisions for H2: keep nanolance's plan text format (pylance's differs for float IVF too); lance-c
`LANCE_METRIC_HAMMING` over uint8 queries in H2, with Hamming still refused on float scanners
(`tests/api/test_lance_c_search.cpp` ~317 stays); benchmarks: never overwrite `bench/results/*.json`
(copy it back after a run).

**H2 outcome (2026-10-20): task done.** nanolance builds Hamming IVF_FLAT indexes (Lance's k-modes:
Hamming assignment, per-bit majority update; uint8 centroids and rows; pylance's files and metadata,
details metric_type 3), `train_ivf(distance_type="hamming")`, `IvfModel` save / load cross-compatible
with pylance, `optimize_indices` delta segments, `create_index_uncommitted`; lance-c scanners take
`LANCE_METRIC_HAMMING` with a uint8 query (still refused with a float one). The five targets pass (added
to the expected list: 1239). Float IVF_FLAT build and search unchanged (within noise). Left, accepted:
no partition rebalancing for Hamming indexes in `optimize_indices` (sizes can drift from pylance's after
large appends); k-modes trains flat above 256 partitions (Lance goes hierarchical); supplied
`ivf_centroids` refused for Hamming; lance-c index creation with Hamming refused.
