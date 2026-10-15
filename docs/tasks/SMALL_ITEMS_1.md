# Task: small items bundle 1 -- row-address masks, VectorIndexReader, `_take_rows` of deleted rows

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Three independent
items; do them in order, one session may finish all three.

## 0. Context budget

About 100k tokens. `grep -n` then `sed -n` windows of at most ~120 lines (`dataset.py` 3.5k+,
`src/lance_table_reader.cpp` 3k+, `src/vector_search.cpp` 2k+). Cut all output (`| tail -15`; pytest
with `-o addopts=` for a summary line; `tools/pylance_suite.py ... | tail -5`). The pylance tests are
the spec: read only the named ones. When unclear, run the same calls on real pylance (`import lance`).

## 1. Row-address masks: `row_addr_allowlist` / `row_addr_blocklist`, `lance.dataset.serialize_row_addrs`

Oracle: all of `.deps/pylance-tests/v12.0.0/tests/test_row_addr_prefilter.py` (12 tests).

Facts (measured):
- `serialize_row_addrs([0, 2, 4, ...])` returns Lance's **RowAddrTreeMap** serialization: u32 count of
  fragments, then per fragment ascending `[u32 fragment id][u32 byte size][portable Roaring bitmap of
  the row offsets]`. nanolance already writes exactly this (`serialize_treemap` in
  `src/scalar_index.cpp`) and reads it (`read_treemap` in `src/index_search.cpp`). For
  `[1, 2, (1<<32)|5]` pylance returns 58 bytes starting `02 00 00 00 00 00 00 00 14 00 00 00 3a 30 ...`
  -- your Python output must be byte-identical (compare with pylance in your test). The Python roaring
  encoder exists: `_serialize_roaring` in `bindings/python/nanolance/lance/bitmap.py`.
- `scanner(row_addr_allowlist=bytes, row_addr_blocklist=bytes)` and `ScannerBuilder` setters (see
  `test_builder_setter_matches_the_kwarg`): the scan keeps only rows whose address is in the allowlist
  and not in the blocklist, **before** filters, limits and ranking -- a KNN or full-text search computes
  its top-k over the surviving rows (`test_knn_topk_is_computed_over_masked_rows`). Malformed bytes:
  an error matching `(?i)row address mask|invalid`.
- Implementation hint: masks are row sets per fragment, like the scalar-index candidates the scan
  already takes (`PlannedFile::candidates` in `src/lance_table_reader.cpp`; how `index_candidates`
  feeds them in). Vector search has a `RowMask` (`src/vector_search.cpp`), full-text search a `RowMask`
  in `src/fts_search.cpp`: intersect the caller's mask there. Pass the mask from Python as bytes to the
  native call; decode it once in C++.
- The two `test_overlay_stale_replay_*` tests need data overlays, which now work (`src/overlays.cpp`):
  they must pass once the mask is applied on every row source.

## 2. `lance.dataset.VectorIndexReader`

Oracle: `tests/test_vector_index.py::test_nested_field_vector_index` (and any other test using
`VectorIndexReader`; grep). Facts: `VectorIndexReader(dataset, index_name)`, `num_partitions()`,
`read_partition(partition_id, with_vector=False)` -> a pyarrow Table with `_rowid` (uint64) only, plus
with `with_vector=True` the storage column: `flat` (FixedSizeList<float32>[dim]) for IVF_FLAT,
`__pq_code` (FixedSizeList<uint8>[m]) for IVF_PQ -- copy the exact schema and row order real pylance
returns on the same index, for IVF_FLAT, IVF_PQ and IVF_HNSW_SQ (`__sq_code`). The partition data is
in the index's `auxiliary.idx` (offsets/lengths per partition are in the IVF model nanolance already
decodes: `load_ivf` / `IvfIndex` in `src/vector_search.cpp`). With stable row ids `_rowid` is the id.

## 3. `_take_rows` of a deleted row's address

`ds._take_rows([address_of_a_deleted_row, ...])` returns the deleted row in nanolance; pylance leaves it
out (datasets without stable row ids). Reproduce: `write_dataset` 300 rows, `delete("id % 4 = 1")`,
`_take_rows([(0 << 32) | 5])` -> pylance `[]`. Fix where `_take_rows` resolves addresses
(`grep -n "def _take_rows" bindings/python/nanolance/lance/dataset.py`, and `resolve_row_ids` in
`src/row_ids.cpp`, which skips deleted rows only for stable ids). Add the case to
`bindings/python/tests/test_take.py` against pylance.

## 4. Rules, builds, stop rules, done

`AGENTS.md` sections 5 and 7. No skipped / weakened tests. C++ without warnings. Do not commit, push,
change branches, or run `--update`. Builds: `cd build && ninja`, then
`cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
Stop and ask (question first) if a mask would need the reader's file planning changed beyond the
existing candidates path, or if VectorIndexReader's output differs between pylance index kinds in a way
section 2 does not cover.

Tests you write: `bindings/python/tests/test_row_addr_masks.py` (both engines: same rows for scans,
filters, KNN, FTS under allow / block lists; `serialize_row_addrs` bytes equal pylance's), plus the
cases in sections 2-3.

Done -- report tails of: `cd build && ctest -j8 | tail -3`; the oracle `-k` runs; your test files;
`python3 tools/pylance_suite.py | tail -4` ("all N expected tests passed"); last, once, quiet:
`cd bindings/python && python3 -m pytest -o addopts= -q -x -p no:cacheprovider tests | tail -3`.
Report under 40 lines: files, per-item status, tails, decisions, questions.

**Session 1 (2026-10-15):** `serialize_row_addrs` done (byte-identical to pylance;
`tests/test_row_addr_masks.py`). Stopped at the stop rule on the scan mask. **Reviewer's design for the
rest of item 1:**
- In the reader (`src/lance_table_reader.cpp`), a row the mask excludes is dropped exactly like a
  deleted row: build the fragment's `keep` vector from its deletion file AND the mask (allowlist: only
  listed offsets; blocklist: listed offsets off), so slicing, compaction and row ids keep working.
  A fragment with no allowed rows is skipped at planning.
- With a mask present, offset / limit are applied after the read (`plan.post_range`, as a filter
  already does): the planning arithmetic over manifest row counts must never see masked rows. That is
  the one planning change approved; nothing else in the planner changes.
- KNN and FTS: intersect the existing `RowMask` (vector_search.cpp, fts_search.cpp) with the caller's
  mask, decoded once in C++ from the bytes (`read_treemap`'s format); the exhaustive / unindexed scans go
  through the reader and get it there.
- Oracle: run pylance's tests through `python3 tools/pylance_suite.py -k "row_addr or serialize_row_addrs or allowlist or blocklist"`
  -- the runner loads nanolance as `lance`; plain `pytest` in `.deps/pylance-tests` tests pylance itself.
- Add tests to `test_row_addr_masks.py` only as they pass (both engines, same rows).

**Session 2 (2026-10-15):** item 1 done -- `row_addr_allowlist` / `row_addr_blocklist` in scans, filters,
KNN (all index kinds), FTS and overlay rows, the malformed-mask error; all 12 of pylance's
`test_row_addr_prefilter.py`. A mask turns off scalar-index candidates for that scan (correct, slower);
a mask with `include_deleted_rows` masks the table scan only (untested). Items 2 and 3 next.
