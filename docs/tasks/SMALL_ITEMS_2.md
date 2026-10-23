# Task: small items bundle 2 -- API gaps of 2-9 pylance tests each

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Three phases, each a
fresh session: **A** types and validation, **B** plans and stats, **C** dataset / fragment APIs.
~55 of pylance's tests. Each item is independent: do them in the listed order, and report the ones
you could not finish rather than stretching the session.

## 0. Context budget

About 100k tokens per session. `grep -n` then `sed -n` windows of at most ~120 lines; never read
`bindings/python/nanolance/lance/dataset.py` (3.6k+), `src/expr/expr.cpp`, `src/scan/lance_table_reader.cpp`,
`src/dataset/index_search.cpp` or `bindings/python/src/nanolance_bindings.cpp` whole. Cut all output. For each
item: read the named pylance tests (`.deps/pylance-tests/v12.0.0/tests/...`), measure the behaviour
on real pylance 12.0.0 (`import lance` in plain `python3`), then implement. Write nanolance's own code.

## 1. Items

**Phase A -- types and validation**
1. `lance.bitmap.Bitmap` construction checks (`tests/test_bitmap.py`, 9 tests: grep
   `rejects_non_integer\|out_of_range\|rejects`): non-integer input -> ValueError, values outside u32
   -> OverflowError with pylance's exact text (`bitmap values are u32, got -1` etc.: measure).
   nanolance's Bitmap: `bindings/python/nanolance/lance/bitmap.py`.
2. Decimal and duration columns in filters and scalar indexes (`tests/test_scalar_index.py::
   test_scalar_index_types[btree-decimal128|decimal256|duration]` and the bitmap / zonemap variants,
   grep `test_scalar_index_types`): `arrow_cast(x, 'Decimal128(p, s)')` / `'Duration(Millisecond)'`
   in filters (src/expr/expr.cpp ~885-908 refuses them), and BTREE (and whatever other kinds the test
   parametrizes) on decimal128 / decimal256 / duration columns (src/index/scalar/scalar_index.cpp refuses:
   "a scalar index on a column of type decimal..."). Index files must equal pylance's (compare
   page_data / page_lookup rows as earlier index tasks did).

**Phase B -- plans and stats**
3. `scanner.analyze_plan(count_rows=True)` and count pushdown (`tests/test_count_pushdown.py`, 4
   tests): measure the text pylance prints and which parts tests assert.
4. `tests/test_dataset.py::test_analyze_filtered_scan` (4 cases): the regex the test searches
   `analyze_plan()` output for.
5. `test_scalar_index.py::test_btree_index_cache_hit_miss_stats` (4): index cache hit / miss
   counters in `analyze_plan` or stats -- measure where pylance reports them; nanolance's caches are
   in src/index/index_files.hpp (`IndexCache`).
6. Late-materialization plan text (`test_dataset.py::test_late_materialization_param` 3,
   `test_fragment.py::test_fragment_scanner_late_materialization[*]` 3): the projection lines the tests
   assert (`projection=[values], source=stream`, `projection=[filter, values]`), and the
   `late_materialization=` scanner parameter.
7. `MergeInsertBuilder.explain_plan()` / `analyze_plan()` (`test_dataset.py::
   test_merge_insert_explain_analyze_plan`, 2).
Plan text: nanolance prints its own plan (`nanolance scan of ...`); add the lines tests assert in
pylance's wording, without pretending to run DataFusion operators nanolance does not have -- if a test
needs a whole DataFusion plan tree, report it instead.

**Phase C -- dataset / fragment APIs**
8. `LanceDataset.delta(...)` (`tests/test_delta.py`, 5 tests): measure its API (`DatasetDelta`?
   `get_inserted_rows`, `get_updated_rows`, `get_deleted_row_ids`...), built on the version metadata
   nanolance already has (row ids, `_row_created_at_version`, deletion files, transactions).
9. `update(..., where="_rowid = ...")` (`test_dataset.py::test_update_by_rowid`, 2): `_rowid` in an
   update predicate (nanolance's filter knows `_rowid`; the update path does not).
10. `create_index` on an empty dataset for a vector index (`tests/test_create_empty_index.py::
    test_create_empty_vector_index`, 2): measure what pylance writes.
11. `LanceFragment.open_session()` (`test_fragment.py::test_fragment_session` 2,
    `test_json.py::test_json_fragment_session_take` 2): measure its API (`take`, ...).
12. `test_commit_index.py::test_commit_index_rejects_invalid_covering_fields` (2): the OSError pylance
    raises for invalid covering fields in a CreateIndex commit.

## 2. Oracles

`python3 tools/pylance_suite.py -k "<test names>" 2>&1 | grep -E "^FAILED|passed|failed" | tail -15`
(ignore the `tests/test_datagen.py` collection error).

## 3. Tests you write

`bindings/python/tests/test_small_items_2.py` (new), both engines side by side via `require_pylance()`
from `tests.support`, a section per item.

## 4. Rules, builds, stop rules

`AGENTS.md` sections 5 and 7. No skipped / weakened tests. C++ without warnings. Do not commit, push,
change branches, or run `--update`. Do NOT schedule reminders, send_later or triggers of any kind. No
`pytest -n`. The full pytest takes ~12 minutes: run it in the background (Bash `run_in_background`)
with a 1500 s timeout. Delete only your own scratch files. Builds: `cd build && ninja`, then
`cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
Stop and ask (question first) when an item needs a new on-disk format, a change to shared planning /
type-system code, or more than ~250 lines; skip to the next item and report it.

## 5. Done per phase -- report (under 40 lines, sent once)

Tails of: `cd build && ctest -j8 | grep "tests passed"`; each item's oracle; your test file;
`python3 tools/pylance_suite.py | tail -4`; last, once: the full pytest (`-q -rf`) tail. Per item:
done / partial / not started, decisions, questions.

**Phase A outcome (2026-10-16):** item 1 done (Bitmap construction / update / add errors as pylance,
+8 pylance tests). Item 2: decimal128 and duration BTREE / BITMAP / ZONEMAP already build with pages
equal to pylance's (tests added); left for a separate item (reviewer): Decimal and Duration value
kinds in the expression engine (filters cannot compare decimal columns; `arrow_cast` to
Decimal / Duration refused; duration literals are unit-unaware) and a 256-bit key kind for decimal256
indexes -- shared type-system code, grouped with the result-type item on the roadmap. Reviewer also
fixed a flaky batch-KNN test: with an index and a post-filter the per-query row counts depend on each
engine's own index, so only query_index's shape is compared there (`matches_singles` stays exact).

**Phase B outcome (2026-10-16), part 1:** item 4 done (`analyze_plan()` runs the scan and prints
`LanceRead: ... metrics=[output_rows, rows_scanned, fragments_scanned]` for what really ran), item 6
partial (`late_materialization=` bool / column list, a real late read: filter columns first, then take
by address; plan lines only for what runs), item 7 partial (`MergeInsertBuilder.explain_plan()`, a dry
`analyze_plan()`). +7 pylance tests (1159). Reviewer's facts for the rest:
- Item 6 default (`late_materialization=None`), Lance 12 `MaterializationStyle::Heuristic`
  (dataset/scanner.rs ~2811): local storage -> a column is read early when its type has a fixed byte
  width < 10 bytes (`byte_width_opt() < 10`), late otherwise (variable-width types have no fixed width
  -> late); cloud storage threshold 1000; a blob returned as descriptions is early.
- Item 7: pylance's `merge_insert(...).analyze_plan(source)` really executes the write (bytes_written,
  num_files_written) but does NOT commit (the dataset version is unchanged) -- nanolance should write the
  fragments and report the real counts and bytes, leaving them uncommitted (orphaned files, as Lance).
- Items 3 (`CountFromMask`: a count answered from an exact scalar-index mask without reading) and 5
  (index cache hit / miss through `scan_stats_callback` / `ScanStatistics`) need shared scan /
  planning code: roadmap items, not this bundle.

**Phase B outcome, part 2 (2026-10-16):** items 6 and 7 done. `late_materialization=None` follows
pylance (measured on 24 types: early iff the fixed byte width < 10 -- lists by their items' width,
structs by their children's sum, fixed-size lists by items x size; booleans, nulls, maps, variable
types late; blobs early), kept early for partial-schema fragments and datasets with blob columns
(exactness guards). `MergeInsertBuilder.analyze_plan(source)` writes through the uncommitted merge
(version unchanged, files orphaned as Lance) and reports num_inserted / updated / deleted rows,
num_files_written and bytes_written. Phase C next. Benchmark the default-late change (filtered scans of
wide columns now read late) in the coming performance review.

**Phase C outcome (2026-10-16):** items 8-12 done: `ds.delta(...)` (`delta.py`: list_transactions,
get_inserted_rows / get_updated_rows / get_deleted_row_ids, pylance's validation), `update(where=
"_rowid ...")` (resolved by a scan into a row-address allowlist for the native update), vector
`create_index(train=False)` refused as pylance, `LanceFragment.open_session().take(...)`, CreateIndex
covering-field checks. +11 pylance tests (1172). Reviewer: an address-driven update resolves on the
latest version and is not retried on a conflict (the addresses name one version's rows). Left:
`delete()` with `_rowid`; `delta()` raises version-not-found eagerly (pylance lazily);
`test_create_empty_scalar_index` (train=False on a scalar index still indexes the rows). Task
SMALL_ITEMS_2 done.

**Performance review (2026-10-16, reviewer), late materialization.** Benchmarked against the build before
this stretch (e9bf014): scalar-index queries over all columns were up to 3.7x slower (`cat = 'c7'`,
100k of 5M rows: 45 -> 166 ms). Cause: the B2 default (Lance's heuristic: late for columns of 10+ bytes).
Measured on nanolance, late (filter columns first, then a take by address) loses to its own scan:
1.17x slower at 0.005% selectivity on 2 KB strings, 0.88x at 1%, ~3x at 10% and more, ~2x on mixed
columns. Decision: the default (`late_materialization=None`) reads early -- nanolance's own choice, its
plan says so; an explicit True / column list still reads late (now through a direct take of the scan's
sorted addresses: no Python sort, no list round trip). pylance's
`test_dataset.py::test_late_materialization_param` asserts pylance's default plan and leaves the
expected list (1172 -> 1171) on purpose: the plan must describe what runs.
