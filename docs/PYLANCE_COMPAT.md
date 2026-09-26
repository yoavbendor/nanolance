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
| Files | `lance.file`: `LanceFileReader` (`read_all`, `read_range`, `take_rows`, `num_rows`, `metadata`, `file_statistics`, `read_global_buffer`), `LanceFileWriter`, `LanceFileSession` (local), `stable_version` |

Datasets and files are written in format 2.2, which is pylance 12's default. A request for another
version (`data_storage_version="2.0"`, `LanceFileWriter(version="2.1")`) raises.

## What is not implemented

These raise `NotImplementedError` (`nanolance.lance.NotSupportedError`) naming the feature. None of
them is silently ignored:

- Filters (`filter=`), SQL, `order_by`. These are planned for step 3, the shared C++ predicate
  evaluator.
- Dataset changes other than appends: `delete`, `update`, `merge_insert`, `add_columns` /
  `alter_columns` / `drop_columns`, compaction, the transaction API (`LanceOperation`, `commit`),
  `write_fragments`. Also planned for step 3.
- Indexes of every kind, vector search (`nearest`), full-text search.
- Tags and branches, stable row ids, multiple base paths, shallow and deep clones.
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
| nanolance.lance | **128**, every one of which pylance also passes |

By test file, where nanolance passes any:

| file | pylance | nanolance |
|---|---|---|
| test_dataset.py | 250 | 49 |
| test_file.py | 40 | 27 |
| test_map_type.py | 19 | 12 |
| test_scalar_index.py | 189 | 10 |
| test_lance.py | 23 | 9 |
| test_column_names.py | 27 | 5 |
| test_json.py | 18 | 5 |
| test_pydantic.py | 12 | 4 |
| others | | 7 |

The main reasons tests fail today:

- About 400 need indexes, vector or full-text search, namespaces, object stores or the `mem_wal`.
  These are out of scope.
- About 300 need filters or dataset changes. These are step 3.
- A tail of writer gaps in nanolance itself: Arrow dictionary arrays, empty structs, a nullable
  fixed-size list of nullable values, bfloat16. Each is a real gap, listed by the run.

## Bugs the suite found in nanolance itself

The first runs found three bugs in nanolance's core. All three are fixed and pinned in
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

The same work made the manifest codec keep everything pylance writes, so nanolance no longer drops
it: timestamps, writer version, table config and metadata, schema metadata, feature flags, and
fields it does not model.
