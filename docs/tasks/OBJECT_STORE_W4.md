# Task: maintenance on s3:// datasets -- cleanup, drop, branches, clones (object-store writes, phase W4)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Design:
`docs/OBJECT_STORE_WRITES.md` (sections 2, 3, 5). W1-W3 are done (`docs/tasks/OBJECT_STORE_W2.md`,
`OBJECT_STORE_W3.md`: every write but these). One session. After it, nothing nanolance writes is refused on
`s3://` except `migrate_manifest_paths_v2` (local-only by design: no atomic rename on object stores).

## 0. Context budget

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines. Big ones: `src/table/dataset_refs.cpp` (cleanup, branches, clones),
`bindings/python/nanolance/lance/dataset.py` (5k+). Cut output: `| tail -15` for pytest (add `-o addopts=`).

## 1. Goal

On an `s3://` dataset opened with pylance's `storage_options`, each of these does what it does locally,
and pylance agrees with the result:
- `cleanup_old_versions` (every option: `older_than`, `retain_versions`, `delete_unverified`,
  `error_if_tagged_old_versions`), auto cleanup on commit (W1 skipped it on object stores:
  `run_auto_cleanup` in `src/table/manifest_writer.cpp`), and its branch-aware rules (`docs/tasks/BRANCHES.md`
  B2 outcome: versions a branch forks from are kept);
- `LanceDataset.drop` (every object under the dataset's prefix);
- branches: `create_branch`, `checkout_version((branch, v))`, writes on a branch (`tree/<B>/`),
  `branches.delete`, `branches.replace_metadata`, tags on branches; refs under `_refs/branches/`;
- `shallow_clone` and `deep_clone` (to an `s3://` target, and between local and `s3://` both ways).

## 2. Facts

- Measure pylance on moto first for each operation (the request sequence: which objects it LISTs, PUTs,
  DELETEs -- moto's log; pylance's tag delete was a HEAD then a batch `POST ?delete`, see W2a outcome).
- Object stores have no directories and no rename. Listing: `io::list_dir` (one level; recurse yourself
  where cleanup needs every object under `data/`, `_deletions/`, `_indices/`, `_transactions/`,
  `_versions/`, `tree/`). Deleting: `io::remove_output` (one DELETE per object; nanos3reader has no batch
  delete -- if cleanup of many objects is slow, say so, do not add one). Copy (deep clone): read through
  `io::open_source` and write through `io::open_output` (streamed: the sink spills to a temporary file over
  8 MiB), never a whole object in memory.
- A branch create must not overwrite another writer's branch: create the ref with `io::put_if_absent`
  (as W2a did for tags).
- Python refusals to lift: `grep -n "refuse_object_store_write" bindings/python/nanolance/lance/*.py`
  (cleanup, drop, branches, clones). Keep `migrate manifest paths`.
- pylance's `test_s3_ddb_branches` and `test_namespace_managed_branches` need DynamoDB: out of scope.

## 3. Measuring and testing

As in `docs/tasks/OBJECT_STORE_W2.md` section 3 (moto on **your own port**, not 5077; stop only the
process you start, by PID). Extend `bindings/python/tests/test_object_store_commits.py`: per operation the
s3 result against a local copy (versions, rows, which objects remain -- for cleanup, the object listing
equals the local file listing), pylance reading nanolance's result and the reverse (pylance's cleanup /
branch on S3, then nanolance). One race: cleanup vs an append on S3 (no live file deleted: the append's
rows all read back). Remove each operation from `WRITES` in `tests/test_object_store.py`.

## 4. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. The
local native build has `-DNANOLANCE_WERROR=ON`; delete helpers you stop using; check the wasm build
(`source /home/user/emsdk/emsdk_env.sh`, configure as in `.github/workflows/wasm.yml` into a directory under
/tmp, build, delete it). **A local-disk operation must do exactly what it did before** (same files kept
and removed). Do not add fields to Python classes that mirror pylance's. Do not commit, push, change git
branches, or run `--update`. Scratch only in
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/w4-work/`. Builds: `cd build &&
ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`. **The disk is
nearly full (about 2 GB free)**: no extra build trees; do not delete anything outside your scratch directory
(not `/tmp/pytest-of-root`, not `/tmp/tmp*`); report `df -h /` at the end. Run each full suite once, at the
end; the bindings suite with `| grep -E "^(FAILED|ERROR)|passed|failed" | tail -15`.

## 5. Stop and ask (question first in the report)

- an operation needs an atomic rename or a conditional write other than the manifest's and refs';
- cleanup on S3 would delete an object a local cleanup would keep (or the reverse);
- a local-disk test that passes today fails; the disk drops below 500 MB.

## 6. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; the object-store tests against moto (from `bindings/python`:
`pytest -o addopts= -q -rs tests/test_object_store.py tests/test_object_store_writes.py
tests/test_object_store_commits.py`); `python3 tools/pylance_suite.py | tail -4`; last, once, the full
bindings suite. Report under 40 lines: questions first, then files, per-operation status, tails,
decisions, `df -h /`.

**W4 outcome (2026-10-21): object-store writes done.** cleanup_old_versions (branch-aware), drop, branches
(create with a create-only ref, delete removes `tree/<B>/`, replace_metadata, tags on branches), shallow and
deep clones (local <-> s3 both ways; deep clones stream each object in 8 MiB chunks) work on `s3://`. Drop
on S3 keeps the local rule: nothing is deleted unless a readable manifest proves the prefix is a dataset.
Where an object-store listing fails, existence is assumed (refuse rather than overwrite). Local paths keep
their std::filesystem calls unchanged. The session stopped at its usage limit after the object-store tests
(141 passed); the reviewer ran ctest, the object-store and branch tests (172 passed), the pylance suite and
the full bindings suite. Only `migrate_manifest_paths_v2` stays local-only.
