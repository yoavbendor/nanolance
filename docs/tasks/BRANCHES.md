# Task: branches and clones (create_branch, checkout of a branch, branches API, shallow / deep clone)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP "Table-format
items", **T3** (Wave 2). Three sessions: **B1** (read pylance's branches), **B2** (create and write
branches; the branches API), **B3** (shallow / deep clone; namespace branch operations). Do only the phase
you are given.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines. Big ones: `bindings/python/nanolance/lance/dataset.py` (5k+),
`src/dataset.cpp`, `src/manifest_writer.cpp`, `src/dataset_ops.cpp`, `bindings/python/src/nanolance_bindings.cpp`.
Lance's Rust: read only the files named here. Cut output: `| tail -15` for pytest (add `-o addopts=`),
`| tail -5` for `tools/pylance_suite.py`.

## 1. Goal

pylance 12 datasets can carry branches: named lines of versions forked from a version of another branch
(or main). nanolance refuses them by name today (`dataset.py`: `raise unsupported("branches")` near lines
636 and 2460, and the attribute list near line 2260; `_namespace_dir.py` near lines 1186 and 1765). After
this task nanolance reads, creates, writes, lists and deletes branches exactly as pylance does, on disk
and through the API, and `shallow_clone` / `deep_clone` work. Each engine opens and appends to the other's
branches.

Targets (`.deps/pylance-tests/v12.0.0/tests/`): `test_dataset.py::test_branches`, `::test_shallow_clone`,
`::test_deep_clone`; `test_namespace_dir.py::...::test_branch_crud_round_trip`,
`::test_create_branch_from_other_branch` (B3). `test_s3_ddb_branches` and
`test_namespace_managed_branches` need DynamoDB / S3 writes: out of scope.

## 2. Facts (measured on pylance 12.0.0 -- do not re-derive)

Re-run in `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/br/` (`ds.lance`:
v1 3 rows, v2 +1 row; branch `exp` from v1, then one append on `exp`).

Layout. A branch `B` lives under `<root>/tree/B/` with its own `_versions/`, `_transactions/`, `data/`
(and `_versions/latest_version_hint.json`, `{"version":N}`, which pylance writes on every commit, main
included). Branch names can contain `/` (Lance's `branch_location.rs`). Its reference is
`<root>/_refs/branches/B.json`:

```json
{"parentBranch": null, "identifier": {"version_mapping": [[1, "1935c23a6df942abaf8023c96579f240"]]},
 "parentVersion": 1, "createAt": 1792482184, "manifestSize": 369, "metadata": {}}
```

`parentBranch` is null for main, else the parent's name. `identifier.version_mapping` is the chain of
(parent_version, 32-hex random id) from the root: a branch of a branch has two entries (its parent's,
then its own). `createAt` is Unix seconds; `manifestSize` the size in bytes of the parent manifest it was
forked from.

The branch's first manifest has the **same version number as the parent version** it forks (branch from
v1: the branch's `_versions/` holds version 1), and is the parent's manifest with:
- every inherited data file (and deletion file) gets `base_id = 0` (DataFile field 7): base path id 0,
  not "the dataset itself" -- only an absent base_id means the dataset's own directory (Lance
  `dataset.rs` `data_file_dir_for_base`);
- `base_paths` (Manifest field 18) gains `{id: 0, is_dataset_root: true, path: <parent's root, as the
  dataset was opened: relative if opened relative>}` (BasePath fields 1, 3, 4);
- reader and writer feature flags (Manifest fields 9, 10) gain 16 (base paths);
- `branch` (Manifest field 20) = `"B"`;
- a new transaction file (a Clone operation; nanolance's transaction writer must learn it) in
  `tree/B/_transactions/`.

Files written on the branch afterwards have no `base_id` (they live under `tree/B/data/`); the next
version is parent version + 1 (append on `exp` from v1 -> `exp` v2 while main has its own v2). Main is
never touched by a branch write.

API (pylance 12):
- `ds.create_branch(name, reference=None)` -> the branch as a `LanceDataset` at its first version.
  `reference`: `None` (this dataset's current version on its branch), an int version, a tag name, or a
  `(branch, version)` tuple where branch `None` or `"main"` is main and version `None` is that branch's
  latest. `branch.uri` is `<root>/tree/<name>` (absolute if the root was). A second branch of the same
  name fails (pylance's own message is an internal error; raise `OSError` containing "already exists").
- `lance.dataset(<root>/tree/B)` opens the branch; `write_dataset(..., <root>/tree/B or the branch
  dataset, mode="append"/"overwrite")` commits on it.
- `ds.checkout_version(x)`: `x` an int, a tag, or `(branch, version)`. A missing branch:
  `ValueError` ("Not found: .../tree/nope/_versions"); a missing tag name: `OSError` containing "does not
  exist" (test_branches relies on it). `checkout_latest()` moves to the branch's latest.
- `ds.branches`: `list()` -> `{name: {"parent_branch", "branch_identifier": [(int, str), ...],
  "parent_version", "create_at", "manifest_size", "metadata"}}`; `list_ordered()` (by `create_at`, then
  name: check against pylance); `delete(name)`; `replace_metadata(name, dict)`. Deleting `"main"`:
  `OSError` "Ref is invalid: Branch name cannot be 'main'". Deleting a branch another branch forks from:
  `OSError` 'Ref conflict error: Branch b1 is referenced by [("b1child", 1)] versions, can not delete'.
  Delete removes the JSON and `tree/B/`.
- Tags gain a branch: `_refs/tags/T.json` has `"branch": "exp"` or `null`; `tags.create(name, ref)`
  takes the same reference forms (`(None, None)` and `("main", None)` are main's latest);
  `tags.list()[t]["branch"]`.
- `shallow_clone(target, reference)`: `target/_versions/` gets one manifest (same version number as the
  source version) whose files all point at the source through `base_paths` (as a branch's do), plus a
  transaction; no `data/`. `deep_clone(target, reference)`: the same, with the data (and deletion) files
  copied into `target/` and no base path to the source. Both return the clone, opened.

Lance's code (sources under `/root/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/`):
`lance-12.0.0/src/dataset/refs.rs` (branch and tag JSON, checks, errors), `.../dataset/branch_location.rs`
(paths), `lance-12.0.0/src/dataset.rs` lines 601 (`create_branch`), 3264 (`shallow_clone`), 3314
(`deep_clone`); the Clone transaction in `lance-12.0.0/src/dataset/transaction.rs`
(`grep -n "Clone" ...`); `lance-table-12.0.0/protos/table.proto` (Manifest field 20 `branch`, 18
`base_paths`) and `transaction.proto` (the Clone operation).

## 3. Where things are in nanolance

- Base paths are read and written already: `include/nanolance/base_paths.hpp`, `src/base_paths.cpp`
  (`read_base_paths`, `write_base_paths`, data-file resolution by `base_id`); multi-base datasets are done
  (`docs/tasks/MULTI_BASE.md`).
- Manifests: `src/dataset.cpp` (reading, line ~281 reads base paths), `src/manifest_writer.cpp`
  (feature flags, unknown-field preservation: check field 20 survives a commit).
- Transaction files: `grep -rn "Clone\|write_transaction" src/*.cpp | head`.
- Tags (Python): `dataset.py` near line 2460 (`Tags`); refs JSON reading in `_namespace_dir.py` near 1670.
- Commit retries / version numbering: `src/dataset.cpp` (`rebase`), `src/manifest_writer.cpp`.

## 4. Phases

- **B1** (read): open a pylance branch by `<root>/tree/B` and by `checkout_version((B, v))` /
  `(B, None)`; scans, takes, filters, indexes inherited from the parent all answer as pylance does;
  `branches.list()` / `list_ordered()` and tags with a branch read pylance's JSON; nanolance appends to a
  pylance branch and pylance reads the result (files under `tree/B/data/`, version parent+1, main
  unchanged).
- **B2** (create / write): `create_branch` with every reference form, branches of branches, `delete`
  (and its two refusals), `replace_metadata`, tags on branches, `latest_version_hint.json` on every commit
  if pylance needs it to open (measure: does pylance open a nanolance branch without it?). pylance reads
  nanolance's branches with the same results. `test_branches` passes.
- **B3** (clones): `shallow_clone`, `deep_clone` (C++ core function + Python), the namespace branch
  operations in `_namespace_dir.py` (`create_table_branch`, `list_table_branches`,
  `delete_table_branch`, `describe_table` / `list_table_versions` with `branch=`); the remaining targets
  pass.

## 5. Tests you write

`bindings/python/tests/test_branches.py` (`require_pylance()`): each engine creates a branch (from main,
from a tag, from a branch), appends and overwrites on it, and the other engine reads it with the same
rows and versions; main untouched; scalar and vector indexes inherited by a branch answer as pylance's;
the refs JSON nanolance writes equals pylance's in every field but ids and times; delete and its
refusals; tags with branches; clones (shallow reads through the source, deep has its own files and still
reads after the source is deleted).

## 6. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. C++
without warnings (CI uses `-Werror`). Datasets without branches must not change on disk (no new files or
fields; if `latest_version_hint.json` turns out to be needed, ask first). Do not commit, push, change git
branches, or run `--update`. Scratch files only in
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/br-work/`. Work only in
`/home/user/nanolance` (never `/home/user/nl-perf`). Builds: `cd build && ninja`, then `cd
bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp _nanolance.cpython-311-x86_64-linux-gnu.so
/usr/local/lib/python3.11/dist-packages/nanolance/`. Run each full suite once, at the end, one at a time;
never two at once; no sleep loops; kill by PID. Delete `/tmp/pytest-of-root` after full runs (the disk is
small).

## 7. Stop and ask (question first in the report)

- a branch write needs a change to how main's commits are written (beyond carrying field 20 and the base
  paths);
- pylance cannot open a branch nanolance wrote, and the difference is not in the facts above;
- `latest_version_hint.json` (or another new file) is needed on datasets without branches.

## 8. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; the target pylance tests run through nanolance (`cd
.deps/pylance-tests/v12.0.0 && python3 -m pytest -o addopts= -q -p nanolance.lance._pytest_plugin
tests/test_dataset.py -k "branches or clone" | tail -5`); `python3 -m pytest -o addopts= -q
bindings/python/tests/test_branches.py | tail -2`; `python3 tools/pylance_suite.py | tail -4`; last, once:
`cd bindings/python && python3 -m pytest -o addopts= -q -p no:cacheprovider tests | tail -3`. Report
under 40 lines: questions first, then files, per-test status, tails, decisions.

**B1 outcome (2026-10-20):** done in one Haiku session; reviewed. pylance's branches open by
`<root>/tree/B` and by `checkout_version((B, v))` / `(B, None)` / a branch tag; data, deletion and index
files resolve through the manifest's base paths (base_id 0 is base path 0; index `base_id`, IndexMetadata
field 9, honoured on every index read path); `branches.list()` / `list_ordered()` (pylance's order:
parent_version descending, then name), tags with a branch, names with `/`; nanolance appends and
overwrites on a pylance branch and pylance reads the result. `tests/test_branches.py` (12). Reviewer's
decisions for B2:
- **Cleanup must be branch-aware** (the session found the data loss): pylance's `cleanup_old_versions`
  keeps every version a branch forks from (measured: main v1 forked as `keep`, main overwritten, cleanup
  with `older_than=0, delete_unverified=True` -> `old_versions: 0`, the branch still reads). nanolance's
  cleanup (and `restore` / `drop` where they delete) must keep the same versions and files; a test
  reproduces the loss first (`scratchpad/br-work/clean2.py`).
- `latest_version_hint.json`: not written (pylance opens branches without it; no new files on datasets
  without branches).
- Index write and maintenance paths (`index_segments` commit / merge, `index_optimize` segment dirs) still
  use `<dataset>/_indices`: B2 makes them branch-correct (an index built on a branch lives under
  `tree/B/_indices`).
- `checkout_version((B, v)).uri` is `tree/B` (pylance reports the root); keep it, writes need it.
