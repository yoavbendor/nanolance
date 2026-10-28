# api: the C APIs and lance-c

<!-- layer-deps:start -->
Layer 11 of 11 (1 is the bottom, 11 the top). Uses: [base](../base/README.md), [io](../io/README.md), [arrow](../arrow/README.md), [file](../file/README.md), [table](../table/README.md), [expr](../expr/README.md), [scan](../scan/README.md), [index](../index/README.md), [dataset](../dataset/README.md). Used by: none. See [docs/LAYERS.md](../../docs/LAYERS.md).
<!-- layer-deps:end -->

The api layer is what programs outside nanolance link against: nanolance's own C writer and reader
(`nano_lance_writer.h`, `nano_lance_reader.h`), their C++ wrappers (`writer.hpp`, the compile-time
`typed_writer.hpp`), the hooks the dataset layer's operations use on a writer (`writer_internal.hpp`), and
`liblance_c`, lance-c's C API implemented over nanolance. It holds no format logic: every call is a thin
translation (C types and status codes in, the dataset and scan layers' calls, C results and messages out). The
Lance specification has no API part, so its table follows lance-c's API (`docs/LANCE_C_COMPAT.md`) and nanolance's
own headers.

## Using it

- **Writer** (`include/nanolance/nano_lance_writer.h`, `src/api/nano_lance_writer.cpp`, over the dataset layer's
  `WriteSession`): `nano_lance_writer_open(&w, path, &options)` with a zeroed `NanoLanceWriteOptions` for the
  defaults, then `nano_lance_write_batch` (any number), `nano_lance_writer_commit` (each a fragment and, unless
  `stage_fragments`, a version) or `nano_lance_writer_finish` (all staged fragments as one version, CREATE /
  APPEND / OVERWRITE), then `nano_lance_writer_close`, always, also after an error. `nano_lance_writer_init` /
  `_init_append` and the `set_*` calls are the older spelling: a setter after the first batch returns
  `NANO_LANCE_INVALID_STATE`. Batches are copied unless `borrow_buffers`, in which case their fixed-width buffers
  must stay valid until commit. Every call returns a `NanoLanceStatus`; `nano_lance_writer_last_error(&w)` says
  why. A writer is used from one thread.
- **C++ writers**: `nano_lance::Writer` (`writer.hpp`, `src/api/writer_cxx.cpp`) is the same writer with RAII and
  `bool` results; `nano_lance::typed::writer<Schema>` (`typed_writer.hpp`, header-only) takes typed spans and
  declares each column's encoding at compile time.
- **Reader** (`include/nanolance/nano_lance_reader.h`): `nano_lance_table_read_dataset` and its `_ex`,
  `_projected`, `_range` forms, `nano_lance_table_take`, `nano_lance_table_open_stream` / `_range` (an
  `ArrowArrayStream` the caller releases), `nano_lance_table_read_schema`, `nano_lance_table_count_rows`
  (`src/api/nano_lance_table_reader_c.cpp`, over the scan layer); `nano_lance_dataset_read_latest` (the latest
  manifest as plain C structs, `src/api/nano_lance_reader.cpp`); `nano_lance_fetch_external_blob`,
  `nano_lance_external_blob_size` and the S3 block cache (`src/api/nano_lance_external_blob.cpp`); threads and
  work counters (`nano_lance_set_threads`, `nano_lance_work_stats`). Results are freed with
  `nano_lance_table_read_result_free` / `nano_lance_dataset_metadata_free`; on failure the outputs are left
  released or zeroed and must not be freed. Errors are written into the caller's `error_message` buffer.
- **lance-c** (`compat/lance-c/include/lance/lance.h`, `src/api/lance_c.cpp`, the `lance_c` / `LanceC::lance_c`
  targets): lance-c's 127 functions. Include `<nanoarrow/nanoarrow.h>` (or Arrow's C ABI header) before
  `lance/lance.h`. Failures return lance-c's documented error value and set a thread-local
  `lance_last_error_code()` / `lance_last_error_message()` (free the message with `lance_free_string`). A change
  (`lance_dataset_delete`, `_update`, `_merge_insert`, column changes, `_compact_files`) commits a version and
  moves the handle to it; `lance_dataset_restore` returns a new handle. What nanolance does not do fails with
  `LANCE_ERR_NOT_SUPPORTED`, never with a silently different result.
- `writer_internal.hpp` is for nanolance's own operations (a staged writer's field id base, partial-schema
  appends, file events, `writer_take_staged`); other callers do not need it.

```c
NanoLanceWriteOptions options = {0};
NanoLanceWriter w;
if (nano_lance_writer_open(&w, "out.lance", &options) != NANO_LANCE_OK) { /* nano_lance_writer_last_error(&w) */ }
nano_lance_write_batch(&w, &batch, &schema);
nano_lance_writer_commit(&w, false);
nano_lance_writer_close(&w);
```

## Modules

| module | what it holds | tests |
|---|---|---|
| `nano_lance_writer` | the C writer: options, lifecycle, setters, status and last error | `nano_lance_writer_lifecycle`, `nano_lance_writer_api`, `nano_lance_writer_options`, `nano_lance_writer_memory`, `nano_lance_writer_append`, `nano_lance_schema_change_error` |
| `writer` / `writer_cxx` | the C++ `Writer` over the C writer | `nano_lance_writer_api` |
| `typed_writer` | the compile-time typed writer (header-only) | `nano_lance_typed_writer`, `nano_lance_writer_memory` |
| `writer_internal` | staged-writer hooks for the dataset layer's operations | `nano_lance_writer_internal` |
| `nano_lance_reader` | the C reader header; `nano_lance_dataset_read_latest` (manifest as C structs) | `nano_lance_manifest_reader`, `nano_lance_feature_flags` |
| `nano_lance_table_reader_c` | the C table reads, take, streams, schema, count, threads and work stats | `nano_lance_table_stream`, `nano_lance_feature_flags` |
| `nano_lance_external_blob` | external blob fetch and size, the S3 block cache | `nano_lance_external_blob` |
| `lance_c` | lance-c's C API over nanolance | `nano_lance_lance_c_dataset`, `nano_lance_lance_c_search`, `nano_lance_lance_c_stable_row_ids` |

## Spec coverage

The Lance format specification has no API section; the rows follow lance-c's C API at the commit in
`compat/lance-c/UPSTREAM` (`docs/LANCE_C_COMPAT.md`) and nanolance's own C headers. lance-c's own test suite
(`tools/lance_c_suite.py`, `tests/lance_c/expected_pass.txt`: 106 of 106 pass, on fixtures written by pylance and
by nanolance) also covers the lance-c rows, but is not a ctest, so the Tests column does not name it.

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `api.writer.lifecycle` | ref: nanolance nano_lance_writer.h (open / write_batch / commit / finish / close) | done | `src/api/nano_lance_writer.cpp::nano_lance_writer_open`, `src/api/nano_lance_writer.cpp::nano_lance_writer_finish`, `src/api/nano_lance_writer.cpp::nano_lance_writer_close` | `nano_lance_writer_lifecycle`, `nano_lance_writer_api`, `nano_lance_writer_append`, `nano_lance_schema_change_error`, `app_deletion_append_pylance_smoke`, `bindings/python/tests/test_lance_parity.py::test_temporal_and_decimal_readable_by_stock_lance` | unit+interop | Create, append, overwrite, staged fragments published at finish; a schema change within a session and calls out of order are refused. The Python module writes through this API, so pylance reading its datasets is the interop half. |
| `api.writer.options` | ref: nanolance NanoLanceWriteOptions and the set_* calls | done | `src/api/nano_lance_writer.cpp::nano_lance_writer_set_column_encoding`, `src/api/nano_lance_writer.cpp::nano_lance_writer_set_max_pending_bytes`, `src/api/nano_lance_writer.cpp::nano_lance_writer_set_initial_bases` | `nano_lance_writer_options`, `nano_lance_writer_memory`, `app_dict_pylance_interop_smoke`, `app_bss_zstd_pylance_interop_smoke`, `bindings/python/tests/test_multi_base.py::test_write_all_bases_by_nanolance_pylance_reads` | unit+interop | Compression, structural encodings, per-column encodings, the memory budget, borrowed buffers, initial config and schema metadata, stable row ids, base paths and fragment targets, blob pack size, transaction properties. |
| `api.writer.cxx` | ref: nanolance writer.hpp and typed_writer.hpp | done | `src/api/writer_cxx.cpp::Writer`, `include/nanolance/typed_writer.hpp::writer` | `nano_lance_writer_api`, `nano_lance_typed_writer` | unit | The C++ Writer (RAII over the C writer) and the typed writer (compile-time schema and encodings). |
| `api.writer.internal` | ref: nanolance writer_internal.hpp | done | `src/api/nano_lance_writer.cpp::writer_take_staged`, `src/api/nano_lance_writer.cpp::writer_project_append` | `nano_lance_writer_internal`, `nano_lance_write_session` | unit | Field id base, partial-schema append, file events, staged files handed over. |
| `api.reader.manifest` | ref: nanolance nano_lance_reader.h nano_lance_dataset_read_latest | done | `src/api/nano_lance_reader.cpp::nano_lance_dataset_read_latest`, `src/api/nano_lance_reader.cpp::nano_lance_dataset_metadata_free` | `nano_lance_manifest_reader`, `nano_lance_feature_flags` | unit | The latest manifest's schema, fragments and data files as C structs, refused for an unsupported feature flag. |
| `api.reader.table` | ref: nanolance nano_lance_reader.h nano_lance_table_* | done | `src/api/nano_lance_table_reader_c.cpp::nano_lance_table_read_dataset_range`, `src/api/nano_lance_table_reader_c.cpp::nano_lance_table_take`, `src/api/nano_lance_table_reader_c.cpp::nano_lance_table_open_stream` | `nano_lance_table_stream`, `bindings/python/tests/test_row_range.py::test_stock_lance_agrees_with_the_rows_a_range_returns`, `bindings/python/tests/test_take.py::test_take_matches_pylance` | unit+interop | Full, projected and ranged reads, take, streams, schema and count; failures leave outputs released. The Python module reads through these calls, so its comparisons with pylance are the interop half. |
| `api.reader.external_blob` | ref: nanolance nano_lance_reader.h nano_lance_fetch_external_blob | done | `src/api/nano_lance_external_blob.cpp::nano_lance_fetch_external_blob`, `src/api/nano_lance_external_blob.cpp::nano_lance_external_blob_size` | `nano_lance_external_blob` | unit | `file://` always, `s3://` in a build with S3; the S3 block cache is exercised only against an S3 endpoint (`bindings/python/tests/test_object_store.py`). |
| `api.reader.threads` | ref: nanolance nano_lance_reader.h nano_lance_set_threads / nano_lance_work_stats | done | `src/api/nano_lance_table_reader_c.cpp::nano_lance_set_threads`, `src/api/nano_lance_table_reader_c.cpp::nano_lance_work_stats` | `nano_lance_table_stream`, `bindings/python/tests/test_parallel.py` | unit | The process's thread count and the work counters tests assert on. |
| `api.lance_c.dataset` | ref: lance-c lance.h (open, metadata, versions, data statistics, restore) | done | `src/api/lance_c.cpp::lance_dataset_open`, `src/api/lance_c.cpp::lance_dataset_versions`, `src/api/lance_c.cpp::lance_dataset_calculate_data_stats`, `src/api/lance_c.cpp::lance_dataset_restore` | `nano_lance_lance_c_dataset`, `nano_lance_lance_c_stable_row_ids` | unit | Latest or a version, `memory://` URIs, sessions (no shared cache: zero stats). |
| `api.lance_c.write` | ref: lance-c lance.h lance_dataset_write / write_with_params / lance_write_fragments | done | `src/api/lance_c.cpp::lance_dataset_write`, `src/api/lance_c.cpp::lance_dataset_write_with_params` | `nano_lance_lance_c_dataset`, `nano_lance_lance_c_search`, `nano_lance_lance_c_stable_row_ids` | unit | Create / append / overwrite, max rows and bytes per file, stable row ids; a create records lance-c's auto-cleanup policy in the config. |
| `api.lance_c.scanner` | ref: lance-c lance.h lance_scanner_* | done | `src/api/lance_c.cpp::lance_scanner_new`, `src/api/lance_c.cpp::lance_scanner_to_arrow_stream`, `src/api/lance_c.cpp::lance_dataset_take` | `nano_lance_lance_c_dataset`, `nano_lance_lance_c_search` | unit | Projection, SQL filter, offset / limit over the rows that pass, row id / address, include_deleted_rows, fragment ids, blob handling, batch-by-batch and async (always ready). Substrait filters are `LANCE_ERR_NOT_SUPPORTED`. |
| `api.lance_c.changes` | ref: lance-c lance.h delete / update / merge_insert / compact / column changes | done | `src/api/lance_c.cpp::lance_dataset_delete`, `src/api/lance_c.cpp::lance_dataset_merge_insert`, `src/api/lance_c.cpp::lance_dataset_alter_columns` | `nano_lance_lance_c_dataset`, `nano_lance_lance_c_search`, `nano_lance_lance_c_stable_row_ids` | unit | Each one version, the handle moved to it; a stale handle's change gets `LANCE_ERR_COMMIT_CONFLICT`. |
| `api.lance_c.indexes` | ref: lance-c lance.h create / drop / list indexes and index segments | partial | `src/api/lance_c.cpp::lance_dataset_create_scalar_index`, `src/api/lance_c.cpp::lance_dataset_create_vector_index`, `src/api/lance_c.cpp::lance_dataset_commit_index_segments`, `src/api/lance_c.cpp::lance_dataset_drop_index` | `nano_lance_lance_c_search` | unit | Missing: vector kinds IVF_SQ, IVF_HNSW_PQ, IVF_HNSW_FLAT; parameters for BTREE / BITMAP / LABEL_LIST. Drop and list are tested only by lance-c's suite. |
| `api.lance_c.search` | ref: lance-c lance.h lance_scanner_nearest / full_text_search | partial | `src/api/lance_c.cpp::lance_scanner_nearest`, `src/api/lance_c.cpp::lance_scanner_full_text_search`, `src/api/lance_c.cpp::lance_dataset_prepare_fts_query` | `nano_lance_lance_c_search` | unit | Results compared with nanolance's own search (which the Python tests compare with pylance). Missing: multivector search, the Hamming metric, fuzzy FTS, nearest or FTS with `set_fragment_ids`. |
| `api.lance_c.blobs` | ref: lance-c lance.h lance_dataset_take_blobs and lance_blob_file_* | done | `src/api/lance_c.cpp::lance_dataset_take_blobs`, `src/api/lance_c.cpp::lance_blob_file_read` | `bindings/python/tests/test_blob_kinds.py::test_read_blob_ranges_and_pandas` | interop | Every Blob v2 storage kind, only the bytes asked for. The named test drives the same blob reads through the Python module; the lance-c calls themselves are tested only by lance-c's suite (`test_take_blobs`, `test_scanner_blob_handling`). |
| `api.lance_c.object_stores` | ref: lance-c lance.h storage_opts and s3:// URIs | missing | | | - | `s3://` and other object stores return `LANCE_ERR_NOT_SUPPORTED`; `storage_opts` is accepted and ignored (the C writer and reader do support S3). |

## Known gaps

- **lance-c's missing pieces** (`api.lance_c.indexes`, `api.lance_c.search`, `api.lance_c.object_stores`): three
  vector index kinds, multivector and fuzzy search, scalar index parameters, Substrait, and object stores. Object
  stores are the one most callers would notice; the io layer has S3, so it is a matter of passing `storage_opts`
  through to it.
- **Tests that only lance-c's suite runs**: the blob handles, sessions, drop / list index and data statistics of
  lance-c are covered by `tools/lance_c_suite.py`, which needs network access to fetch lance-c's tests and runs in
  the Python CI job, not by a ctest. `nano_lance_lance_c_dataset` now covers the dataset, version, scan, take,
  column-change, delete and restore calls.
- **Module tests the checker cannot see**: `lance_c`, `nano_lance_external_blob`, `nano_lance_table_reader_c` and
  `writer_cxx` have no header named after them (they define what `lance/lance.h`, `nano_lance_reader.h` and
  `writer.hpp` declare), so no test can include them by name. Each is exercised by the tests named in the
  Modules table.
