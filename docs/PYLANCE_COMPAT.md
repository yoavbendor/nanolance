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
| Changes | `delete`, `update` (SQL values), `merge_insert` (`when_matched_update_all`, with or without a condition over `source.*` / `target.*`, `when_not_matched_insert_all`, `when_not_matched_by_source_delete`, `execute`), `add_columns` (SQL expressions, a `pa.field` / schema of null columns, or a reader), `drop_columns`, `alter_columns` (rename, nullability, data type), `optimize.compact_files`, `optimize.optimize_indices`. Each is one version (compaction of indexed
fragments two: see "Keeping indexes up to date"), and writes what pylance writes: deletion files, a schema-only drop, a schema-only null column. |
| Blobs | Blob v2 columns, every storage kind pylance writes (inline, packed, dedicated, external, empty, null): `to_table` returns their descriptions, as pylance does, and `blob_handling="all_binary"` their bytes. `take_blobs` (by `ids`, `addresses` or `indices`) returns `lance.BlobFile` handles (`read`, `readall`, `readinto`, `seek`, `tell`, `size`, `read_range`, `read_ranges`), and `read_blobs` the bytes. A handle reads only the bytes asked for, where they are. |
| Files | `lance.file`: `LanceFileReader` (`read_all`, `read_range`, `take_rows`, `num_rows`, `metadata`, `file_statistics`, `read_global_buffer`), `LanceFileWriter`, `LanceFileSession` (local), `stable_version` |

Datasets and files are written in format 2.2, which is pylance 12's default. A request to write
another version (`data_storage_version="2.0"`, `LanceFileWriter(version="2.1")`) raises. Formats 2.0,
2.1 (the default of earlier pylance releases) and 2.2 are all read:
`test_pylance_written_shapes_read_back` has pylance write every shape of the encoding matrix and the
list tests in 2.1 and 2.2, and nanolance must read back what pylance does. `test_format_v2_0.py`
does the same for 2.0 -- nested structs and lists, dictionaries, FSST/zstd/lz4 strings, blob
columns, packed structs, deletions, ranges and takes.

## Indexes

### Scalar indexes: built and used

`create_scalar_index(column, "BTREE" | "BITMAP" | "LABEL_LIST", name=None, replace=True)` builds
Lance's scalar indexes in Lance's file layout (`src/scalar_index.cpp`), committed in the manifest as
pylance commits one; `drop_index`, `list_indices`, `describe_indices` and `has_index` work as in
pylance. Filtered reads use any BTree, Bitmap or LabelList index the dataset has, whoever built it
(`src/index_search.cpp`):

| index | files | answers |
|---|---|---|
| BTREE | `page_data.lance` (values sorted, nulls first, with row ids), `page_lookup.lance` (each 4096-row page's min, max, null count) | `=`, `<`, `<=`, `>`, `>=`, `!=`, `IN`, `BETWEEN`, `IS [NOT] NULL` |
| BITMAP | `bitmap_page_lookup.lance` (one row bitmap per distinct value, null first) | the same |
| LABEL_LIST | the same over a list column's elements, plus its null lists (global buffer 1) | `array_has_any`, `array_has_all`, `array_contains` / `array_has`, `IS NULL` |

An index narrows a read to the rows it says may pass; the whole filter is then applied to those rows,
so an index changes how much is read, never what comes back. A predicate's AND / OR / NOT structure is
kept (AND intersects, OR unites); fragments an index does not cover are read through. `explain_plan()`
names the index each predicate used, in pylance's words
(`ScalarIndexQuery: query=[x >= 1 && x <= 3]@x_idx(BTree)`); `use_scalar_index=False` reads without.

`test_scalar_index.py` checks it both ways: pylance lists, validates and answers from indexes
nanolance built (its plans show them, its results equal a scan's), their contents equal the indexes
pylance builds from the same data (LabelList keys aside, which Lance writes in hash order), and
nanolance answers every filter from either builder's indexes exactly as a scan does. pylance's own
index tests that need only these index types now pass too (`test_bitmap_index`,
`test_label_list_index`, `test_temporal_index`, `test_use_multi_index`, ...).

Speed, 5M rows in 5 fragments, 4 cores (`docs/BENCHMARKS.md`, "Scalar indexes"): building is
1.5-3.6x faster than pylance (BTree on int64 0.79 s vs 1.16 s, on strings 1.47 s vs 2.19 s; Bitmap
0.39 s vs 1.39 s; LabelList 0.97 s vs 2.26 s), and indexed reads are 1.1-3.2x faster, but for a
one-row lookup returning every column (2.9 ms vs 1.3 ms).

Not built: Lance's other scalar indexes (NGRAM, ZONEMAP, BLOOMFILTER, JSON, RTREE; INVERTED is below),
build options (`fragment_ids`, `train=False`, ...), and indexes on a dataset with stable row ids.
Those a dataset already has are kept, as below.

### Vector indexes: built and searched

`to_table(nearest={...})` (and `scanner(nearest=...)`, `ScannerBuilder.nearest`) searches a vector
column -- a fixed-size list of floats -- as pylance does, and `create_index(column, "IVF_FLAT" |
"IVF_PQ" | "IVF_HNSW_SQ", metric=..., num_partitions=..., num_sub_vectors=..., num_bits=..., m=...,
ef_construction=..., max_level=...)` builds Lance's index for it. The format and the search, step for step, are in `docs/VECTOR_INDEX.md`.

- **Searching** (`src/vector_search.cpp`) an IVF_FLAT or IVF_PQ index -- built by pylance or by
  nanolance -- gives pylance's answer: the same rows, in the same order, with the same `_distance`,
  under L2, cosine and dot, for Lance's default (adaptive) probing, `nprobes` / `minimum_nprobes` /
  `maximum_nprobes`, `refine_factor` and `distance_range`. With 4-bit PQ, which ranks by quantized
  distances, rows at equal distance may come in another order. Around the index it does what pylance
  does: fragments the index does not cover are searched exactly (and the index's candidates
  re-scored), deleted rows and null vectors never come back, `prefilter=True` filters before the
  search and otherwise the k nearest are filtered, `fast_search` searches only what is indexed, and
  a `metric` other than the index's (or `use_index=False`, or no index) searches exactly.
- **Building** (`src/vector_index_build.cpp`) follows Lance's defaults: k-means as Lance trains it
  (random initial centroids, at most 50 iterations, 256 training vectors a partition, hierarchical
  past 256 partitions), partitions from rows / 4096 (IVF_FLAT) or / 8192 (IVF_PQ), PQ codebooks
  trained and codes chosen by L2 on residuals, files and manifest entry as Lance writes them. pylance
  lists such an index, reports its stats, searches it with nanolance's results, and
  `optimize_indices` folds new rows into it.

`test_vector_search.py` checks all of it against pylance. Speed, 200,000 vectors of 128 dimensions,
256 partitions, 4 cores (`docs/BENCHMARKS.md`, "Vector indexes"): searches 2.6-3.7x faster than
pylance (IVF_PQ k=10 2.9 ms vs 7.6 ms; IVF_FLAT 1.5 ms vs 5.3 ms), building 1.3x faster (IVF_PQ
12.7 s vs 16.4 s), at the same recall.

**IVF_HNSW_SQ** (`test_hnsw.py`): pylance's indexes are searched with pylance's answers -- rows,
order and distances for L2 and cosine, with and without deletions, for k, `nprobes`, `refine_factor`,
`ef`, `distance_range` and filters before and after the search; for dot the same, except filtered
searches, where pylance's parallel partition search can itself answer differently from run to run.
Indexes nanolance builds (graphs built as Lance builds them, its node levels and entry points
exactly) are searched by pylance with nanolance's answers, at the recall of pylance's own, and
`optimize_indices` maintains them.

Not supported: IVF_HNSW_PQ, IVF_HNSW_FLAT, IVF_SQ and IVF_RQ indexes (a search falls back to an exact
one, its plan says why), batch and multivector queries, binary (Hamming) vectors, building on float16 / float64
vectors, `lance.indices.IndicesBuilder`, and a vector index on a dataset with stable row ids.

### Full-text indexes: built and searched

`full_text_query=` on `to_table` and `scanner` (and `ScannerBuilder.full_text_search`) searches text
columns as pylance does. `create_scalar_index(column, "INVERTED" | "FTS", ...)` builds Lance's index,
with LanceDB's default analyzer or the settings given. `lance.query` has pylance's query classes:
`MatchQuery`, `MultiMatchQuery`, `BoostQuery`, `BooleanQuery` (and `&` / `|`), `PhraseQuery`.
`docs/FTS_INDEX.md` has the format and the search, step for step.

- **Searching** (`src/fts_search.cpp`) an INVERTED index, built by pylance or by nanolance, gives
  pylance's answer: the same rows with the same `_score`, bit for bit. Lance's order among equal
  scores is arbitrary; nanolance's is by row id. Around the index it does what pylance does:
  - rows in fragments the index does not cover are tokenized on the fly and scored with the
    index's statistics plus theirs;
  - deleted rows never come back;
  - `prefilter=True` filters before the search, and otherwise the best `limit` rows are filtered;
  - `fast_search` searches only what is indexed;
  - a `MatchQuery` on a column without an index is Lance's flat search: the bare simple tokenizer,
    the documents' own statistics.

  `_score` comes after the columns asked for, or where `columns` names it.
- **Building** (`src/fts_index_build.cpp`) supports the simple, whitespace and raw tokenizers with
  English stemming and stop words (or custom ones), lower-casing, ASCII folding and
  `max_token_length`. For the same data, the files hold what pylance's and LanceDB's builds hold,
  byte for byte: vocabulary (an `fst` map, ported), posting blocks, block scores and impact data.
  Two things can differ: the partition number, and document order when Lance's workers take
  fragments out of order. pylance and LanceDB search such an index as their own, and pylance's
  `optimize_indices` merges new rows into it.
- **Phrase queries** (`PhraseQuery`, with `slop`, or a string in double quotes) on indexes with
  positions (`with_position=True`), built by pylance or by nanolance, give pylance's rows and
  scores, unindexed and deleted rows included (`test_fts_phrase.py`, and 1,050 random phrases
  checked against pylance).

`test_fts.py` checks all of it against pylance. Speed, 500,000 documents, 4 cores
(`docs/BENCHMARKS.md`, "Full-text (INVERTED) indexes"):
- building takes 4.1 s against pylance's 7.1 s, for an index of the same size;
- searches are 1.8-8.9x faster: one common word, best 10, 0.31 ms vs 2.74 ms; three words 1.23 ms
  vs 2.65 ms.

Not supported:
- fuzzy matching (`fuzziness` other than 0);
- tokenizers other than simple, whitespace and raw (icu, ngram, code, jieba, lindera), and
  languages other than English;
- full-text search over list columns;
- 256-document posting blocks (index format v3);
- an INVERTED index on a dataset with stable row ids.

### Keeping indexes up to date

`optimize.optimize_indices(num_indices_to_merge=None, index_names=None, retrain=False)` does what
pylance's does (`lance/src/index/append.rs`), for the index kinds nanolance builds:

- the last `num_indices_to_merge` segments of each index (1 by default) are replaced by one covering
  their fragments that still exist and every fragment no segment covers; 0 adds a segment over the
  uncovered fragments alone; `retrain` trains a vector index's model anew over every fragment;
- the new segment keeps the replaced segment's parameters: a scalar index's type; an INVERTED index's
  analyzer and its documents (deleted rows too, which Lance keeps counting until a rebuild, so the
  scores stay pylance's, bit for bit); a vector index's centroids and PQ codebook, new rows assigned
  and encoded with them, then the partitions rebalanced as Lance 12 rebalances them (split those over
  four times the target size, join those under a quarter of it), even when nothing is new;
- every index optimized is committed in one version; an index with nothing to do is left alone; an
  index of a kind nanolance cannot build (IVF_HNSW_PQ, NGRAM, ZONEMAP, ...) fails the call, so name
  the others in `index_names`.

`test_index_optimize.py` optimizes copies of the same dataset with pylance and with nanolance and
checks, with pylance: the same segments and coverage, no unindexed rows, the same filter rows,
bit-identical full-text scores, the same vector distances and partition counts. Compaction uses the
same machinery to give its rewritten rows back to the indexes.

### Indexes nanolance keeps

Every commit nanolance makes
-- append, delete, update, `merge_insert`, column changes, compaction, restore -- writes the
version's indices into the new manifest by Lance's own rules (`src/index_maintenance.cpp`, after
lance-table's `index_maintenance.rs`), so pylance goes on using them:

| nanolance commit | what happens to an index |
|---|---|
| append, delete, update, `merge_insert`, add columns, rename | kept as it is: new fragments are not covered (pylance scans them, and `optimize_indices` adds them), deleted rows are masked |
| drop a column, change its type | an index on that column is dropped |
| compaction | the fragments it rewrites leave every index's coverage (the index points at their rows' old places); then, in a second version, the indexes nanolance can rebuild take them back (`compact_files(reindex=False)` leaves them out) |
| overwrite | every index is dropped |
| restore | that version's indices come back |

A new fragment never takes an id an index covers. `test_index_preservation.py` builds BTree, Bitmap,
full-text and IVF_PQ indexes with pylance, changes the dataset with nanolance, and checks every
indexed query against a plain scan. A dataset whose index section nanolance cannot read stays
readable, but nanolance refuses to commit to it rather than drop its indices.

## What is not implemented

These raise `NotImplementedError` (`nanolance.lance.NotSupportedError`) naming the feature. None of
them is silently ignored:

- `order_by`, Substrait filters, and SQL functions beyond the list above (regular expressions,
  JSON, array functions other than `array_has_any`, `array_has_all` and `array_contains` /
  `array_has`). A function the filter dialect lacks is refused by name.
- The transaction API (`LanceOperation`, `commit`, `write_fragments`), `LanceFragment.merge_columns`
  / `update_columns`, `cleanup_old_versions`. Conflicting writers are refused rather than retried:
  a change built on a version another writer has since replaced fails with "commit conflict".
- Fuzzy full-text queries and the full-text features listed under "Full-text indexes",
  vector indexes other than IVF_FLAT and IVF_PQ, and scalar indexes other than BTree, Bitmap,
  LabelList and INVERTED. An index pylance built is kept, though: see "Indexes" below.
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
| nanolance.lance | **257**, every one of which pylance also passes (254 before optimize_indices and conditional merge_insert, 241 before full-text search, 216 before vector search, 189 before scalar indexes) |

By test file, where nanolance passes any:

| file | pylance | nanolance |
|---|---|---|
| test_dataset.py | 250 | 87 |
| test_scalar_index.py | 189 | 46 |
| test_file.py | 40 | 27 |
| test_map_type.py | 19 | 17 |
| test_column_names.py | 27 | 17 |
| test_lance.py | 23 | 11 |
| test_coerce_query_vector.py | 10 | 10 |
| test_filter.py | 26 | 9 |
| test_fragment.py | 85 | 9 |
| test_json.py | 18 | 5 |
| test_pydantic.py | 12 | 4 |
| test_schema_evolution.py | 23 | 3 |
| test_vector.py | 9 | 3 |
| others | | 9 |

The main reasons tests fail today:

- Most need full-text features nanolance lacks (fuzzy matching, other
  tokenizers, list columns, distributed builds), scalar indexes other than BTree / Bitmap /
  LabelList / INVERTED or their build options (about 145 in `test_scalar_index.py`), vector index kinds other than
  IVF_FLAT / IVF_PQ or `lance.indices.IndicesBuilder` (about 30), namespaces (about 130), object
  stores or the `mem_wal`.
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
