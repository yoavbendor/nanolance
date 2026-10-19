# Task: progress callbacks (index builds and fragment writes) as pylance reports them

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP "Index plan
for AI users", Wave 1, item 2. One session.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 100k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines (`bindings/python/nanolance/lance/dataset.py` is 3.5k+ lines,
`fragment.py` large too). Cut output: `| tail -15` for pytest (add `-o addopts=`), `| tail -5` for
`tools/pylance_suite.py`.

## 1. Goal

Five of pylance's tests fail (`.deps/pylance-tests/v12.0.0/tests`; run them through nanolance as
`tools/pylance_suite.py` does):
- `test_dataset.py::test_dataset_progress`: `write_dataset(progress=...)` never calls the
  `FragmentWriteProgress`;
- `test_fragment.py::test_dataset_progress`: `lance.progress.FileSystemFragmentWriteProgress` is refused;
- `test_scalar_index.py::test_create_inverted_index_progress_callback` and
  `..._error_after_completion_is_ignored`: the INVERTED build's stages differ from pylance's;
- `test_vector_index.py::test_create_index_progress_callback_vector`: IVF_PQ stages in another order.
Make all five pass with pylance's exact events. Everything here is Python (`nanolance/lance/`); the
C++ library is not touched unless a stage boundary is only visible there (then stop and ask).

## 2. Facts (measured on pylance 12.0.0 -- do not re-derive)

Index progress events (attributes `event`, `stage`, `completed`, `total`, `unit`), in order:

INVERTED, 2 fragments x 75 rows, `remove_stop_words=False` (12 events):
```
start load_data 0 None rows       complete load_data 0 None rows
start tokenize_docs 0 None rows   progress tokenize_docs 75 None rows   progress tokenize_docs 150 None rows
complete tokenize_docs 150 None rows
start copy_partitions 0 1 partitions   progress copy_partitions 1 1 partitions   complete copy_partitions 1 1 partitions
start write_metadata 0 1 files    progress write_metadata 1 1 files    complete write_metadata 1 1 files
```
(`tokenize_docs` progresses once per fragment, cumulative rows; `copy_partitions` counts the index's
partitions; no `merge_partitions` stage.) A callback that raises on `complete:write_metadata` is
ignored: the index is committed and the error does not propagate (the last event recorded is that
one). A callback that raises before completion propagates (already passing; keep it).

IVF_PQ, 1500 x 128 float32, 4 partitions, 4 sub-vectors (21 events):
```
start train_ivf 0 50 iterations, then one "progress train_ivf k 50 iterations" per k-means iteration
  actually run (8 here), complete train_ivf <iterations run> 50 iterations
start train_quantizer 0 None ''        complete train_quantizer 0 None ''
start shuffle 0 1500 rows      progress shuffle 1500 1500 rows      complete shuffle 1500 1500 rows
start merge_partitions 0 4 partitions, progress merge_partitions 1..4 4 partitions, complete 4 4
```
The test checks the order of starts and completes, the last `shuffle` progress = row count, and the
last `merge_partitions` progress = number of partitions. nanolance emits `train_quantizer` after
`shuffle` today (order seen: `[0, 1, 4, 5, 2, 3, ...]`).

Fragment write progress (`lance.progress.FragmentWriteProgress`: `begin(fragment, **kwargs)`,
`complete(fragment)`):
- `write_dataset(10 rows, max_rows_per_file=5, progress=p)`: `begin` (fragment id 0, no files,
  `physical_rows` None), `complete` (id 0, 1 file, 5 rows), then the same again for the second file:
  pylance reports id 0 for both, since ids are assigned at commit.
- `write_dataset(..., mode="append", progress=p)` with 3 rows: only `begin` is called.
- `LanceFragment.create(uri, reader, fragment_id=7, progress=p)` whose reader raises after one batch:
  `begin` (id 7, 1 file) is called, then the write fails with `OSError`.
- `FileSystemFragmentWriteProgress(base_uri, metadata=None)` is pure Python in pylance:
  `/usr/local/lib/python3.11/dist-packages/lance/progress.py` lines 121-190. `begin` writes
  `fragment_{id}.in_progress` (JSON with `fragment_id` and `metadata`) and `fragment_{id}.json` (the
  fragment's JSON); `complete` deletes the `.in_progress` file. Port it into
  `nanolance/lance/progress.py`, keeping pylance's Apache-2.0 header and copyright line as they are in
  that file (copy them from the file, never from memory). It uses `pyarrow.fs`, which nanolance may use.

## 3. Where things are in nanolance

- Progress today: `grep -n "report(\|progress_callback\|IndexProgress" bindings/python/nanolance/lance/*.py`
  (for example `dataset.py` about line 1422 reports `remap_partition_files`).
- Fragment writes: `grep -n "def write_dataset\|def create\b\|progress" bindings/python/nanolance/lance/dataset.py bindings/python/nanolance/lance/fragment.py`.
- The refusal of `FileSystemFragmentWriteProgress`: `grep -rn "FileSystemFragmentWriteProgress" bindings/python/nanolance/`.

## 4. Tests you write

`bindings/python/tests/test_progress.py` (`require_pylance()`): for each of the measured cases above,
run both engines and assert equal event lists (for IVF_PQ, compare the stage sequence and the counts
the pylance test checks, since the number of k-means iterations may differ); both write-progress cases;
`FileSystemFragmentWriteProgress` files after a successful and a failing `LanceFragment.create`.

## 5. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures
(the "error after completion is ignored" case catches only around that one callback, as pylance does).
Index builds must not get slower: callbacks are only called when given. Do not commit, push, change
branches, or run `--update`. Scratch files only in
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/pg/`. If you touch C++:
`cd build && ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`. Run each
full suite once, at the end, one at a time; never two at once; no sleep loops; kill by PID.

## 6. Stop and ask (question first in the report)

- a stage boundary pylance reports is not observable from Python (it would need a C++ callback);
- matching pylance's write-progress quirks (id 0 for every fragment, no `complete` on append) needs a
  change to how fragments are written or committed.

## 7. Done and report

Tails of: the five target tests run with pytest through nanolance; `python3 -m pytest -o addopts= -q
bindings/python/tests/test_progress.py | tail -2`; `python3 tools/pylance_suite.py | tail -4`; last,
once: `cd bindings/python && python3 -m pytest -o addopts= -q -x -p no:cacheprovider tests | tail -3`.
Report under 40 lines: questions first, then files, per-test status, tails, decisions.
