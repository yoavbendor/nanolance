# Task: INVERTED indexes on nested fields (item 10) and the table-format leftovers (T5)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP Wave 3, item
**10** and **T5**. Two sessions: **N** (full-text indexes on struct fields), **S** (spec leftovers). Do
only the phase you are given.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines. Big ones: `src/fts_index_build.cpp`, `src/fts_search.cpp`,
`bindings/python/nanolance/lance/dataset.py` (5k+), `src/dataset.cpp`. Cut output: `| tail -15` for
pytest (add `-o addopts=`), `| tail -5` for `tools/pylance_suite.py`.

## 1. Goal and targets

- **N:** pylance builds INVERTED indexes on struct leaves (`meta.title`, `meta.body.text`) and searches
  them by path; nanolance refuses ("an INVERTED index on a nested column is not supported"). Targets:
  `tests/test_scalar_index.py::test_nested_field_fts_index`,
  `::test_multiple_nested_field_fts_indices_e2e`.
- **S:** `tests/test_dataset.py::test_migrate_manifest` (nanolance lacks
  `lance.lance.manifest_needs_migration` / `LanceDataset.migrate_manifest_paths_v2`), and reading the
  manifest's inline transaction.

(`.deps/pylance-tests/v12.0.0/`.)

## 2. Facts (measured on pylance 12.0.0 -- do not re-derive)

**N** -- probe `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/w3/nested.py`
(`python3 nested.py py|nl`; outputs `py.txt` = pylance, `nl.txt` = nanolance today). Six rows, `id`
int64, `meta` struct<title: string, body: struct<text: string>> with a null `meta`, a null
`meta.title`, a null `meta.body`.
- `create_scalar_index("meta.title", "INVERTED")` -> index `meta.title_idx`, `fields`
  `['meta.title']`, type `Inverted`; same for `meta.body.text` (two indexes, each its own
  metadata.lance / part_0_{docs,invert,tokens}.lance -- the same files as a top-level INVERTED index).
- `full_text_query={"columns": ["meta.title"], "query": "apple"}` -> ids `[5, 1, 0]`, scores
  `[0.50625, 0.37366, 0.31387]`; `full_text_query="apple"` (no columns) uses the nested index too (same
  result); `MatchQuery("apple", "meta.title")` the same; `meta.body.text` / "fruit" -> `[2, 0, 1]`,
  `[0.43250, 0.33698, 0.33698]`. A null struct or null leaf is a document with no tokens.
- After an append (unindexed fragment): `[5, 5, 1, 1, 0, 0]` with scores
  `[0.50625, 0.46189, 0.37366, 0.34092, 0.31387, 0.28637]` (the unindexed rows are scored flat, as for
  top-level columns).
- Index field id: the leaf's field id (how nanolance resolves nested paths for scalar indexes: ROADMAP
  T1.3 "Nested field paths" -- reuse that resolution).

**S** -- probe `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/w3/t5.py`.
- pylance 12 writes manifest field **21** (`transaction_section`: the transaction inline in the manifest
  file) **and** `_transactions/*.txn`. nanolance writes only the `.txn`. With the `.txn` files removed,
  pylance's `read_transaction(v)` still answers (from field 21); nanolance's returns None. Make
  nanolance read field 21 when there is no transaction file -- in `read_transaction` and in the C++
  conflict classifier (`src/commit_conflicts.cpp` / `classify_concurrent` in `src/dataset.cpp`: today a
  version without a transaction file is a retryable conflict; with field 21 it is decided normally).
  Lance's layout of field 21: `lance-table-12.0.0/src/format/manifest.rs` and `lance-12.0.0/src/io/commit.rs`
  (grep `transaction_section`). Do **not** write field 21 (decided: no change to what nanolance writes).
- `_versions/latest_version_hint.json` (`{"version":N}`, pylance writes it on every commit): decided **not
  written** (pylance opens without it; revisited with object-store writes, ROADMAP item 8).
- `version_aux_data`: always 0 / absent in pylance 12's manifests: **not needed**.
- Migration to stable row ids: pylance 12 has no API for it: **not needed**.
- `migrate_manifest_paths_v2` / `manifest_needs_migration`: implement as pylance does (V1 manifest names
  `{version}.manifest` renamed to V2 `{u64::MAX - version}` names, 20 digits; read
  `lance-12.0.0/src/dataset.rs` `migrate_manifest_paths_v2` and the test for the exact behaviour).

## 3. Where things are in nanolance

- INVERTED build / search: `src/fts_index_build.cpp`, `src/fts_search.cpp`; the nested refusal:
  `grep -rn "nested column is not supported" src bindings/python/nanolance`.
- FTS on list columns (ROADMAP Wave 1 item 1, `docs/tasks/FTS_LISTS.md`) changed how a document's text
  is collected from a column: the nested case is another source of text, not a new index format.
- Nested path resolution for scalar indexes: `grep -n "resolve.*path\|case-insensitive" src/*.cpp`.
- Transactions: `read_transaction` in `dataset.py` ~2011; manifest reading in `src/dataset.cpp`;
  V1 / V2 manifest names in `src/manifest_writer.cpp` (nanolance already reads both).

## 4. Phases

- **N:** build, search (by path, default columns, MatchQuery / phrase / boolean queries as for top-level
  columns), maintain (optimize_indices, compaction re-cover) INVERTED indexes on struct leaves at any
  depth; each engine uses the other's nested index with equal results; lance-c FTS on a nested column
  where lance-c takes a column name. The two N targets pass.
- **S:** field 21 read (both places), `manifest_needs_migration` and `migrate_manifest_paths_v2`, and the
  decisions above recorded in ROADMAP T5. The S target passes.

## 5. Tests you write

N: `bindings/python/tests/test_fts_nested.py` (`require_pylance()`): the probe's cases through both
engines, ids and scores equal (scores to 1e-6), plus a pylance-built nested index searched by nanolance
and the reverse, an append, a delete, optimize_indices. S: in `bindings/python/tests/test_small_apis.py`
or a new file: a pylance dataset with its `.txn` files removed reads its transactions through nanolance
and a nanolance commit racing it is classified normally (C++ two-writer test in
`tests/test_commit_conflicts.cpp`); migration of a V1-named dataset (write one with pylance's
`enable_v2_manifest_paths=False`), then both engines open it.

## 6. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. The
local native build has `-DNANOLANCE_WERROR=ON`: delete helpers you stop using. Check the wasm build:
`source /home/user/emsdk/emsdk_env.sh`, configure as in `.github/workflows/wasm.yml` into a directory
under /tmp, build, delete it. Do not add fields to Python classes that mirror pylance's. Do not commit,
push, change git branches, or run `--update`. Scratch files only in
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/w3-work/`. Builds: `cd build &&
ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`. Disk is
small: delete `/tmp/pytest-of-root` and `/tmp/tmp*` after test runs. Run each full suite once, at the
end; the bindings suite with `| grep -E "^(FAILED|ERROR)|passed|failed" | tail -15`.

## 7. Stop and ask (question first in the report)

- a nested index needs an on-disk difference from a top-level one that section 2 does not show;
- field 21's layout cannot be read without changing how manifests are written;
- a pylance test that passes today fails.

## 8. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; your new tests; the phase's targets through nanolance (`cd
.deps/pylance-tests/v12.0.0 && python3 -m pytest -o addopts= -q -p nanolance.lance._pytest_plugin
<target ids> | tail -5`); `python3 tools/pylance_suite.py | tail -4`; last, once, the full bindings suite.
Report under 40 lines: questions first, then files, per-target status, tails, decisions.

**N outcome (2026-10-20):** INVERTED indexes on struct leaves at any depth (build, search by dotted
path, default columns, MatchQuery, element granularity under a struct, append / delete /
optimize_indices, unindexed nested fragments scanned flat as pylance does), lance-c FTS on a nested
column; a document is null when any struct above its leaf is null. Both targets pass (expected list
1241). Rows tied on a score may come in either order between engines (tests compare scores in order and
rows with their scores).

**S outcome (2026-10-20): task done.** Manifest field 21 (the inline transaction) is read when there is no
transaction file, by `read_transaction` and by the C++ conflict classifier; `manifest_needs_migration` and
`migrate_manifest_paths_v2` as pylance's (local datasets only: a rename on an object store is not atomic).
The target passes (expected list 1242). Recorded, not changed: nanolance writes V2 manifest names for new
datasets even with `enable_v2_manifest_paths=False`; `manifest_needs_migration` does not check the
presence of physical_rows / num_deleted_rows.
