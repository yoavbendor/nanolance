# Task: mutations on s3:// datasets (object-store writes, phase W2)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Design:
`docs/OBJECT_STORE_WRITES.md` (read sections 2, 3 and 5 first). W1 is done: create / append / overwrite on
`s3://`, the `io::ByteSink` layer (`include/nanolance/byte_sink.hpp`), the create-only manifest publish,
the conditional-write probe, the fault hook. W2 makes the **mutations** work on `s3://` too. Two sessions:
**W2a** (deletes, updates, merge_insert, config / metadata, restore, tags), **W2b** (schema changes --
add / drop / alter columns -- and compaction). Do only the phase you are given. W3 (indexes) and W4
(cleanup, drop, branches) come later: keep refusing those by name.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines. Big ones: `src/dataset_ops.cpp`, `bindings/python/nanolance/lance/dataset.py`
(5k+), `src/dataset_refs.cpp`, `src/deletion_vector.cpp`. Cut output: `| tail -15` for pytest (add
`-o addopts=`).

## 1. Goal

Each operation below, on an `s3://` dataset opened with pylance's `storage_options`, does what it does on a
local dataset: same versions, same rows, same transaction kinds, and pylance reads the result. Today the
Python layer refuses them (`refuse_object_store_write(..., "<op>")` in `dataset.py`), and the C++ paths
still use `std::filesystem` / `std::ofstream` / `fopen` for what they write or read.

- **W2a:** `delete`, `update`, `merge_insert`, fragment `delete`, `update_config` / `delete_config_keys`,
  `update_metadata` / `update_schema_metadata` / `update_field_metadata`, `restore`, tags (`tags.create`,
  `tags.update`, `tags.delete`: `_refs/tags/<name>.json`).
- **W2b:** `add_columns` (SQL, nulls, data), `drop_columns`, `alter_columns`, `optimize.compact_files`.

## 2. Facts

- Every write goes through `io::open_output(path_or_uri)` -> `OutputFile` (an `std::ostream`;
  `commit(error)` makes the object exist) or `io::put_if_absent` (create-only); removals through
  `io::remove_output`. On an object store there are no directories: guard `create_directories` and
  `std::filesystem::exists` with `io::is_object_store_output(path)` (see W1's `src/writer.cpp`).
- Reads go through `io::open_source` / `io::read_all` / `io::list_dir` (`include/nanolance/byte_source.hpp`),
  which already work on `s3://`. A write path that reads an existing file with `std::ifstream` /
  `fopen` / `std::filesystem` must switch to these.
- Remaining filesystem calls on write paths (from `grep -n "std::filesystem::\(create_directories\|remove\|
  rename\|exists\|directory_iterator\|file_size\|copy\)\|std::ofstream\|std::ifstream\|fopen(" src/*.cpp`):
  `src/deletion_vector.cpp` (3: the deletion-file writer -- W2a), `src/dataset_refs.cpp` (1: refs JSON --
  tags, W2a), `src/dataset_stitcher.cpp` (5: column stitching -- W2b), `src/nano_lance_external_blob.cpp`
  (2), `src/dataset.cpp` (1), `src/data_file_reader.cpp` (1), `src/manifest_reader.cpp` (1); index
  builders (`fts_index_build.cpp`, `vector_index_build.cpp`, `scalar_index.cpp`, `index_optimize.cpp`:
  W3, not now). Check each one you touch: is it on the path of an operation of your phase?
- Tags on S3: pylance writes `_refs/tags/<name>.json` with a plain PUT and deletes it with a DELETE
  (measure on moto: the moto server log shows each request; see "Measuring" below).
- Lance on S3 never renames. If an operation uses `std::filesystem::rename` locally, find what pylance does
  on S3 (moto log) and do that.

## 3. Measuring and testing

- moto: `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/motoenv/bin/moto_server -H
  127.0.0.1 -p <free port>` (start it in the background with `nohup ... > <scratch>/moto.log 2>&1 &`; create
  the bucket with `curl --noproxy '*' -s -X PUT http://127.0.0.1:<port>/nl-test`; bucket names have at
  least 3 characters). Always `NO_PROXY=127.0.0.1 no_proxy=127.0.0.1`. Kill it by PID when done.
- The request sequence pylance makes for an operation: run it through pylance against moto, then
  `grep -E '"(PUT|POST|DELETE|GET|HEAD) ' moto.log` (moto colours non-2xx lines; strip `\x1b[...m` first).
- Tests: extend `bindings/python/tests/test_object_store_commits.py` (it is skipped unless
  `NANOLANCE_S3_TEST_ENDPOINT` is set; run with `NANOLANCE_S3_TEST_ENDPOINT=http://127.0.0.1:<port>`).
  For each operation: nanolance does it on an s3 dataset, pylance reads the result (rows, version, the
  transaction kind) equal to doing it on a local copy; and one pylance-then-nanolance case. Remove the
  operation from `WRITES` in `tests/test_object_store.py` (the refusal list). A race case for delete vs
  delete on S3 (two processes, as `test_racing_writers_lose_and_double_nothing`).

## 4. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. The
local native build has `-DNANOLANCE_WERROR=ON`; delete helpers you stop using; check the wasm build
(`source /home/user/emsdk/emsdk_env.sh`, configure as in `.github/workflows/wasm.yml` into a directory under
/tmp, build, delete it). **A local-disk write must produce exactly the files and bytes it did before.** Do
not add fields to Python classes that mirror pylance's. Do not commit, push, change git branches, or run
`--update`. Scratch files only in `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/w2-work/`.
Builds: `cd build && ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`. Disk is small
(~4 GB free): delete `/tmp/pytest-of-root` and `/tmp/tmp*` after test runs. Run each full suite once, at
the end; the bindings suite with `| grep -E "^(FAILED|ERROR)|passed|failed" | tail -15`.

## 5. Stop and ask (question first in the report)

- an operation needs an atomic rename or a conditional write other than the manifest's;
- pylance's request sequence for an operation writes something nanolance has no equivalent for;
- a local-disk test that passes today fails.

## 6. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; the object-store tests against moto (`pytest -o addopts= -q -rs
tests/test_object_store.py tests/test_object_store_writes.py tests/test_object_store_commits.py`, run from
`bindings/python`); `python3 tools/pylance_suite.py | tail -4`; last, once, the full bindings suite. Report
under 40 lines: questions first, then files, per-operation status, tails, decisions.

**W2a outcome (2026-10-20):** delete, update, merge_insert, fragment delete (and its hand-built Delete
commit), restore, config / table / schema / field metadata updates, tags (create / update / delete,
replace_metadata) work on `s3://`, equal to a local copy as pylance reads them; two processes deleting
disjoint ranges both land. Found and fixed: a native delete on `s3://` wrote its deletion file into a
local directory named `s3:` (fopen). Reviewer's decisions: a tag create is a create-only PUT (a racing
create cannot silently replace another's tag; pylance's plain PUT would), a tag delete one DELETE (pylance:
HEAD then a batch POST ?delete; same end state).

**W2b outcome (2026-10-20): W2 done.** add_columns (SQL, nulls, data), drop_columns, alter_columns,
optimize.compact_files and the Compaction plan / execute / commit steps work on `s3://`, equal to a local
copy as pylance reads them (Python-only change: the C++ paths already wrote through the W1 sink); races of
two schema changes and of compaction vs appends lose nothing. Still refused by name, moved to W3 with the
index builds: add_columns from a function and `merge` (fragment-level writes and hand-built Merge commits).
