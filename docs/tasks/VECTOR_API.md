# Task: vector-index API gaps (validation, describe / explain, cache stats, create_index options)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Roadmap order item 4
(the parts that need no new index file format; IVF_SQ / IVF_RQ / IVF_HNSW_PQ / Hamming formats are
the reviewer's).

## 0. Context budget (read this first)

About 100k tokens for the whole session:
- Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'` windows of at most ~120 lines.
  `bindings/python/nanolance/lance/dataset.py` is 3.5k+ lines, `src/vector_search.cpp` 2k+,
  `src/vector_index_build.cpp` 1.4k+.
- Always cut output: `| tail -15` (pytest; add `-o addopts=` for a summary line, the config passes
  `-q`), `| tail -5` for `tools/pylance_suite.py`, `ninja 2>&1 | grep -E "error|warning:" | head`.
- The oracle tests ARE the spec: read each one only (`grep -n "def test_x" -A50
  .deps/pylance-tests/v12.0.0/tests/test_vector_index.py`). When a test is unclear, run the same
  calls on real pylance (`import lance`) and copy what it does.
- Do only your phase.

## 1. Rules

- `AGENTS.md` sections 5 and 7 (grep the headings). A test may skip only for a missing optional
  dependency; never weaken / skip / delete a test; never wrap a failure in `except Exception`.
- Match surrounding code; errors through `_errors.py` (`native()`, `unsupported()`); C++ without
  warnings. Keep the diff to the listed tests.
- Do not commit, push, change branches, or run `tools/pylance_suite.py --update`.
- Builds after C++ changes: `cd /home/user/nanolance/build && ninja`, then
  `cd /home/user/nanolance/bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp _nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
- Iterate with `cd /home/user/nanolance && python3 tools/pylance_suite.py -k "<names joined by or>" 2>&1 | grep -E "^FAILED|test suite"`
  (a `tests/test_datagen.py` collection error shows up with `-k`; ignore it).

## 2. Stop and ask (question first in your report) when

- a test needs a new on-disk index format (IVF_SQ, IVF_RQ, IVF_HNSW_PQ, HNSW_FLAT, Hamming /
  binary vectors) -- list it as "needs format", do not attempt it;
- a fix would change the bytes nanolance writes for an index built today;
- you would need an API name not in pylance.

## 3. Phase V1a -- validation, describe / explain, cache stats, utilities

Tests (all in `tests/test_vector_index.py`):
`test_fragment_scan_disallowed_on_ann`, `test_vector_index_invalid_query_parallelism`,
`test_vector_index_invalid_approx_mode`, `test_scanner_rejects_unknown_index_segments`,
`test_batch_vector_search_rejects_dataset_query_index_column` (only the error -- batch search itself
is phase V2), `test_describe_vector_index`, `test_describe_index_runtime_hints_stored`,
`test_vector_index_with_nprobes` (explain-plan text: look at how `explain_plan` builds its lines
today, `grep -n "minimum_nprobes\|def explain_plan" bindings/python/nanolance/lance/dataset.py
src/vector_search.cpp`), `test_index_cache_size`, `test_index_cache_size_bytes`,
`test_index_cache_size_deprecation` (pylance's `index_cache_hit_rate`, `index_cache_entry_count` etc.;
nanolance has a process-wide index cache in C++ -- grep `load_ivf` / `cache` in `src/vector_search.cpp`;
counting hits there is fine), `test_target_partition_size_to_num_partitions_clamps`,
`test_validate_vector_index` (`lance.util` functions -- see what `bindings/python/nanolance/lance/util.py`
has; placeholders come from `_stubs.py`), `test_use_index`, `test_nested_field_vector_index`.

Note: `LanceDataset.has_index` was made a property (pylance's) just before this task; that fixed
6 tests. Do not change it back.

## 4. Phase V1b -- create_index options (a fresh session)

Tests: `test_index_with_pq_codebook`, `test_index_with_4bit_numpy_pq_codebook`,
`test_index_with_pq_codebook_rejects_wrong_num_bits_shape`, `test_pre_populated_ivf_centroids`,
`test_retrain_indices[None|False|True]`, `test_create_index_progress_callback_vector`,
`test_create_index_progress_callback_error_before_completion_propagates`,
`test_create_index_accelerator_fallback`, `test_index_with_nans[V3|Legacy]`,
`test_create_ivf_pq_skip_transpose`.

Facts: the option refusals are in `bindings/python/nanolance/lance/dataset.py`
(`grep -n "create_index option" bindings/python/nanolance/lance/dataset.py`). The C++ build already
takes a pretrained model for distributed builds -- `include/nanolance/vector_index*.hpp`, grep
`VectorIndexModel\|trained\|model` in `src/vector_index_build.cpp`, and how
`create_index_uncommitted` / `IndicesBuilder` pass `ivf_centroids` / `pq_codebook` in Python
(`grep -n "ivf_centroids\|pq_codebook" bindings/python/nanolance/lance/*.py`). Reuse that path for
`create_index(ivf_centroids=, pq_codebook=)`. Progress callbacks: scalar / FTS builds already report
progress (`index_build::Progress` in C++, `progress=` in Python) -- copy that for vector builds.

## 5. Done (each phase) -- report the tail lines

1. C++ (if touched): no warnings; `cd build && ctest -j8 2>&1 | tail -3` 100%.
2. The phase's tests via the `-k` command: as many pass as possible; the rest listed with reasons.
3. `python3 tools/pylance_suite.py 2>&1 | tail -4` ends "all N expected tests passed".
4. Last, once, nothing else running:
   `cd bindings/python && python3 -m pytest -o addopts= -q -x -p no:cacheprovider tests 2>&1 | tail -3`.
5. If you add behaviour beyond what pylance's tests check, add a nanolance-vs-pylance case to
   `bindings/python/tests/test_vector_search.py` (its existing style).

Report (under 40 lines): files changed; status per test; the tails; decisions not covered here;
questions.
