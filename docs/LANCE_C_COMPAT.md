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
declare is defined in `src/lance_c.cpp`. What nanolance implements does the work. The rest fails
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
| Scans | `lance_scanner_new` (projection), `set_limit`, `set_offset`, `set_batch_size`, `with_row_id`, `with_row_address`, `set_fragment_ids`, `set_blob_handling` (argument checks), `set_statistics_callback` (bytes and reads), the tuning setters (accepted, no effect on results), `to_arrow_stream`, `next` + `lance_batch_to_arrow` / `lance_batch_free`, `scan_async` + `async_stream_free`, `poll_next` (always ready: decoding is synchronous) |
| Writes | `lance_dataset_write`, `lance_dataset_write_with_params` (create / append / overwrite, `max_rows_per_file`, `max_bytes_per_file`), `lance_write_fragments` (data files, no manifest). A create records lance-c's auto-cleanup policy in the table config. nanolance does not reclaim versions itself; Lance applies the policy when it next commits. |
| Indexes | `lance_dataset_index_count` (0) and `lance_dataset_index_list_json` (`[]`), since a dataset nanolance opens has no index it can use |

URIs: local paths, `file://`, and `memory://` (a directory private to the process). Object stores
(`s3://`, ...) return `LANCE_ERR_NOT_SUPPORTED`. `storage_opts` is accepted and ignored.

## Not implemented yet

- **Filters**: an SQL filter in `lance_scanner_new`, and `lance_scanner_additional_sql_filter`.
- **Dataset changes**: `lance_dataset_delete`, `update`, `merge_insert`, `compact_files`,
  `drop_columns`, `alter_columns`, `add_columns_*`.

Both are planned next, on a C++ predicate evaluator and change set shared with the pylance-compatible
module.

**Blob v2 files**: `lance_dataset_take_blobs*`, `lance_blob_file_*`, and scanning a Blob v2
dataset that pylance wrote. nanolance writes and reads its own Blob v2 layout but not yet the
packed and dedicated layouts Lance writes.

## Out of scope

Vector and scalar indexes, index segments, vector search (`nearest`), full-text search, and
Substrait filters.

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

**52 of 80 pass: 13 of 20 C tests and 26 of 40 C++ tests, per fixture writer.** Every
implemented group above has a test that passes. The ones that fail today:

| Tests | Why |
|---|---|
| update, merge_insert, alter / drop / add columns, compact, delete (7 per language) | dataset changes, the next step |
| index_lifecycle, index_segment_builder(_progress), vector_models_and_reusable_segments, commit_index_segments | indexes: out of scope |
| scanner_blob_handling, take_blobs | Lance's Blob v2 layouts |

Four C++ tests pass on purpose without the feature: `nearest_smoke`, `fts_smoke`,
`index_segments_smoke` and `multivector_rejects_flat_column`. Upstream wrote them to prove the
wrappers compile, link and report errors, and they accept an error as the outcome.

The same run under AddressSanitizer and UBSan (`build-asan`) reports no error in `liblance_c`.

## Found on the way

The fixtures, a dataset with a non-nullable column appended to, exposed a nanolance reader bug: the
Arrow schema it returned marked every field nullable, whatever the manifest said. Reading a table
and appending it back to the same dataset then failed on a nullability mismatch. The schema now
says what the manifest says.
