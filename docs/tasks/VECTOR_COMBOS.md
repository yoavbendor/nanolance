# Task: IVF_SQ, IVF_HNSW_PQ, IVF_HNSW_FLAT vector indexes (recombinations of existing pieces)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Roadmap order item 4.

## 0. Context budget

About 100k tokens per session. `grep -n` then `sed -n` windows of at most ~120 lines; `src/vector_search.cpp`
(2k+) and `src/vector_index_build.cpp` (1.4k+) must never be read whole. Cut all output (`| tail -15`,
pytest with `-o addopts=` for a summary line; `tools/pylance_suite.py ... | tail -5`;
`ninja 2>&1 | grep -E "error|warning:" | head`). Only your phase.

## 1. The idea (measured on pylance 12.0.0 here)

A Lance IVF vector index is two files in `_indices/<uuid>/`: **`auxiliary.idx`** = the *storage*
(per-row codes, `_rowid` + one column) and **`index.idx`** = the *sub-index* over each partition.
Storage kinds: FLAT (`flat: fixed_size_list<float>[d]`, metadata `storage_metadata = ["{\"dim\":d}"]`),
PQ (`__pq_code: fixed_size_list<uint8>[m]`, the codebook in a global buffer), SQ (`__sq_code:
fixed_size_list<uint8>[d]`, `storage_metadata = ["{\"dim\":d,\"num_bits\":8,\"bounds\":{\"start\":..,\"end\":..}}"]`).
Sub-index kinds: FLAT (`index.idx` with zero rows, schema `__flat_marker: uint64 not null`, metadata
`lance:flat = ["","",...]` one empty string per partition) and HNSW (`__vector_id: uint32,
__neighbors: list<uint32>, _distance: list<float>`, metadata `lance:hnsw = [ {entry_point, params,
level_offsets}, ... ]` per partition). Both files carry `lance:ivf` (a global buffer index of the IVF
model) and `distance_type`; `index.idx` carries `lance:index = {"type": "<NAME>", "distance_type": ..}`.
Manifest `details_type_url` = `/lance.index.pb.VectorIndexDetails` for all of them.

nanolance already reads, searches and builds three of the six pairings:

| name | storage | sub-index | in nanolance |
|---|---|---|---|
| IVF_FLAT | FLAT | FLAT | yes |
| IVF_PQ | PQ | FLAT | yes |
| IVF_HNSW_SQ | SQ | HNSW | yes |
| **IVF_SQ** | SQ | FLAT | **this task** |
| **IVF_HNSW_PQ** | PQ | HNSW | **this task** |
| **IVF_HNSW_FLAT** | FLAT | HNSW | **this task** |

Measured examples (2000 x 16 float32, 4 partitions): IVF_SQ `auxiliary.idx` 35737 bytes, identical
in layout to IVF_HNSW_SQ's; its `index.idx` 592 bytes, 0 rows, 2 global buffers. IVF_HNSW_PQ: PQ
`auxiliary.idx` (2000 rows, storage_metadata with `codebook_position`, `nbits`, `num_sub_vectors`,
`dimension`, `transposed`...) + HNSW `index.idx` exactly like IVF_HNSW_SQ's. IVF_HNSW_FLAT: FLAT
`auxiliary.idx` (as IVF_FLAT's storage) + HNSW `index.idx`.

So the work is to **decouple storage from sub-index** where the code assumes the three known pairs:
find them with `grep -n "IVF_HNSW_SQ\|IVF_PQ\|IVF_FLAT\|hnsw\|__sq_code\|__pq_code\|__flat_marker"
src/vector_search.cpp src/vector_index_build.cpp include/nanolance/vector*.hpp`, and the Python names
that refuse these kinds (`grep -n "IVF_SQ\|HNSW_PQ\|HNSW_FLAT\|does not support .* indexes" 
bindings/python/nanolance/lance/*.py`). Reuse the SQ / PQ / FLAT distance code and the HNSW graph
build / search that exist -- do not write new ones. Build parameters (`num_bits`, `m`,
`ef_construction`, `max_level`, `num_sub_vectors`...) as pylance takes them; defaults as pylance's.
Before writing a builder, build the same index with pylance and with nanolance on the same data and
compare: schema, metadata keys, row counts, and that each engine's search over the other's index
returns the same top-k (with `nprobes` = all partitions and a large `ef`, results must match exactly).

## 2. Phases

- **C1 -- read + search** pylance-built IVF_SQ, IVF_HNSW_PQ, IVF_HNSW_FLAT (all metrics nanolance
  supports: l2, cosine, dot), with prefilters, deletions, refine_factor, as the existing kinds do.
- **C2 -- build** them (`create_index(index_type=...)`, distributed `create_index_uncommitted`,
  `optimize_indices` deltas), readable by pylance.

Oracle (pylance's tests): grep `.deps/pylance-tests/v12.0.0/tests/test_vector_index.py` for
`IVF_SQ`, `IVF_HNSW_PQ`, `IVF_HNSW_FLAT`, `HNSW_PQ`, `HNSW_FLAT` (e.g. `test_create_ivf_sq_index`,
`test_index_type`, `test_distributed_vector[...IVF_SQ...]`); run them with
`cd /home/user/nanolance && python3 tools/pylance_suite.py -k "<names>" 2>&1 | grep -E "^FAILED|test suite"`
(a `tests/test_datagen.py` collection error shows with `-k`; ignore it).

## 3. Tests you write

Extend `bindings/python/tests/test_hnsw.py` (or `test_vector_search.py`, following its style) with
the three kinds: each engine builds, the other searches, top-k equal under exhaustive settings;
`describe_indices()` / `stats` equal.

## 4. Rules, builds, stop rules

`AGENTS.md` sections 5 and 7 (grep the headings). No skipped / weakened tests. C++ without warnings.
Indexes nanolance builds today (IVF_FLAT / IVF_PQ / IVF_HNSW_SQ) must stay byte-identical. Do not
commit, push, change branches, or run `--update`. Builds: `cd build && ninja`, then
`cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
Stop and ask (question first) if pylance's files differ from section 1, if a kind needs a distance or
quantizer nanolance lacks (RaBitQ / IVF_RQ, Hamming are out of scope), or if decoupling would change
an existing kind's bytes.

## 5. Done per phase -- report (under 40 lines)

Tails of: `cd build && ctest -j8 | tail -3`; the oracle `-k` run; your test file;
`python3 tools/pylance_suite.py | tail -4` ("all N expected tests passed"); last, once, quiet:
`cd bindings/python && python3 -m pytest -o addopts= -q -x -p no:cacheprovider tests | tail -3`.
Files changed, per-test status, decisions, questions.

**C1 investigation (2026-10-15), no code changed -- facts for the implementing session:**
- pylance's files match section 1 (probe scripts: `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/c1/`
  -- `build_pylance.py` builds all kinds, `meta.py` dumps them).
- Today a pylance IVF_SQ / IVF_HNSW_PQ / IVF_HNSW_FLAT index is **silently not used**: `load_ivf`
  (src/vector_search.cpp ~750) refuses the type, `dataset_nearest` falls back to an exact search and
  the plan says why (results exact, so they differ from pylance's approximate ones). Keep that
  fallback for kinds still unsupported (reviewer's call).
- Coupling points to decouple (vector_search.cpp): type whitelist ~750 and `pq` / `hnsw` flags from
  the name ~754; the storage column choice in `partition()` ~664; `load_hnsw_partition` ~542-566
  assumes uint8 codes of stride `dim`; the PQ codebook loads only when `pq` ~866-900; `score` ~1534
  dispatches HNSW first and `hnsw_partition` ~1446 / `hnsw_flat` ~1394 hard-wire `SqDistance` ~1268;
  the non-HNSW exhaustive branch ~1561 expects float vectors (IVF_SQ needs an SQ distance there).
  `describe_indices` raises through `vector_index_details` -> `load_ivf`.
- Distances (Lance's source, checked by the reviewer): the IVF layer hands each storage the query, as a
  residual (query - partition centroid) only when the quantizer uses residuals -- PQ for L2 / cosine
  (`PQBuildParams::use_residual`), never for SQ or FLAT. Inside HNSW the storage's distance calculator
  is used unchanged: PQ = the PQ distance table built on that (residual) query, exactly as nanolance's
  IVF_PQ path does today; SQ = `SqDistance`; FLAT = plain float distance.
- Order (agreed): (a) IVF_HNSW_FLAT, (b) IVF_SQ, (c) IVF_HNSW_PQ. Tests go in
  `bindings/python/tests/test_hnsw.py`.
- C2 will need: lance_c.cpp ~2459 (`vector_options` refuses these kinds at build), dataset.py ~1136 and
  ~1280 (create_index refusals), and `tests/test_lance_c_search.cpp` ~358 asserts an IVF_HNSW_PQ build
  is refused.
