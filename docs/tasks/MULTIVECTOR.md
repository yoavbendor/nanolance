# Task: multivector (ColBERT-style) search -- flat search, indexed search, index builds

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP "Index plan
for AI users", Wave 1, item 3. Three sessions: **M1** flat search, **M2** search of pylance-built
indexes, **M3** builds. Do only the phase you are given.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines. Big ones: `src/vector_search.cpp`, `src/vector_index_build.cpp` (or
wherever `grep -ln "IVF_FLAT" src/*.cpp` points), `bindings/python/nanolance/lance/dataset.py`
(3.5k+). Lance's Rust: read only the functions named below. Cut output: `| tail -15` for pytest (add
`-o addopts=`), `| tail -5` for `tools/pylance_suite.py`.

## 1. Goal

A multivector column is `list<fixed_size_list<float32, dim>>` (also float16 / float64; uint8 with
hamming): each row holds several vectors (ColBERT token embeddings). pylance searches it flat and with
IVF indexes, and builds those indexes. nanolance refuses (`raise unsupported("multivector search")`,
`dataset.py` about line 2752). Targets, in `.deps/pylance-tests/v12.0.0/tests`:
`test_vector_index.py::test_multivec_search_paths` (M1 + M2 + M3), `::test_multivec_ann` (M2, M3),
`::test_multi_vector_field` and `test_indices.py::test_ivf_centroids_multivector_fragment_ids` (M3).

## 2. Facts (measured on pylance 12.0.0 / read in lance 12.0.0 -- do not re-derive)

Lance sources: `/root/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/lance-12.0.0/src` and
`.../lance-linalg-12.0.0/src`.

**Query shape.** `nearest={"column": c, "q": Q, ...}` where Q is one vector (dim) or a list / 2-D
array of vectors (n x dim). Against a multivector column, a single vector is a one-vector query, and a
list of vectors is one multivector query (`dataset/scanner.rs` about lines 1898-1970). Errors
(`ValueError`): `Query vector size 3 does not match index column size 2`; `All query vectors must have
the same length, but got 2 and 3`. Results return the column as `list<fixed_size_list<...>>`
(`results["vector"][0]` has the row's vectors).

**Flat search (M1)** -- `use_index=False`, or rows no index covers: `multivec_distance`
(`lance-linalg/src/distance.rs` about line 354): for each query vector, the minimum distance to any of
the row's vectors, summed over the query vectors. Null or empty rows give NaN (and sort last). Metrics:
L2, cosine, dot for float types; hamming only for uint8; other combinations are refused with the
messages in that function. Without `metric` in the query, flat search uses L2 (measured: with an
IVF_FLAT cosine index on the column, `use_index=False` and no metric gave L2 distances 0, 0.5, 2.0).
The pylance tests pass `metric` explicitly.

**Indexed search (M2)** -- `dataset/scanner.rs` `multivec_ann` (about line 6836) and
`io/exec/knn.rs` `MultivectorScoringExec` (about lines 2807-2990), XTR-style:
1. For each query vector separately: an ordinary ANN search of the index (same nprobes / filters /
   prefilter), with `refine_factor` forced to `DEFAULT_XTR_OVERFETCH` = 10 (env
   `LANCE_XTR_OVERFETCH`, scanner.rs about line 507), sorted by (distance, row id), keeping
   `k * 10` rows.
2. Per query vector: keep the first (best) entry per row id; similarity = `1 - distance`; `min_sim` =
   `1 - ` the last (worst) kept distance.
3. Combine across the query vectors in turn: a row already seen and present in this list adds its
   similarity; already seen but absent adds this list's `min_sim`; new rows start at their similarity
   plus the sum of earlier lists' `min_sim`.
4. Distance = `num_query_vectors - total similarity`; sort by (distance, row id); keep
   `k * refine_factor` (the query's refine factor, default 1), then the usual limit k.
Lance combines the lists in stream-arrival order, so for rows missing from some list the result can
depend on that order. Tests that compare scores exactly must use data where every row is in every
list (k * 10 >= the number of indexed vectors).

Worked example (verified): rows `0: [[1,0],[0,1]]`, `1: [[1,0],[1,0],[0.5,0.5]]`, `2: [[0,1]]`,
IVF_FLAT cosine, 1 partition, query `[[1,0],[0,1]]`, k=3: distances 0.0, 0.2928932, 1.0 (rows 0, 1, 2).
`test_multivec_search_paths` expects ids `[0, 2, 1, 3]` and distances `[0, 0, 1, 2]` for flat, mixed
(indexed + unindexed rows) and fully indexed (`fast_search=True` after `optimize_indices`).

**Index layout (M2 reads, M3 writes)**: the same IVF files as for fixed-size vectors, over the
flattened vectors: `auxiliary.idx` has one row per vector, `_rowid` repeated for each vector of a row
(row 1 above has three entries), vectors normalized for cosine (0.7071, 0.7071);
`storage_metadata` `["{\"dim\":2}"]`; `index.idx` as for IVF_FLAT
(`lance:index {"type":"IVF_FLAT","distance_type":"cosine"}`). IVF_PQ, IVF_SQ and the HNSW kinds are
built the same way over the flattened vectors. Training samples: `index/vector/utils.rs` about lines
88-140 (`estimate_multivector_vectors_per_row`) and 300-320.

## 3. Where things are in nanolance

- Python: `dataset.py` `nearest` parsing (`grep -n "_nearest_query\|multivector" bindings/python/nanolance/lance/dataset.py`).
- C++ search: `include/nanolance/vector_search.hpp`, `src/vector_search.cpp` (the flat path, the IVF
  partition search, refine). The existing KNN handles fixed-size lists only.
- Builds: `grep -ln "train_ivf\|IVF_PQ" src/*.cpp`.
- lance-c: `lance_scanner_nearest_multivector` (`compat/lance-c/include/lance/lance.h`): wire it in M2.
- Existing tests to follow: `bindings/python/tests/test_vector_search.py`, `test_hnsw.py`.

## 4. Phases

- **M1**: query parsing and validation; flat multivector search for each metric and element type;
  result column types; filters and prefilter; NaN for null / empty rows. The flat part of
  `test_multivec_search_paths` passes (with a pylance-built index left unused).
- **M2**: search pylance-built multivector IVF_FLAT, IVF_PQ (with refine), IVF_SQ and IVF_HNSW_* indexes
  by the XTR steps above; mixed indexed / unindexed rows; lance-c. `test_multivec_search_paths` passes
  on pylance's index; `test_multivec_ann` passes on pylance's index.
- **M3**: build every kind nanolance builds over multivector columns, so pylance searches nanolance's
  index with the same answers; `optimize_indices`; the remaining tests.

## 5. Tests you write

`bindings/python/tests/test_multivector.py` (`require_pylance()`), each case on both engines: (a) flat
distances equal pylance's for L2, cosine, dot (float32, float16) and hamming (uint8); (b) on datasets
where every row is in every list, indexed results equal pylance's (rows, order, distances to 1e-6);
(c) M3: each engine searches the other's index with the same results; `auxiliary.idx` rows equal
pylance's for IVF_FLAT; (d) recall@10 of nanolance's IVF_PQ multivector index within 0.03 of pylance's
on a 2,000-row, 8-vectors-per-row dataset (mean over 3 builds: training is random).

## 6. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. C++
without warnings (CI uses `-Werror`); portable (no `__int128`, no unguarded builtins). Search and builds
over fixed-size vector columns must not change or slow down: run `python3 tools/bench_vector_index.py
--rows 20000 --builds 3 --min-seconds 0.5 --work
/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/mv/bench --out
/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/mv/after.json` once at the end
and report its lines (the reviewer compares). Do not commit, push, change branches, or run `--update`.
Scratch files only in `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/mv/`.
Builds: `cd build && ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`. Run each
full suite once, at the end, one at a time; never two at once; no sleep loops; kill by PID.

## 7. Stop and ask (question first in the report)

- the existing KNN code cannot take several vectors per row without restructuring how candidates are
  collected or refined (describe what you would change; do not do it);
- a distance differs from pylance's by more than 1e-6 on data where every row is in every list;
- the index files pylance builds over multivectors differ from the layout above.

## 8. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; the phase's target pylance tests run through nanolance;
`python3 -m pytest -o addopts= -q bindings/python/tests/test_multivector.py | tail -2`; `python3
tools/pylance_suite.py | tail -4`; the benchmark lines; last, once: `cd bindings/python && python3 -m
pytest -o addopts= -q -x -p no:cacheprovider tests | tail -3`. Report under 40 lines: questions first,
then files, per-test status, tails, decisions.

**M1 outcome (2026-10-19):** done in one Haiku session (stopped once by a usage limit, resumed);
reviewed. Flat multivector search (`list<fixed_size_list>` columns: Lance's multivec_distance, the least
distance per query vector summed; L2, cosine, dot, Hamming on uint8; float16 queries rounded as pylance
rounds them); an index over a multivector column is left unused in plain search and refused for
`fast_search` / explicit segments. `tests/test_multivector.py` (25). Reviewer's answers: null and empty
rows are left out of the results (Lance's KNN drops NaN distances; the brief's "sort last" was wrong);
`test_multivec_search_paths` waits for M3 (it builds its index through nanolance); a uint8 multi-query is
refused as pylance refuses it. Open for M2: float64 columns are compared in float32 (pylance's L2 differs
by about one float32 ulp, 1.9e-6 at distances 8-16); a double path needs the binding to carry a double
query.

**M2 split (2026-10-19).** The first M2 session ran out of budget before coding; what it established from
lance-12 (`dataset/scanner.rs` multivec_ann about lines 6836-6920; `io/exec/knn.rs`
MultivectorScoringExec about lines 2807-2990), the facts for an exact float32 match:
- Per query vector: an IVF search with refine_factor 10 (DEFAULT_XTR_OVERFETCH), then a sort by
  (distance, row id) keeping k * 10. Distances are over the flattened vectors.
- Per list: keep each row's first (best) entry; sim = 1 - dist (f32); min_sim = 1 - the last kept dist.
- Combine in stream-arrival order: a row already seen adds its sim (or min_sim when it is not in this
  list); a new row gets sim + the sum of the earlier lists' min_sim. Accumulate in f32, in that order.
- Final distance = number of query vectors - sim (f32); sort (distance, row id); keep k * refine_factor;
  the caller applies limit k.
- Not yet known: how refine works for a multivector column (needed for IVF_PQ with refine and SQ / HNSW),
  and Lance's rule for fragments no index covers (flat scoring, then merged or not). Measure against
  pylance first.
Phases now: **M2a** IVF_FLAT with the steps above and the mixed indexed / unindexed case
(`test_multivec_search_paths`' fully_indexed step); **M2b** IVF_PQ / IVF_SQ / IVF_HNSW_* and the refine
question; **M2c** lance-c (`lance_scanner_nearest_multivector`).

**M2a outcome (2026-10-19):** done in one Haiku session; reviewed. pylance-built multivector IVF_FLAT
indexes (float32) are searched with Lance's XTR steps and match pylance (rows, order, distances to 1e-6;
2 and 8 partitions, nprobes 1 / 2 / all, k 5 / 50, after optimize_indices, fast_search). Mixed case, as
Lance's knn_combined does it: the XTR candidates re-scored exactly, unioned with the exact top k of the
unindexed fragments. pylance 12 builds multivector indexes for cosine only; an L2 query over one is
answered exactly. Refused for now (M2b): refine_factor and distance bounds on a multivector index;
float16 multivector indexes still get the exact (M1) answer. Fixed-size vector search unchanged in speed
(every benchmark search within noise).
