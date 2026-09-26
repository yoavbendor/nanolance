# pylance compatibility (`nanolance.lance`)

`nanolance.lance` is a pylance-compatible Python API over nanolance. Code written against pylance's
`lance` module runs on nanolance by changing its import:

```python
import nanolance.lance as lance

ds = lance.write_dataset(table, "data.lance", max_rows_per_file=1_000_000)
ds.to_table(columns=["image", "label"], limit=64, with_row_id=True)
ds.take([4031, 17, 2980])
lance.dataset("data.lance", version=3).count_rows()
```

Or, leaving the code unchanged, by making `import lance` load nanolance for the process:

```python
import nanolance.lance
nanolance.lance.install_as_lance()   # before anything imports lance

import lance                         # nanolance's, for this process only
```

`install_as_lance()` changes what `import lance` means in the current process. It does not touch a
pylance install. `uninstall_as_lance()` undoes it. No package called `lance` is published: the real
pylance stays what `pip install pylance` gives you. nanolance is an independent implementation of
the Lance format and is not affiliated with the Lance project.

The API tracks pylance **12.0.0** (`nanolance.lance.PYLANCE_API_VERSION`). Where the two can be
compared, results are checked against pylance on the same files, in both directions:
`bindings/python/tests/test_pylance_compat.py`.

## What is implemented

| Area | API |
|---|---|
| Open | `lance.dataset(uri, version=, asof=)`, `LanceDataset(...)`, `local paths`, `file://`, `memory://` |
| Write | `lance.write_dataset(data, uri, schema=, mode="create" / "append" / "overwrite", max_rows_per_file=, max_bytes_per_file=)`. Takes a table, batches, a reader, pandas, polars, dicts, lists of dicts or pydantic models, another dataset. One version per call, as in pylance. `LanceDataset.insert`, `LanceDataset.from_pydantic_model`. |
| Read | `to_table`, `to_batches`, `scanner` (+ `ScannerBuilder`), `head`, `slice`, `take`, `_take_rows` / `take_rows`, `sample`, `count_rows`, `to_pandas`. `columns=` as a list or a `{alias: column}` rename, `limit`, `offset`, `batch_size`, `fragments=`, `with_row_id`, `with_row_address`, and the system columns `_rowid`, `_rowaddr`, `_rowoffset` in a projection. |
| Versions | `version`, `latest_version`, `versions()` (with pylance's summary metadata), `checkout_version`, `checkout_latest`, `restore` |
| Metadata | `schema` (with its schema metadata), `data_storage_version`, `config` / `update_config` / `delete_config_keys`, `metadata` / `update_metadata`, `schema_metadata` / `update_schema_metadata` / `replace_schema_metadata` |
| Fragments | `get_fragments`, `get_fragment`; `LanceFragment`: `fragment_id`, `metadata` (`FragmentMetadata`, `DataFile`, `DeletionFile`), `count_rows`, `physical_rows`, `num_deletions`, `to_table`, `to_batches`, `scanner`, `head`, `take` |
| Filters | `filter=` on `to_table`, `to_batches`, `scanner`, `count_rows` and fragments: an SQL string or a pyarrow compute expression. Comparisons, `AND` / `OR` / `NOT` with SQL's three-valued logic, `IS [NOT] NULL`, `IN`, `BETWEEN`, `LIKE` / `ILIKE`, arithmetic, `CAST`, `DATE` / `TIMESTAMP` literals, struct fields (`s.a`), and the functions `lower`, `upper`, `length`, `abs`, `coalesce`, `starts_with`, `ends_with`, `contains`. With a filter, `offset` and `limit` count the rows that pass, as in pylance. |
| Changes | `delete`, `update` (SQL values), `merge_insert` (`when_matched_update_all` with a condition, `when_not_matched_insert_all`, `when_not_matched_by_source_delete`, `execute`), `add_columns` (SQL expressions, a `pa.field` / schema of null columns, or a reader), `drop_columns`, `alter_columns` (rename, nullability, data type), `optimize.compact_files`. Each is one version, and writes what pylance writes: deletion files, a schema-only drop, a schema-only null column. |
| Blobs | Blob v2 columns, every storage kind pylance writes (inline, packed, dedicated, external, empty, null): `to_table` returns their descriptions, as pylance does, and `blob_handling="all_binary"` their bytes. `take_blobs` (by `ids`, `addresses` or `indices`) returns `lance.BlobFile` handles (`read`, `readall`, `readinto`, `seek`, `tell`, `size`, `read_range`, `read_ranges`), and `read_blobs` the bytes. A handle reads only the bytes asked for, where they are. |
| Files | `lance.file`: `LanceFileReader` (`read_all`, `read_range`, `take_rows`, `num_rows`, `metadata`, `file_statistics`, `read_global_buffer`), `LanceFileWriter`, `LanceFileSession` (local), `stable_version` |

Datasets and files are written in format 2.2, which is pylance 12's default. A request to write
another version (`data_storage_version="2.0"`, `LanceFileWriter(version="2.1")`) raises. Formats 2.1
(the default of earlier pylance releases) and 2.2 are both read:
`test_pylance_written_shapes_read_back` has pylance write every shape of the encoding matrix and the
list tests in each, and nanolance must read back what pylance does. Format 2.0 is not read.

## What is not implemented

These raise `NotImplementedError` (`nanolance.lance.NotSupportedError`) naming the feature. None of
them is silently ignored:

- `order_by`, Substrait filters, and SQL functions beyond the list above (regular expressions,
  JSON, array functions). A function the filter dialect lacks is refused by name.
- The transaction API (`LanceOperation`, `commit`, `write_fragments`), `LanceFragment.merge_columns`
  / `update_columns`, `cleanup_old_versions`. Conflicting writers are refused rather than retried:
  a change built on a version another writer has since replaced fails with "commit conflict".
- Indexes of every kind, vector search (`nearest`), full-text search.
- Tags and branches, stable row ids, multiple base paths, shallow and deep clones.
- Writing Lance's inline, packed and dedicated blob layouts (`lance.blob_field`, `lance.blob_array`):
  nanolance writes external blobs, and reads every kind.
- Object stores and namespaces (`s3://`, `gs://`, REST and directory namespaces).
- torch and Hugging Face integration, UDFs, blob-file APIs, the memtable write-ahead log
  (`mem_wal`).

A name from pylance that nanolance does not implement still imports, for example
`from lance.util import validate_vector_index`. A module that imports many names but uses a few
keeps working. Using such a name raises `NotImplementedError`.

## pylance's own test suite

`tools/pylance_suite.py` fetches pylance's tests from the Lance repository at the tracked tag, with a
sparse git checkout into `.deps/`. Nothing is copied into this repository. It runs them with
nanolance installed as `lance`:

```sh
python tools/pylance_suite.py                 # run; fail if a listed test no longer passes
python tools/pylance_suite.py --update        # rewrite the list of tests that pass
python tools/pylance_suite.py --real-pylance  # the same tests against pylance (baseline)
python tools/pylance_suite.py -k take         # extra arguments go to pytest
```

`bindings/python/tests/pylance_suite/expected_pass.txt` lists the tests that pass. CI runs the suite
on every push that touches the bindings (`.github/workflows/bindings-python.yml`) and fails if a
listed test stops passing, so the list only grows.

Current results (pylance 12.0.0 tests; this machine; `bench/results/pylance_suite.json`):

| | tests passing |
|---|---|
| pylance itself | 1,473 (362 skipped, 14 failing here for environment reasons) |
| nanolance.lance | **189**, every one of which pylance also passes (129 before filters and changes) |

By test file, where nanolance passes any:

| file | pylance | nanolance |
|---|---|---|
| test_dataset.py | 250 | 79 |
| test_file.py | 40 | 27 |
| test_map_type.py | 19 | 17 |
| test_column_names.py | 27 | 16 |
| test_scalar_index.py | 189 | 10 |
| test_filter.py | 26 | 9 |
| test_lance.py | 23 | 9 |
| test_fragment.py | 85 | 5 |
| test_json.py | 18 | 5 |
| test_pydantic.py | 12 | 4 |
| test_schema_evolution.py | 23 | 2 |
| others | | 6 |

The main reasons tests fail today:

- Most need indexes (175 fail on `create_scalar_index` alone), vector or full-text search,
  namespaces (about 130), object stores or the `mem_wal`. These are out of scope.
- About 90 need the transaction API, fragment-level writes, stable row ids or multiple base paths.
- About 25 need filter functions nanolance lacks, or a data storage version other than 2.2.
- A tail of writer gaps in nanolance itself: Arrow dictionary arrays, empty structs, a nullable
  fixed-size list of nullable values, bfloat16. Each is a real gap, listed by the run.

## Bugs the suite found in nanolance itself

The runs found these bugs in nanolance's core. All are fixed and pinned in
`test_pylance_compat.py`:

- **Any schema-level metadata broke writes.** pandas adds such metadata to every table. nanoarrow's
  `ArrowMetadataGetValue` succeeds for a missing key and leaves the value null. nanolance read that
  as "present", so the root looked like an extension type and the write failed with "struct array
  for '' is missing child".
- **Columns of Arrow extension types lost their data.** This covered every extension other than
  Lance's blob, for example `arrow.fixed_shape_tensor` and `arrow.uuid`. The column was mapped as
  having no storage: the file held nothing for it, and nanolance read it back with **zero rows**.
- **Appending dropped deletion files.** The manifest encoder did not write a fragment's deletion
  file, so appending to a dataset with deleted rows brought those rows back.

- **Appends to a dataset with several files per fragment failed.** The column indices of the
  latest fragment's files collide (each file counts from 0), and a dataset whose field ids had gaps
  (a dropped column) never matched a fresh batch's schema.
- **Reading after pylance dropped a column failed.** The dropped column's data stays in its files;
  a full scan decoded it with no schema entry to size it by ("fixed-width column has no value
  width"). Such columns are now skipped.
- **A column pylance added without data could not be read.** `add_columns(pa.field(...))` writes
  only the schema, and every existing fragment reads the column as nulls. nanolance refused the
  scan; it now synthesizes the nulls, and appending writes the column.
- **Racing writers could lose a change.** A change committed as "the version after the latest",
  re-read at commit time, so a change built on version N could land on top of another writer's
  N+1 and silently discard it. And two writers racing for one version shared a temp file name, so
  one could publish the other's manifest and still report success. A change now commits as the
  version after the one it read, the publish refuses to replace an existing version, and the temp
  name is each writer's own (`test_concurrent_commits_lose_nothing`).

- **Reading format 2.1 was refused outright**, and a fixed-size-binary constant wider than 32 bytes
  in a pylance-written 2.2 dataset was refused. The file footer holds the major version and then
  the minor, and nanolance read them swapped. That was invisible while only 2.2 was accepted. A
  wide fixed-width constant is stored as a one-buffer scalar, which nanolance did not read. Both
  were found with the Rust suite below and are pinned by `test_pylance_written_shapes_read_back`.
- **A column of constant pages with different values read as its first page's value.** Lance
  picks each page's layout on its own, and pylance writes this shape too
  (`test_constant_pages_with_different_values`).

- **A Blob v2 column from pylance could not be read,** and a batch with a blob column dropped the
  nulls of every other column. nanolance read only its own blob layout, which has external blobs
  and no nulls. `test_pylance_blobs_read_back` compares descriptions, bytes, handles and
  `read_blobs` with pylance for every storage kind.

The Rust crates' own encoding tests run against nanolance too: see [RUST_SUITE.md](RUST_SUITE.md).

The same work made the manifest codec keep everything pylance writes, so nanolance no longer drops
it: timestamps, writer version, table config and metadata, schema metadata, feature flags, and
fields it does not model.
