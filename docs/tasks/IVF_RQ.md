# Task: IVF_RQ (RaBitQ) vector indexes -- search pylance's, then build them

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP "Index plan
for AI users", Wave 2, item 6. Three sessions: **R1a** (search a pylance-built default index: fast
rotation, 1-bit + extended codes, the raw-query estimator), **R1b** (the other rotation and estimator
paths, every `num_bits`, cosine and dot), **R2** (build). Do only the phase you are given.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 150k. Lance's RaBitQ is ~9.4k lines
(`/root/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/lance-index-12.0.0/src/vector/bq/`:
storage.rs 4.7k, builder.rs 1.3k, ex_dot.rs 1.1k, dist_table_quant.rs 0.9k, transform.rs 0.7k, prune.rs
0.5k, rotation.rs 0.2k). Never read a file whole: `grep -n "fn "`, then windows of at most ~120 lines,
only the functions named here and what they call. Cut output as usual.

## 1. Goal

pylance builds and searches IVF_RQ indexes; nanolance refuses them (`nanolance.lance does not support
IVF_RQ indexes`). Seven of pylance's tests fail (`.deps/pylance-tests/v12.0.0/tests/test_vector_index.py`):
`test_create_ivf_rq_index` (line 1229), `test_create_ivf_rq_skip_transpose` (1285),
`test_create_ivf_rq_multi_bit_searches_l2_and_cosine` (1322), `test_create_ivf_rq_requires_dim_divisible_by_8`
(1351), `test_create_ivf_rq_mostly_null` (1369), `test_build_rq_model_default_num_bits`,
`test_distributed_ivf_rq_shared_rotation`. The `compat` IVF_RQ tests already pass (they need pylance only).

## 2. Facts (measured on pylance 12.0.0 -- do not re-derive)

Measured case: 3,000 x 64 float32 (standard normal, seed 0), `create_index("vec", "IVF_RQ",
num_partitions=4)` (re-run: `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/rq/`).
- `describe_indices()` details: `{'metric_type': 'L2', 'compression': {'type': 'rq', 'num_bits': 5,
  'rotation_type': 'fast'}, ...}`. Defaults: 5 bits (1 binary bit + 4 extended bits), fast rotation.
- `index.idx`: no rows (`__flat_marker`); metadata `lance:index {"type":"IVF_RQ","distance_type":"l2"}`,
  `lance:flat ["","","",""]`, `lance:ivf` (the IVF model, as for the other kinds nanolance reads).
- `auxiliary.idx`: one row per vector, schema `_rowid: uint64; _rabit_codes: fixed_size_list<uint8>[8]`
  (dim/8 bytes: one bit per rotated dimension), `__add_factors`, `__scale_factors`, `__error_factors`
  (float32), `__blocked_ex_codes: fixed_size_list<uint8>[32]` (4 bits x 64 dims, "blocked" packing),
  `__add_factors_ex`, `__scale_factors_ex` (float32). Metadata `distance_type l2`, `lance:ivf 1`,
  `storage_metadata` = a JSON list of one JSON string: `{"rotate_mat_position":null,
  "fast_rotation_signs":[140,124,...] (32 bytes),"rotation_type":"fast","code_dim":64,"num_bits":5,
  "packed":true,"query_estimator":"raw_query"}`.
- Search (k=5, a query near row 5): indexed distances `0.0990, 62.0608, 64.7492, 67.3567, 67.8608`
  (ids 5, 125, 2137, 419, 2106) against exact `0.0064, 62.1319, 63.8509, 67.1749, 68.4981`: the index
  returns estimated distances, then (no refine) reports them. Same top 5 as exact here.
- Lance's code, the functions to port (storage.rs unless named): the metadata struct (about line 220-300,
  `RabitQueryEstimator`, `rotated_dim`); query rotation `rotate_query_vector_fast_into` (708) /
  `_fast_f32_into` (742) / `_dense_into` (659), with rotation.rs; `prepare_raw_query_context` (374);
  `dist_calculator` (2105) and `raw_query_factor`; `RabitDistCalculator::distance` (1800-1843) and
  `distance_all_with_scratch` (1859); the extended-code distance (`quantize_ex_fastscan_dist_table_into`,
  1606; ex_dot.rs); the blocked layout of `__blocked_ex_codes` (search for "blocked" in storage.rs and
  builder.rs). Build: builder.rs `RabitQuantizer::new_with_rotation` (268), the quantize path and
  `EX_TIGHT_START` (82).

## 3. Phases

- **R1a**: read `auxiliary.idx` / `storage_metadata`; fast rotation; the 1-bit and extended-code
  distance estimate with the raw-query estimator; IVF partition probing as for IVF_PQ (`nprobes`,
  `refine_factor` re-scores with the original vectors); prefilter and deletions as for the other kinds.
  Done when, on pylance-built default indexes (L2; dims 64 and 128; 2 and 16 partitions), nanolance returns
  pylance's rows in pylance's order with distances equal to 1e-5 (relative), with and without refine.
- **R1b**: dense ("matrix") rotation (`rotate_mat_position` set), `num_bits` 1 to 8 (1 = no extended
  codes), cosine and dot, any other `query_estimator` value pylance writes (find which by building with
  each parameter), `skip_transpose`. Same oracle. `stats.index_stats` for IVF_RQ as pylance reports it.
- **R2**: build IVF_RQ (fast rotation by default, dense when asked; seeded as nanolance's other builds
  are; `build_rq_model` and shared rotation for distributed builds; dim not divisible by 8 refused with
  pylance's message; mostly-null columns). Done when pylance searches nanolance's index with the same
  answers nanolance gives, and recall@10 of nanolance's index is within 0.03 of pylance's (mean of 3
  builds each, 20,000 x 128, 64 partitions); all seven target tests pass.

## 4. Tests you write

`bindings/python/tests/test_ivf_rq.py` (`require_pylance()`): per phase, both engines on the same
dataset, as above; plus `tools/bench_vector_index.py`'s KINDS gets IVF_RQ in R2 (the reviewer runs it).

## 5. Rules

`AGENTS.md` sections 5, 6a and 7. No skipped or weakened tests; no `except Exception` around failures. C++
without warnings (`-Werror` in CI); portable (no `__int128`, no unguarded builtins; use the existing
popcount helper if there is one, else a portable fallback). Other vector kinds must not change or slow
down: run `python3 tools/bench_vector_index.py --rows 20000 --builds 3 --min-seconds 0.5 --work
/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/rqw/bench --out
/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/rqw/after.json` once at the
end and report its lines. Do not commit, push, change branches, or run `--update`. Scratch files only in
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/rqw/`. Builds: `cd build &&
ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`. Each full
suite once at the end, one at a time; no sleep loops; kill by PID.

## 6. Stop and ask (question first in the report)

- a distance differs from pylance's by more than 1e-5 and the Rust does not explain why (float order?);
- the existing IVF search path cannot take RaBitQ's estimator without restructuring how partitions are
  scanned (describe the change; do not make it);
- pylance writes a `query_estimator`, rotation or packing this brief does not list.

## 7. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; `python3 -m pytest -o addopts= -q
bindings/python/tests/test_ivf_rq.py | tail -2`; the phase's target pylance tests through nanolance
(`-p nanolance.lance._pytest_plugin`); `python3 tools/pylance_suite.py | tail -4`; the benchmark lines;
last, once: the full pytest. Report under 40 lines: questions first, then files, per-test status, tails,
decisions.

**R1a, first session (2026-10-20):** stopped at a stop-and-ask point; its code is in the tree, not
committed (src/vector_search.cpp: `Storage::Rq`, `load_rq_metadata` (refuses everything but 5 bits / fast
rotation / raw_query / packed / L2, naming each), `load_rq_partition`, `rq_rotate` (fast rotation, with
the non-power-of-two path), `rq_prepare`, `rq_row_codes` / `rq_binary_ip` (packed layout), `rq_distance`
(4-bit blocked extended codes), the hook in `search_segment`). Measured:
- Full estimates equal pylance's to float rounding (max |diff| 3e-5 at distances near 140, dims 64 and
  128, 2 and 16 partitions), once Lance's default binary sum is ported: for full 32-row blocks the u8-
  quantized distance table (affine, ties to even), the remainder rows exact. With refine_factor 4 the
  results equal pylance's.
- **Top k differs**, because Lance's default top-k prunes: a row is skipped when its lower bound
  (`raw_query_binary_distance - error_factor * query_error`, query_error = sqrt(dist(q, centroid))) is not
  below the k-th best of a heap shared across partitions, scanned partition by partition and row by row
  in order. pylance can therefore drop a row whose full estimate is in the top k (dim 128, 2 partitions,
  k 10, seed 5: pylance returns row 1004 at 176.528 and omits 2365 at 174.399).
- Reviewer's decision: **port the gating** (option A), inside the RQ path only: partitions scanned in
  probe order with one shared heap, the gate as Lance computes it (`__error_factors` column), so the rows
  are pylance's. Other index kinds' search does not change. Tests must cover several seeds, k 1 / 10 /
  100, nprobes 1 / 2 / all, with and without refine, and the case above.

**R1a outcome (2026-10-20):** done in two Haiku sessions; reviewed. pylance-built default IVF_RQ indexes
(5 bits, fast rotation, raw_query estimator, packed codes, L2) are searched as pylance searches them:
Lance's u8-quantized binary sums for full 32-row blocks, the 4-bit extended codes, and Lance's top-k gate
(lower bound from `__error_factors` against one heap shared by the probed partitions, in probe order), so
the rows are pylance's even where the gate drops a row whose full estimate would be in the top k. Every
other RQ setting is refused by name (R1b). `tests/test_ivf_rq.py` (30: seeds 0 / 5 / 11, dims 64 / 128,
2 / 16 partitions, k 1 / 10 / 100, nprobes 1 / 2 / all, refine none / 4, prefilter, deletions); 30
repeated runs clean. Reviewer's decision on the tolerance: an RQ estimate is a difference of terms on the
scale of the query-to-centroid distance, so float32 rounding leaves an absolute error on that scale
(measured up to 2.2e-5 next to a row at dim 128, 9e-5 where results reach 280): distances agree to 1e-6 of
max(2 * dim, the largest distance), and rows may swap only with a row whose distance is that close.
Matching Lance's SIMD summation order exactly was judged not worth it. Other index kinds' search speed
unchanged (benchmark searches 0.92x-1.15x, noise).

**R1b outcome (2026-10-20):** done in one Haiku session; reviewed. Every IVF_RQ index pylance 12 writes
is searched as pylance searches it: num_bits 1 to 9 (1: binary codes only, no gate; pylance accepts 9 and
its own target test uses it), unpacked codes, cosine and dot (dot through the rotated centroids, as
Lance's cache does; cosine's query factor measured to be dist(q, c) without Lance's -1, pylance's
distances equal to 1e-8), skip_transpose, and `stats.index_stats` as pylance reports it. pylance never
writes the dense rotation or the residual-query estimator (checked over num_bits 1-9, l2 / cosine / dot,
skip_transpose), so with no oracle they stay refused by name. Reviewer's change: `approx_mode` 'fast' and
'accurate' change pylance's RQ answers (fast: 1-bit scores without the gate; accurate: another distance
table) and were ignored here; they are now refused by name on an IVF_RQ index (other kinds ignore
approx_mode, as pylance does). `tests/test_ivf_rq.py`: 73, five clean runs. Next: R2 (build).
