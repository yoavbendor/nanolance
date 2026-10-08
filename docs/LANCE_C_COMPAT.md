# lance-c compatibility (`liblance_c`)

nanolance builds `liblance_c`: the C API of the [lance-c](https://github.com/lance-format/lance-c)
project, implemented over nanolance. A program written against lance-c compiles and links against
nanolance unchanged:

- the same header, `#include <lance/lance.h>` (and the `lance.hpp` C++ wrapper);
- the same library name, `liblance_c`;
- the same CMake targets, `LanceC::lance_c` and `LanceC::lance_c_static`.

Nothing else is needed: no Rust toolchain, no Tokio runtime, no 100 MB static library.

```cmake
add_subdirectory(nanolance)            # or FetchContent
target_link_libraries(my_app PRIVATE LanceC::lance_c)
```

```c
#include <lance/lance.h>

LanceDataset* ds = lance_dataset_open("data.lance", NULL, 0);
LanceScanner* sc = lance_scanner_new(ds, (const char*[]){"id", "name", NULL}, NULL);
lance_scanner_set_limit(sc, 100);
struct ArrowArrayStream stream;
lance_scanner_to_arrow_stream(sc, &stream);
```

The headers in `compat/lance-c/include/lance/` are lance-c's own, copied unmodified (Apache-2.0,
The Lance Authors) from the commit in `compat/lance-c/UPSTREAM`. Every one of the 127 functions they
declare is defined in `src/lance_c.cpp`. What nanolance implements does the work: all but the 20
index-segment functions, and a few options (below). The rest fails
the way lance-c reports any failure: the function's documented error value, with
`lance_last_error_code()` set to `LANCE_ERR_NOT_SUPPORTED` and a message naming the feature. It never
returns a silently different result. nanolance is not affiliated with the Lance project.

## Implemented

| Group | Functions |
|---|---|
| Errors | `lance_last_error_code`, `lance_last_error_message`, `lance_free_string`, `lance_free_bytes` |
| Sessions | `lance_session_new` / `close` / `get_cache_stats` (nanolance keeps no shared cache, so the stats are zero), `lance_dataset_open_with_session` |
| Datasets | `lance_dataset_open` (latest or a version), `close`, `version`, `latest_version`, `count_rows`, `schema`, `fragment_count`, `fragment_ids`, `restore` |
| Versions | `lance_dataset_versions`, `lance_versions_count` / `id_at` / `timestamp_ms_at` / `close` |
| Statistics | `lance_dataset_calculate_data_stats`, `lance_data_statistics_count` / `field_id_at` / `bytes_on_disk_at` / `close` |
| Random access | `lance_dataset_take` (input order and repeats kept), `lance_dataset_take_rows` (by `_rowid`) |
| Scans | `lance_scanner_new` (projection), `set_limit`, `set_offset`, `set_batch_size`, `with_row_id`, `with_row_address`, `set_include_deleted_rows` (every stored row; a deleted one with a NULL `_rowid`, `with_row_id` required), `set_use_scalar_index`, `set_fragment_ids`, `set_blob_handling` (a blob column as its description by default, as its bytes with `ALL_BINARY`), `set_statistics_callback` (bytes and reads), the tuning setters (accepted, no effect on results), `to_arrow_stream`, `next` + `lance_batch_to_arrow` / `lance_batch_free`, `scan_async` + `async_stream_free`, `poll_next` (always ready: decoding is synchronous) |
| Filters | an SQL filter in `lance_scanner_new`, and `lance_scanner_additional_sql_filter` (combined with `AND`). The dialect is nanolance's SQL subset, shared with the Python module (`docs/PYLANCE_COMPAT.md`); with a filter, the scanner's offset and limit count the rows that pass. |
| Changes | `lance_dataset_delete`, `update`, `merge_insert` (every `when_matched` mode but `UPDATE_IF`, both `when_not_matched`, `when_not_matched_by_source` keep / delete / delete-if), `compact_files`, `drop_columns`, `alter_columns`, `add_columns_sql` / `add_columns_nulls` / `add_columns_stream`. Merge insert's `when_matched` modes are all there, `UPDATE_IF` included (its condition over `source.<column>` and `target.<column>`). Each commits one version and moves the handle to it, as lance-c does. A writer whose change is built on a replaced version gets `LANCE_ERR_COMMIT_CONFLICT`. |
| Blobs | `lance_dataset_take_blobs` (by row id) and `take_blobs_by_indices`, and the handles they return: `lance_blob_file_size` / `read` / `read_up_to` / `read_range` / `seek` / `tell` / `close`. Every Blob v2 storage kind Lance writes is read where it is, and only the bytes asked for: inline (in the data file), packed and dedicated (in the sidecar `.blob` files beside it), external (at its URI: a local path, `file://`, or `s3://` in a build with S3). A null value is a NULL handle; an empty one, a handle of size 0. |
| Writes | `lance_dataset_write`, `lance_dataset_write_with_params` (create / append / overwrite, `max_rows_per_file`, `max_bytes_per_file`), `lance_write_fragments` (data files, no manifest). A create records lance-c's auto-cleanup policy in the table config. nanolance does not reclaim versions itself; Lance applies the policy when it next commits. |
| Indexes | `lance_dataset_create_scalar_index` (BTREE, BITMAP, LABEL_LIST without parameters; INVERTED with its analyzer in `params_json`, Lance's `InvertedIndexParams` names, LanceDB's defaults for what is left out), `lance_dataset_create_vector_index` (IVF_FLAT, IVF_PQ; L2, cosine, dot), `lance_dataset_drop_index`, `lance_dataset_index_count` and `lance_dataset_index_list_json`. Scans use BTree, Bitmap and LabelList indexes, whoever built them. The changes above keep the indices a dataset has, by Lance's rules, and compaction gives its rewritten rows back to them (`docs/PYLANCE_COMPAT.md`, "Indexes"). |
| Vector search | `lance_scanner_nearest` (float32, float16, float64, uint8 and int8 queries), `set_nprobes` / `set_minimum_nprobes` / `set_maximum_nprobes`, `set_refine_factor`, `set_metric`, `set_use_index`, `set_prefilter`, with the scanner's filter, projection, offset and limit. The rows come back nearest first: the columns asked for, then `_distance`, then the row id columns, as Lance returns them. IVF_FLAT and IVF_PQ indexes are searched as pylance searches them (`docs/VECTOR_INDEX.md`); a column without one is searched exactly. `set_approx_mode`, `set_ef` and `set_query_parallelism` are validated and change nothing: in Lance they only affect HNSW and binary-quantized searches. |
| Full-text search | `lance_scanner_full_text_search` (the query over the columns given, or every indexed column), and prepared contexts: `lance_dataset_prepare_fts_match_query` (OR / AND) and `lance_dataset_prepare_fts_query`, with STRICT coverage (every fragment indexed, or the prepare fails) and INDEX_ONLY (the indexed rows alone, scored by the index's statistics), `lance_scanner_set_fts_query_context` (only on a scanner of the same snapshot) and `lance_fts_query_context_close`. Rows come back best first, with `_score`, bit for bit pylance's scores (`docs/FTS_INDEX.md`). |

URIs: local paths, `file://`, and `memory://` (a directory private to the process). Object stores
(`s3://`, ...) return `LANCE_ERR_NOT_SUPPORTED`. `storage_opts` is accepted and ignored.

## Not implemented yet

- **Index segments** (20 functions): building an index in pieces and committing them
  (`lance_index_segment_builder_*`, `lance_index_train_ivf_model` / `train_pq_model`,
  `lance_index_segment_metadata_*`, `lance_dataset_commit_index_segments`), listing a logical index's
  segments (`lance_dataset_index_segment_count`, `lance_dataset_index_segments`), and restricting a
  search to some of them (`lance_scanner_set_index_segments`, `set_scalar_index_segment`,
  `set_fts_index_segments`: passing none succeeds).
- Vector indexes other than IVF_FLAT and IVF_PQ (IVF_SQ, IVF_HNSW_*), the Hamming metric, and
  multi-vector search (`lance_scanner_nearest_multivector`).
- Fuzzy full-text matching (`max_fuzzy_distance > 0`) and phrase queries
  (`lance_dataset_prepare_fts_phrase_query`): nanolance's INVERTED indexes store no positions, and it
  does not search an index that does. An index without positions gets lance-c's own error.
- Parameters for BTREE, BITMAP and LABEL_LIST indexes (`params_json`).
- Nearest or full-text search combined with `set_fragment_ids`.
- Substrait filters, object stores, writing Lance's inline, packed and dedicated blob layouts
  (nanolance writes external blobs; it reads all of them), and datasets with stable row ids.

`tests/test_lance_c_search.cpp` drives the search API end to end through the C API (a dataset
written with `lance_dataset_write`, its indexes built with lance-c's calls) and compares every
result with nanolance's own search, which the Python tests check against pylance: rows, order,
`_distance` / `_score`, the columns, offset and limit, filters, contexts and their coverage modes,
`include_deleted_rows`, `UPDATE_IF`, and the documented errors. It runs clean under AddressSanitizer
and UBSan.

## lance-c's own tests

`tools/lance_c_suite.py` fetches lance-c's `tests/cpp/test_c_api.c` and `test_cpp_api.cpp` at the
pinned commit into `.deps/`. It runs them against `liblance_c` on the datasets lance-c's harness
builds (`tools/lance_c_fixtures.py`), once written by pylance and once by nanolance:

```sh
python tools/lance_c_suite.py            # build, run, fail if a listed test no longer passes
python tools/lance_c_suite.py --update   # rewrite tests/lance_c/expected_pass.txt
```

Each upstream file is a single program that stops at its first failed assertion. So a generated
driver `#include`s it and runs each test function in its own child process, in the order its
`main()` does. It is built with `NDEBUG` undefined so the `assert()`s of the C++ test are live. CI
runs the suite in `.github/workflows/bindings-python.yml`.

**90 of 106 pass: 18 of 22 C tests and 27 of 31 C++ tests, per fixture writer.** Every test that
fails needs index segments. It was 88 before scalar indexes, 52 of 106 before
filters and dataset changes, and 80 before Lance's blob layouts were read. (An earlier version of this page said "52 of 80": the runner
missed tests whose result line was printed on the same line as the test's own output, so failing
tests went uncounted. It now parses every result and fails if the count does not match the tests
the driver ran.) Every implemented group above has a test that passes. The ones that fail today:

| Tests | Why |
|---|---|
| index_segment_builder(_progress), vector_models_and_reusable_segments, commit_index_segments | index segments: not implemented yet |

`nearest_smoke`, `fts_smoke`, `index_segments_smoke` and `multivector_rejects_flat_column` are smoke
tests: upstream wrote them to prove the wrappers compile, link and report errors, and they accept an
error as the outcome. `fts_smoke` now builds its INVERTED index and searches it.

The same run under AddressSanitizer and UBSan (`build-asan`) reports no error in `liblance_c`.

## Found on the way

The blob tests found that nanolance read Blob v2 only in its own layout. That layout has external
blobs only and no nulls. A pylance-written blob column (inline, packed, dedicated or empty values,
or any null) failed to read: each row's definition level sat in front of the descriptor, and
nanolance read every row one byte off. They also found a quieter bug. In a batch that holds a blob
column, every other column read its nulls back as values: the row-by-row batch builder that blob
columns need never looked at validity. Both are fixed, and `test_pylance_blobs_read_back` in the
Python tests pins them against pylance.

The fixtures, a dataset with a non-nullable column appended to, exposed a nanolance reader bug: the
Arrow schema it returned marked every field nullable, whatever the manifest said. Reading a table
and appending it back to the same dataset then failed on a nullability mismatch. The schema now
says what the manifest says.
