# Task: deferred index remap and the Fragment Reuse Index; the Compaction plan / execute / commit API

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP "Index plan
for AI users", Wave 1, item 4. Two sessions: **R1** (read pylance's FRI), **R2** (write it, and the
`Compaction` API). Do only the phase you are given.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines. Big ones: `src/index/index_search.cpp`, `src/dataset/dataset_ops.cpp`,
`src/dataset/index_optimize.cpp`, `src/index/vector/vector_search.cpp`, `src/index/fts/fts_search.cpp`,
`bindings/python/nanolance/lance/dataset.py` (3.5k+). Lance's Rust: read only the files named here.
Cut output: `| tail -15` for pytest (add `-o addopts=`), `| tail -5` for `tools/pylance_suite.py`.

## 1. Goal

`compact_files(defer_index_remap=True)` (and `Compaction.commit(..., options={"defer_index_remap":
True})`) compacts without rewriting the indexes: it adds a system index `__lance_frag_reuse` that maps
the old row addresses to the new ones, and the indexes keep their old row addresses while their
fragment coverage moves to the new fragments. Searches translate through it. nanolance today, on such
a dataset, ignores every scalar index (`index_candidates` in `src/index/index_search.cpp` about line 1307
returns early: correct, but a full scan), hides the system index from `list_indices` /
`describe_indices`, and cannot write one. Targets in `.deps/pylance-tests/v12.0.0/tests/test_optimize.py`
(lines 500-590): `test_defer_index_remap`, `test_defer_index_remap_via_commit_options[True|False]`,
`test_describe_indices_matches_list_indices_for_frag_reuse`; and the `Compaction.plan` tests the suite
lists as failing (`grep -n "Compaction.plan" .deps/pylance-tests/v12.0.0/tests/*.py`).

## 2. Facts (measured on pylance 12.0.0 -- do not re-derive)

Measured case (re-run: `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/fri/`
has the dataset `f.lance`): 6,000 rows in 6 fragments of 1,000; BTREE on `i`; `delete("i < 500")`;
`compact_files(target_rows_per_fragment=2000, defer_index_remap=True, num_threads=1)` ->
`CompactionMetrics(fragments_removed=6, fragments_added=2, files_removed=7, files_added=2)`; fragments
6 (2,500 rows) and 7 (3,000).
- `list_indices()`: `{'name': '__lance_frag_reuse', 'type': 'FragmentReuse', 'fields': [],
  'fragment_ids': {6, 7}, 'version': 3}` and `{'name': 'i_idx', 'type': 'BTree', 'fragment_ids': {6, 7},
  'version': 1}`. The BTree's files are unchanged (old row addresses inside); only its coverage moved.
  `describe_indices()` agrees with `list_indices()` on every name and type.
- The FRI has no files under `_indices/`: its content is in `IndexMetadata.index_details`, a
  `FragmentReuseIndexDetails` (lance-table 12.0.0 `protos/table.proto` lines 645-685): `oneof content {
  InlineContent inline = 1; ExternalFile external = 2; }` (inline below 200 KB); `InlineContent {
  repeated Version versions = 1; }`; `Version { uint64 dataset_version = 1; repeated Group groups = 3;
  }`; `Group { bytes changed_row_addrs = 1; repeated FragmentDigest old_fragments = 2; repeated
  FragmentDigest new_fragments = 3; }`; `FragmentDigest { uint64 id = 1; uint64 physical_rows = 2;
  uint64 num_deleted_rows = 3; }`. `changed_row_addrs` is a serialized RoaringTreemap of old row
  addresses (nanolance already reads RowAddrTreeMap: `decode_row_addr_treemap`); with the old and new
  fragments it gives the old -> new address mapping (deleted rows map to nothing).
- `filter="i = 4321"` uses `i_idx` (`ScalarIndexQuery: query=[i = 4321]@i_idx`) and returns the row.
  After `optimize_indices()` both indexes still cover {6, 7} and the FRI remains.
- Lance's code: the mapping, `lance-index-12.0.0/src/frag_reuse.rs` (181 lines: `FragReuseIndex`,
  `remap_row_id`, `remap_row_addrs_tree_map`); loading and applying it,
  `lance-12.0.0/src/index/frag_reuse.rs`; writing it and the deferred compaction,
  `lance-12.0.0/src/dataset/index/frag_reuse.rs` and `lance-12.0.0/src/dataset/optimize.rs`
  (`grep -n "defer_index_remap" .../optimize.rs`). Sources under
  `/root/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/`.
- The table feature flags for the FRI (bits 9, 10) are not in Lance 12.0.0: do not set any flag.

## 3. Where things are in nanolance

- `src/index/index_search.cpp` (the early return about line 1307), `src/index/vector/vector_search.cpp` and
  `src/index/fts/fts_search.cpp` (how each maps an index's row addresses to fragments), `src/table/index_maintenance.cpp`
  (`kFragReuseIndex`, kept as a system index), `src/dataset/index_segments.cpp` (system indexes are not user
  indexes).
- Compaction: `include/nanolance/dataset_ops.hpp` (`CompactionOptions`, `reindex`), `src/dataset/dataset_ops.cpp`.
- Python: `dataset.py` `list_indices` / `describe_indices` / `optimize.compact_files`; the `Compaction`
  class (`grep -rn "class Compaction\|Compaction.plan" bindings/python/nanolance/lance/`).

## 4. Phases

- **R1** (read): decode the FRI (inline and external); every index kind (scalar, vector, full-text)
  searched through it on pylance-made datasets, with the same rows as pylance; the early return goes;
  `list_indices` / `describe_indices` / `stats` show the system index as pylance does;
  `optimize_indices` and later compactions keep or drop it as pylance does (measure each with pylance in
  your test, both engines on the same dataset). `test_describe_indices_matches_list_indices_for_frag_reuse`
  passes on pylance-written data.
- **R2** (write): `CompactionOptions.defer_index_remap` (C++), `compact_files(defer_index_remap=True)`,
  and `Compaction.plan` / `CompactionTask.execute` / `Compaction.commit(options=...)` in Python; the
  FRI nanolance writes is read by pylance with the same results. All target tests pass.

## 5. Tests you write

`bindings/python/tests/test_frag_reuse.py` (`require_pylance()`): for BTREE, BITMAP, LABEL_LIST,
INVERTED, IVF_FLAT and IVF_PQ, a dataset compacted with `defer_index_remap=True` by each engine, then
searched by both: the same rows (and scores or distances), and the plan names the index (no full scan);
deletes before and after the compaction; two deferred compactions in a row (two `Version` entries);
`optimize_indices` afterwards; with and without stable row ids.

## 6. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. C++
without warnings (CI uses `-Werror`). Datasets nanolance writes today must not change; an eager
compaction (`defer_index_remap` unset) behaves exactly as now. Searches on datasets without an FRI must
not slow down (the remap is skipped entirely when there is none). Do not commit, push, change branches,
or run `--update`. Scratch files only in
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/fr/`. Builds: `cd build &&
ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`. Run
each full suite once, at the end, one at a time; never two at once; no sleep loops; kill by PID.

## 7. Stop and ask (question first in the report)

- one index kind cannot be searched through the remap without restructuring its search;
- pylance's behaviour after `optimize_indices` or a second compaction is not what the Rust above
  suggests;
- matching pylance needs a change to eager compaction.

## 8. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; the target pylance tests run through nanolance; `python3 -m
pytest -o addopts= -q bindings/python/tests/test_frag_reuse.py | tail -2`; `python3
tools/pylance_suite.py | tail -4`; last, once: `cd bindings/python && python3 -m pytest -o addopts= -q
-x -p no:cacheprovider tests | tail -3`. Report under 40 lines: questions first, then files, per-test
status, tails, decisions.

**R1 outcome (2026-10-20):** done for scalar indexes in one Haiku session; reviewed. The FRI is decoded
(inline and external `details.binpb`, the per-version remap from `changed_row_addrs` and the digests;
cached per dataset and uuid); BTREE, BITMAP, ZONEMAP and BLOOMFILTER answers are remapped (deleted rows
drop, moved rows land in their new fragments; not with stable row ids, whose ids do not move), coverage
is remapped by Lance's straddle rule, `list_indices` / `describe_indices` show `__lance_frag_reuse` as
pylance does, and `optimize_indices` no longer rebuilds an index the FRI still covers.
`tests/test_frag_reuse.py` (9). Reviewer's decisions on the session's questions:
- Vector and full-text indexes after a deferred remap are answered by an exact scan of the rewritten
  fragments (right rows, index not used). Translating their candidate addresses is its own phase,
  **R1b** (vector: the candidate ids in the IVF / HNSW loops and the count fill; FTS: postings and
  document lengths keyed by row address).
- pylance 12.0.0 answers wrongly after an eager compaction on top of a deferred one (an indexed `i = 600`
  returns no row where a scan returns one; its BTREE keeps stale addresses). nanolance does not copy
  wrong answers: on its own eager compaction it keeps the results right (its index metadata then differs
  from pylance's); on a dataset pylance left in that state it reads what is on disk, as pylance does.
  Recorded for an upstream report.
- Index listing order (pylance sorts by name; nanolance keeps manifest order): a small item, not FRI's.
The pylance targets (`test_defer_index_remap*`, `test_describe_indices_matches_list_indices_for_frag_reuse`)
need nanolance's `compact_files(defer_index_remap=True)`: R2.

**R2 outcome (2026-10-20):** done in one Haiku session; reviewed. `compact_files(defer_index_remap=True)`
(and the commit-options form) writes the FRI as pylance writes it (in the rewrite's version, index
version 3, prior versions kept), which pylance and R1's reader both read; the other indexes' coverage is
swapped for whole rewritten groups, and nothing is re-indexed. `lance.Compaction.plan` / `.commit`,
`CompactionPlan`, `CompactionTask.execute`, `RewriteResult` (C++: `dataset_compact_plan` / `_execute` /
`_commit`; eager compaction now runs through the same three steps). Review: eager compaction's output
unchanged (fragments, rows, contents and index answers equal to the previous build's on plain, row-target,
byte-limit and delete cases). Five more of pylance's tests pass (expected list 1219). Open:
`test_optimize_max_bytes` wants the byte limit cut at batch granularity (2 fragments where nanolance
writes 8): it changes eager output, so it is a decision, not done; `defer_index_remap` on a dataset with
stable row ids is refused; an FRI larger than 200 KB (external details) is refused when writing.
