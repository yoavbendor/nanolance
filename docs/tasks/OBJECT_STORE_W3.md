# Task: index builds and fragment-level writes on s3:// datasets (object-store writes, phase W3)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Design:
`docs/OBJECT_STORE_WRITES.md` (sections 2, 3, 5). W1 (create / append / overwrite, `io::ByteSink`) and W2
(`docs/tasks/OBJECT_STORE_W2.md`: mutations, schema changes, compaction) are done. Two sessions: **W3a**
(index builds and maintenance), **W3b** (fragment-level writes and hand-built commits). Do only the phase
you are given. W4 (cleanup, drop, branches, clones, migrate) comes later: keep refusing those by name.

## 0. Context budget

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines. Big ones: `src/vector_index_build.cpp`, `src/fts_index_build.cpp`,
`src/scalar_index.cpp`, `bindings/python/nanolance/lance/dataset.py` (5k+). Cut output: `| tail -15` for
pytest (add `-o addopts=`).

## 1. Goal

- **W3a:** on an `s3://` dataset: `create_scalar_index` (every kind nanolance builds: BTREE, BITMAP,
  LABEL_LIST, INVERTED, NGRAM, ZONEMAP, BLOOMFILTER, JSON), `create_index` (IVF_FLAT, IVF_PQ, IVF_SQ,
  IVF_HNSW_*, IVF_RQ, Hamming IVF_FLAT), `drop_index`, `optimize.optimize_indices`, the distributed
  segment API (`create_index_uncommitted`, `merge_existing_index_segments`, `commit_existing_index_segments`,
  `merge_index_metadata`). Each one's index answers queries on S3 as the same index on a local copy, and
  pylance uses nanolance's S3 index (and the reverse).
- **W3b:** `LanceFragment.create` / `create_from_file`, `write_fragments`, fragment `merge_columns`,
  `add_columns` from a function (`BatchUDF`), `merge` (column join), and `LanceDataset.commit` of every
  hand-built operation (`_refuse_commit` in `dataset.py` allows only Delete today).

## 2. Facts

- Remaining filesystem calls on index build paths: `src/fts_index_build.cpp` ~915-972,
  `src/vector_index_build.cpp` ~2061-2372, `src/scalar_index.cpp` ~1788-1883 (an index directory
  created, and `remove_all` of it on failure), `src/index_optimize.cpp` ~475 (`remove_all` of a merged
  segment's directory). On an object store there are no directories: skip creating them
  (`io::is_object_store_output`), and replace `remove_all` of a failed build by removing the objects it
  wrote (`io::list_dir` + `io::remove_output`, best effort: an unreferenced index directory is harmless,
  cleanup removes it). Index files themselves go through `src/index_files.cpp` `write_file` (check: does
  it reach `io::open_output`?).
- Python refusals to lift: `grep -n "refuse_object_store_write" bindings/python/nanolance/lance/dataset.py
  bindings/python/nanolance/lance/fragment.py` (the index ones for W3a; fragment / merge / UDF / commit for
  W3b). Keep: cleanup, drop, branches, clones, migrate (W4).
- Measure pylance's request sequence for an index build on moto first (one BTREE, one IVF_PQ): which
  objects it PUTs, in what order, and whether anything but the manifest is conditional.

## 3. Measuring and testing

As in `docs/tasks/OBJECT_STORE_W2.md` section 3 (moto on **your own port**, not 5077; stop only the
process you start, by PID). Extend `bindings/python/tests/test_object_store_commits.py`: per operation,
the s3 result against a local copy (queries: `nearest`, `full_text_query`, filters using the scalar index,
with the same rows and distances / scores), pylance searching nanolance's S3 index and the reverse; remove
each operation from `WRITES` in `tests/test_object_store.py`. One race: an index build vs an append on S3
(the build is rebased, not re-run: the C2 rebase applies on S3 as locally).

## 4. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. The local
native build has `-DNANOLANCE_WERROR=ON`; delete helpers you stop using; check the wasm build
(`source /home/user/emsdk/emsdk_env.sh`, configure as in `.github/workflows/wasm.yml` into a directory under
/tmp, build, delete it). **A local-disk write must produce exactly the files and bytes it did before.** Do
not add fields to Python classes that mirror pylance's. Do not commit, push, change git branches, or run
`--update`. Scratch only in `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/w3-idx/`.
Builds: `cd build && ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`. **The disk is
nearly full (about 1.7 GB free)**: no extra build trees; do not delete `/tmp/pytest-of-root` or `/tmp/tmp*`
(the reviewer does); report `df -h /` at the end. Run each full suite once, at the end; the bindings suite
with `| grep -E "^(FAILED|ERROR)|passed|failed" | tail -15`.

## 5. Stop and ask (question first in the report)

- an index build needs a rename or a conditional write other than the manifest's;
- pylance writes something for an index on S3 that nanolance has no equivalent for;
- a local-disk test that passes today fails; the disk drops below 500 MB.

## 6. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; the object-store tests against moto (from `bindings/python`:
`pytest -o addopts= -q -rs tests/test_object_store.py tests/test_object_store_writes.py
tests/test_object_store_commits.py`); `python3 tools/pylance_suite.py | tail -4`; last, once, the full
bindings suite. Report under 40 lines: questions first, then files, per-operation status, tails,
decisions, `df -h /`.

**W3a outcome (2026-10-20):** every index kind nanolance builds (BTREE, BITMAP, LABEL_LIST, INVERTED,
NGRAM, ZONEMAP, BLOOMFILTER, JSON; IVF_FLAT, IVF_SQ, IVF_PQ, IVF_RQ, IVF_HNSW_*, Hamming IVF_FLAT),
drop_index, optimize_indices and the distributed segment API work on `s3://`; pylance uses nanolance's S3
indexes and the reverse. Index directories are not created on object stores, and a failed build removes
the objects it wrote (`index_files` create_index_dir / remove_index_dir); the legacy staged INVERTED parts
go through native I/O (they used os.makedirs / open, which would have made a local `s3:` directory).
Measured: pylance's index requests are plain PUTs; only the manifest is conditional. Approximate kinds
(IVF_PQ, IVF_RQ, IVF_HNSW_*) train a fresh model per build, so their tests compare S3 with pylance on the
same index exactly and with a local build by overlap; exact kinds are compared exactly.

**W3b outcome (2026-10-20):** `LanceFragment.create` / `create_from_file`, `write_fragments`, fragment
`merge_columns` (SQL, data, UDF), `add_columns` from a function, `merge` and `LanceDataset.commit` /
`commit_batch` of every hand-built kind work on `s3://` (Append, Overwrite, Merge, Update, Rewrite, Restore,
Project, UpdateConfig, DataReplacement, UpdateBases via `add_bases`), equal to a local copy as pylance reads
them, including after a deletion; a pylance-written base takes nanolance's fragments; two processes committing
hand-built appends lose and double nothing. The Python refusals are gone (`_refuse_commit` removed). Found and
fixed: `LanceFragment.create`, `write_fragments` and `commit` took `storage_options` and ignored them, so a
worker process that only wrote and committed had no credentials (they are registered per bucket now, as
`write_dataset` does). `DataFile.create` reads the footer and size natively (`_file_top_fields`, a new
`_file_size`). DataOverlay is not tested: pylance 12 cannot read the manifest flags it writes, on local disks too.
Reviewer (W3b): fragment writes and commits register the bucket's storage_options as write_dataset does
(a worker that only writes fragments and commits needs them). Open, unverified: the session reports that a
hand-built DataOverlay commit writes a manifest pylance 12 cannot read (feature flag 64), locally too --
check against `tests/test_data_overlay*.py` before W4.
