# Task: Lance's conflict rules in the C++ commit path

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP "Table-format
items", **T2** (Wave 2). Three sessions: **C1** (classify each concurrent pair as Lance does: compatible,
retryable, incompatible), **C2** (rebase compatible pairs without re-running the operation), **C3**
(merge deletion vectors of concurrent deletes / updates on the same fragment). Do only the phase you are
given.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines. Big ones: `src/dataset_ops.cpp` (2k+), `src/dataset_transaction.cpp`,
`src/transaction_file.cpp`, `bindings/python/nanolance/lance/dataset.py` (5k+). Lance's Rust: only
`/root/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/lance-12.0.0/src/io/commit/conflict_resolver.rs`
(5.6k lines; the functions named below, by `grep -n "fn check_"`). Cut output: `| tail -15` for pytest
(add `-o addopts=`), `| tail -5` for `tools/pylance_suite.py`.

## 1. Goal

When a C++ commit (`commit_next_version`, `src/dataset.cpp` ~line 182) loses the race for its version,
nanolance today does one of two things: `rebase` (same file, ~line 92) succeeds only when the other
writers did nothing but add fragments; anything else is a "commit conflict" and the operation's
`retry_on_conflict` wrapper re-runs it from scratch on the new version. Two consequences:

- **Wrong results where Lance refuses.** A delete raced by an overwrite is re-run on the new data and
  succeeds; Lance raises "Incompatible transaction". Same for two `update_config` calls on one key (last
  writer wins silently).
- **Needless work where Lance rebases.** An index build raced by a delete, an update, or a compaction of
  fragments it does not cover re-builds the whole index; Lance publishes it on top. Same for a
  compaction raced by a delete on other fragments, and a delete raced by a delete on other fragments.

After this task every pair is decided by Lance 12's rules (`TransactionRebase::check_txn` and its
`check_*_txn` functions): **compatible** pairs are rebased (C2, C3) without re-running, **incompatible**
ones fail with Lance's message and are not re-run, **retryable** ones keep today's behaviour (re-run by
`retry_on_conflict`, up to its limit -- a deliberate, documented difference: pylance surfaces some of them
to the user, nanolance retries them itself, with the same end state a user retry gives).

## 2. Facts (measured on pylance 12.0.0 -- do not re-derive)

Probe: `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/t2/probe.py` (pylance)
and `probe_nl.py` (the same through `nanolance.lance`); outputs `py.txt`, `nl.txt`. A 100-row dataset
(`id` int64, `v` float64; two fragments of 50); writer A opens v1; writer B commits one change; then A
commits its change from v1. pylance's results:

| A (ours) | B (theirs, committed first) | pylance | nanolance today |
|---|---|---|---|
| delete id=2 | delete id=1 (same fragment) | ok (98 rows) | ok |
| delete id=1 | delete id=1 | ok (99) | ok |
| delete id=70 | delete id=1 (other fragment) | ok | ok |
| delete id=2 | overwrite | **OSError: `Incompatible transaction: This Delete transaction is incompatible with concurrent transaction Overwrite at version 2.`** | ok (wrong) |
| update id=2 | delete id=1 (same fragment) | ok | ok |
| delete id=1 | update id=2 | ok | ok |
| delete id=2 | compaction | ok | ok |
| compaction | delete id=2 | CommitConflictError `Retryable commit conflict for version 2: This Rewrite transaction was preempted by concurrent transaction Delete at version 2. Please retry.` | ok (re-run: keep) |
| create BTREE id | delete | ok | ok (re-built: C2 rebases) |
| create BTREE id | compaction of its fragments | CommitConflictError (Rewrite) | ok (re-run: keep) |
| create BTREE id | create BTREE v | ok | ok |
| create BTREE id | create BTREE id (same name) | CommitConflictError (CreateIndex) | ok (re-run: keep) |
| append | overwrite | ok (2 rows) | ok |
| add_columns | delete | CommitConflictError (Merge vs Delete) | ok (re-run: keep) |
| drop_columns | append | ok | ok |
| update_config k=2 | update_config k=1 | **OSError: `Incompatible transaction: This UpdateConfig transaction is incompatible with concurrent transaction UpdateConfig at version 2.`** | ok (wrong) |
| restore | append | ok | ok |

The pylance "ok" rows above can be internal re-runs (pylance's own `delete` / `update` retry); the
row counts are what must match. The wording to use (Lance's `retryable_conflict_err` /
`incompatible_conflict_err`, conflict_resolver.rs ~line 229):

- incompatible: `Incompatible transaction: This {Ours} transaction is incompatible with concurrent
  transaction {Theirs} at version {v}.` -- through Python an `OSError` (not `CommitConflictError`);
  through lance-c the error code pylance's maps to (see `src/lance_c.cpp` ~line 202, which already maps
  "commit conflict").
- retryable: keep nanolance's `commit conflict: ...` prefix (that is what `retry_on_conflict` and
  `src/lance_c.cpp` key on), followed by Lance's text `Retryable commit conflict for version {v}: This
  {Ours} transaction was preempted by concurrent transaction {Theirs} at version {v}. Please retry.`
  -- seen only when the retries run out, or by operations that do not retry.

`{Ours}` / `{Theirs}` are Lance's operation names (Append, Delete, Overwrite, CreateIndex, Rewrite,
Merge, Restore, ReserveFragments, Update, Project, UpdateConfig, DataReplacement, DataOverlay, Clone,
UpdateBases).

## 3. Where things are in nanolance

- `src/dataset.cpp`: `rebase` (~92), `commit_next_version` (~182; the retry loop ~227).
- What a commit *is*, as Lance's transaction: `derive_transaction_operation(parent, next)`
  (`src/transaction_file.hpp`) -- the same derivation that writes `_transactions/*.txn` on every commit.
  The other writers' transactions: each intervening manifest's `transaction_file`
  (`_transactions/<name>`), written by nanolance and by pylance alike. A version with no transaction file
  (old datasets) is a **retryable** conflict (today's behaviour).
- Decoding and applying a transaction: `src/dataset_transaction.cpp` -- `decode_operation` (~295),
  `struct Operation` (~259), `apply_operation(base, op, next, ...)` (~847), used by the Python
  `LanceDataset.commit` path. Move what you need into an internal header (`src/dataset_transaction_ops.hpp`
  or similar); do not duplicate the decoder.
- The Python transaction API's own port of the matrix: `bindings/python/nanolance/lance/_transactions.py`
  `conflict_with` (~1002). It covers hand-built transactions without affected rows; the C++ port must
  agree with it on every pair it covers (a test asserts this for the pairs both can express). Do not
  change the Python commit path in this task.
- Re-run wrappers: `grep -n "retry_on_conflict" src/*.cpp src/*.hpp` (delete, update, merge_insert,
  compaction, optimize_indices). Index builds (`src/scalar_index.cpp` ~1882 / ~2127,
  `src/vector_index_build.cpp` ~2187, `src/fts_index_build.cpp` ~955, `src/index_segments.cpp` ~397)
  call `commit_next_version` directly.

## 4. Phases

- **C1 (classify).** On a lost race, for each version `v` in (read version, latest]: decode its
  transaction, decide ours-vs-theirs with a C++ port of `check_txn` (the `check_*_txn` functions for the
  operations nanolance's C++ commits: Append, Delete, Update, Overwrite, CreateIndex, Rewrite, Merge,
  Project, UpdateConfig, Restore, ReserveFragments, DataReplacement, DataOverlay, Clone, UpdateBases;
  the MemWAL / FRI-only branches only where nanolance writes those). The first incompatible or retryable
  verdict decides. Incompatible: fail with the message in section 2, no re-run. Retryable: today's path.
  All compatible: today's `rebase` for now (C2 replaces it). The two "wrong" rows of section 2 then
  match pylance.
- **C2 (rebase).** When every intervening transaction is compatible, build the new manifest by applying
  *our* operation to the latest manifest (`apply_operation`), with Lance's adjustments in `finish` /
  `finish_create_index` / `finish_rewrite` (conflict_resolver.rs ~1814, ~2082, ~2197): an index built
  from an older version keeps only the fragments still present
  (`prune_updated_fields_from_indices` for a concurrent Update of an indexed field), a rewrite's indexes
  remapped against the latest manifest, fragment ids after the latest `max_fragment_id`. `rebase`'s
  append-only special case becomes one instance of this, or goes. Add an internal counter (C++ only, for
  tests: e.g. `commit_conflict_stats()` with `rebased`, `retried`, `refused`) so tests can tell a rebase
  from a re-run.
- **C3 (deletion vectors).** Delete / Update against a concurrent Delete / Update on the same fragment
  with unchanged data files (`check_delete_txn` ~349, `check_update_txn` ~453, `finish_delete_update`
  ~1836): the delete / update passes the row addresses it deleted (its *affected rows*) to the commit;
  the commit reads the latest deletion file of each such fragment, conflicts (retryable, Lance's
  row-address message) if they overlap, else writes a merged deletion file (and removes a fragment whose
  rows are now all deleted, as Lance does). Without affected rows, retryable (as Lance).

## 5. Tests you write

C++: `tests/test_commit_conflicts.cpp` (add to CMake like the other `tests/test_*.cpp`). A two-writer
harness: open at v1, commit "theirs" through the public API, then commit "ours" from the stale v1
manifest. One test per row of section 2 (outcome and row count), plus each compatible pair of C2 / C3
asserting **rebased, not re-run** via the counter, plus: a version without a transaction file is
retryable; an incompatible verdict is not re-run (counter `retried == 0`).
Python: `bindings/python/tests/test_commit_conflicts.py` (`require_pylance()`): the probe of section 2
through both engines, outcomes equal row by row except the four documented "re-run: keep" rows; a
pylance writer racing a nanolance writer and the reverse (each reads the other's transaction file).

## 6. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. C++
without warnings (CI uses `-Werror`; the wasm build too: no threads, no `std::filesystem` on read paths).
A commit that does not race must not change: same files, same manifest bytes as before this task (the
existing tests and the pylance suite check this). Do not commit, push, change git branches, or run
`--update`. Scratch files only in `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/t2-work/`.
Work only in `/home/user/nanolance`. Builds: `cd build && ninja`, then `cd
bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp _nanolance.cpython-311-x86_64-linux-gnu.so
/usr/local/lib/python3.11/dist-packages/nanolance/`. Run each full suite once, at the end, one at a time;
no sleep loops; kill by PID. Delete `/tmp/pytest-of-root` after full runs (the disk is small).

## 7. Stop and ask (question first in the report)

- a pair whose Lance verdict depends on state nanolance's transaction files do not record (say which
  field);
- `apply_operation` would need a change that alters what a non-racing commit writes;
- a pylance test that passes today fails after your change.

## 8. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; `./build/test_commit_conflicts | tail -5`;
`python3 -m pytest -o addopts= -q bindings/python/tests/test_commit_conflicts.py | tail -2`;
`python3 tools/pylance_suite.py | tail -4`; last, once: `cd bindings/python && python3 -m pytest -o
addopts= -q -p no:cacheprovider tests | tail -3`. Report under 40 lines: questions first, then files,
per-pair status, tails, decisions.

**C1 outcome (2026-10-20, first session):** verdicts implemented (`src/commit_conflicts.{hpp,cpp}`, decoder
moved to `src/dataset_operation.{hpp,cpp}`, `classify_concurrent` in `commit_next_version`, counters in
`include/nanolance/dataset_commit.hpp`, `_errors.py` maps "Incompatible transaction:" to OSError,
`tests/test_commit_conflicts.cpp` at verdict level). Reviewer found: **the probe of section 2 is unchanged
end to end**, because nanolance's Python mutations never race from a stale handle -- `delete_once`
(`src/dataset_ops.cpp` ~791) and the other `*_once` functions `load_latest`, so a handle opened at v1
deletes from v3. pylance commits from the handle's own version (`read_version`), then resolves.
Reviewer's answers to the session's questions:
1-2. `Update.inserted_rows_filter` and `update_mode`: record them in `derive_transaction_operation` as
   pylance does (C2; measure pylance's .txn for a keyed merge_insert and an update first).
3. Overwrite `config_upsert_values`: write what pylance writes (C2; measure a pylance overwrite's .txn
   carrying an unchanged config key). Until then, accepted.
4. Lance wins: change `_transactions.py` `conflict_with` to agree with Lance on 4a-4d (C1b).
5. lance-c: keep `LANCE_ERR_INTERNAL` (lance.h is upstream's header; never add codes; pylance raises a
   plain OSError for it, not CommitConflictError).
6. FRI / MemWAL names: accepted as absent.

**C1b (second session):** finish C1.
- **Read version.** Each mutation commits from the version the dataset object was opened at, as
  pylance does: the Python methods pass the handle's version (`self._version`, or "latest" when the
  object follows latest) down through the bindings to the C++ `*_once` functions, which load that
  version instead of `load_latest`; on a lost race the classification of C1 decides (compatible ->
  rebase, retryable -> `retry_on_conflict` re-runs on latest, as pylance's own retries do; incompatible
  -> error). Operations: delete, update, merge_insert, add_columns / drop_columns / alter_columns,
  update_config / table and schema metadata, compaction, create_index / create_scalar_index,
  optimize_indices, restore. Overwrite and append need no read version (their pairs do not depend on
  it beyond Lance's rules for Append / Overwrite as "ours"). C++ API: an optional read version
  parameter (default: latest, today's behaviour, so lance-c and C++ callers are unchanged).
- The two "wrong" rows of section 2 now raise pylance's OSError through `nanolance.lance`; the four
  "re-run: keep" rows still succeed; every other row unchanged. Write
  `bindings/python/tests/test_commit_conflicts.py` (section 5) and the C++ two-writer tests
  (incompatible not re-run; no transaction file -> retryable).
- Align `_transactions.py` `conflict_with` with Lance on 4a-4d above, and add the C++-vs-Python
  agreement test for the pairs both express.
- Check the wasm build still compiles the touched sources (`docs/../AGENTS.md`, wasm note).

**C1b outcome (2026-10-20):** mutations commit from the handle's version (`ReadVersion`,
`retry_from_read_version`; C++ default stays "latest"): delete, update, merge_insert, add / drop / alter
columns, compaction, update_config and table / schema metadata (the last two now retry too). The probe:
"delete vs overwrite" and "update_config same key" raise pylance's OSError; the four "re-run: keep" rows
succeed; all other rows unchanged. `bindings/python/tests/test_commit_conflicts.py` (20),
`tests/test_commit_conflicts.cpp` two-writer tests. Reviewer: removed `load_latest` (unused; the wasm
build's clang `-Werror` failed on it). Decisions:
- A reader-form `add_columns` (stream, not replayable) surfaces a retryable verdict as a commit conflict
  instead of re-running: accepted (pylance surfaces that pair too).
- A handle whose version auto-cleanup removed commits from the latest manifest: accepted
  (`test_enable_disable_auto_cleanup`).
- "4a-4d" in the C1 answers referred to the first session's report; the list is now spelled out in C2.

**C2 scope, in addition to section 4:**
- Index builds (`create_index`, `create_scalar_index`, `optimize_indices`, segment commits) and
  `restore` commit from the handle's version too; with C2's rebase an index build raced by an append /
  delete / update is published without re-building (counter: `rebased`, not `retried`).
- `derive_transaction_operation` records `Update.inserted_rows_filter`, `Update.update_mode` and
  Overwrite's `config_upsert_values` as pylance writes them (measure a pylance .txn for a keyed
  merge_insert, an update, and an overwrite carrying an unchanged config key first).
- `_transactions.py` `conflict_with` agrees with Lance (and the C++ port) on: Overwrite vs Overwrite and
  vs UpdateConfig on one config key (incompatible); UpdateConfig (ours) vs Overwrite on a config key;
  UpdateConfig vs UpdateConfig both setting schema metadata (Lance's rule, conflict_resolver.rs
  `check_update_config_txn`); Rewrite (ours) vs CreateIndex on overlapping fragments (retryable);
  Rewrite / Update (ours) vs DataOverlay on overlapping fragments (retryable); CreateIndex as ours. Add
  the C++-vs-Python agreement test over the pairs both can express.

**C2 outcome (2026-10-20):** compatible pairs rebase by applying our operation to the latest manifest
(`rebase_operation`, `src/dataset_transaction.cpp`; the append-only `rebase` is gone); index builds and
restore commit from the handle's version; `_transactions.py` `conflict_with` agrees with the C++ port
over all ordered pairs of 15 operations (`_commit_verdict` test). Measured: pylance writes
`inserted_rows_filter` only for merge_insert on an unenforced primary key, `update_mode` only for
`rewrite_columns`, and no `config_upsert_values` on a plain overwrite, so `derive_transaction_operation`
is unchanged. Reviewer removed the old helpers (wasm clang `-Werror`). Accepted gaps: no key-existence
filter (a pylance PK merge_insert racing a nanolance update re-runs where Lance refuses); a row-moving
update of an indexed column does not prune the racing index build's coverage (results stay correct);
rewrite_columns merges recorded as RewriteRows; on stable-row-id datasets Append / Update / Merge /
Overwrite are re-run, not rebased (the old `rebase` kept base-counter row ids: possible duplicates).

**C3 scope, in addition to section 4:**
- Affected rows: `delete_once` / `update_once` (and merge_insert's delete / update paths) know the row
  offsets they delete per fragment (`apply_deletions` gets them). Pass them to the commit (a new optional
  argument of `commit_next_version`, or a field next to `ReadVersion`), so a Delete / Update raced by a
  Delete / Update on the same fragment with unchanged data files is merged as `finish_delete_update`
  does: read the latest deletion file of each such fragment, overlap -> retryable with Lance's message
  (`This {Ours} transaction was preempted by concurrent transaction {Theirs} at version {v} (both
  modified rows at addresses [...]). Please retry`, up to 5 addresses), else write a merged deletion
  file (Lance's name and format, as `apply_deletions` writes them) and drop a fragment whose rows are
  now all deleted. Counter: `rebased`, not `retried`. Two-writer tests: disjoint rows (merged, rows
  equal pylance's), the same row (retryable -> re-run, rows equal pylance's), all rows of a fragment.
- Found in the C2 review: a compaction raced by a delete now ends one version later than before (probe
  `compaction vs delete`: v5, was v4): its ReserveFragments commit rebases over the delete, then the
  Rewrite is retryable and the re-run reserves again, leaving an empty ReserveFragments version. Make
  the re-run reuse the reservation it already committed (or not reserve again when the reserved ids are
  still unused), so the probe ends at v4 again with the same rows.

**C3 outcome (2026-10-20): task done.** Delete / Update raced by a Delete / Update on the same fragment
merge deletion vectors as Lance's `finish_delete_update` (overlap -> retryable with Lance's message; a
fragment left with no rows is removed); affected rows passed by delete, update and merge_insert
(`AffectedRows`, `commit_next_version`). Compaction checks its rewritten fragments against the latest
version before committing ReserveFragments, so the probe's compaction vs delete ends at v4 again (a
writer landing between that check and the commit can still leave an empty ReserveFragments version:
accepted). Found and fixed: C2's rebase kept the fragment id an Append / Update chose on its stale base,
so it could duplicate a concurrent writer's id (a dropped row in `test_concurrent_writers`); added
fragments are now numbered after the latest manifest's. Reviewer: `test_concurrent_writers` 6 of 6.
Open, not reproduced: one `test_index_optimize.py::test_optimize_after_append[pylance]` failure in a full
run (the known k-means flake in ROADMAP "Small items").
