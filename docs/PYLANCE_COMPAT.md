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
| Write | `lance.write_dataset(data, uri, schema=, mode="create" / "append" / "overwrite", max_rows_per_file=, max_bytes_per_file=)`. Takes a table, batches, a reader, pandas, polars, dicts, lists of dicts or pydantic models, another dataset. One version per call, as in pylance. `LanceDataset.insert`, `LanceDataset.from_pydantic_model`. An append may leave out nullable columns: as in Lance, its files hold only the columns given, and the others read as null (a struct or list column as a null value). Unlike pylance, an append whose column types differ from the dataset's is cast rather than refused. |
| Read | `to_table`, `to_batches`, `scanner` (+ `ScannerBuilder`), `head`, `slice`, `take`, `_take_rows` / `take_rows`, `sample`, `count_rows`, `to_pandas`. `columns=` as a list or a `{alias: column}` rename (a nested field `s.x` or `` `meta-data`.`id` `` comes back under the name as written), `limit`, `offset`, `batch_size`, `fragments=`, `with_row_id`, `with_row_address`, `order_by` (column names or `ColumnOrdering`, exact names, per-key direction and null placement; floats in IEEE total order as Lance sorts them; after the filter, before offset and limit; the result is sorted in memory), `batch_size_bytes`, `LANCE_DEFAULT_BATCH_SIZE`, `disable_scoring_autoprojection`, `default_scan_options` (the dataset's schema shows the row id columns they add), and the system columns `_rowid`, `_rowaddr`, `_rowoffset`, `_row_created_at_version`, `_row_last_updated_at_version` in a projection or a `take`, and in a filter (`_rowid` / `_rowaddr`, evaluated over the scanned rows before limit and offset; not combined with vector or full-text search). Without stable row ids both version columns read 1, as in Lance. |
| Versions | `version`, `latest_version`, `versions()` (with pylance's summary metadata), `version_refs()`, `checkout_version` (a number or a tag), `checkout_latest`, `restore`; `tags` (`list`, `list_ordered`, `get_version`, `create`, `update`, `delete`, `replace_metadata`, in Lance's `_refs/tags` files, so pylance and nanolance see each other's); `cleanup_old_versions` / `explain_cleanup_old_versions` with all of pylance's options, by Lance's rules (the read version, newer and tagged versions kept; files no version names kept until 7 days old; only files no newer than the earliest version kept are listed); automatic cleanup from `lance.auto_cleanup.*` after every commit (`auto_cleanup_options`, `optimize.enable_auto_cleanup` / `disable_auto_cleanup`); `LanceDataset.drop` (refused unless the path holds a readable manifest). Branches are not supported. |
| Metadata | `schema` (with its schema metadata), `lance_schema` (`lance.schema.LanceSchema` / `LanceField`: ids, parents, field metadata, keys, `field` / `field_case_insensitive`, `from_pyarrow`, pickling), `data_storage_version`, `config` / `update_config` / `delete_config_keys`, `metadata` / `update_metadata`, `schema_metadata` / `update_schema_metadata` / `replace_schema_metadata`, `update_field_metadata` (by path; set, remove, replace) |
| Statistics | `stats.dataset_stats`, `stats.data_stats` (bytes on disk per field, as Lance sums them), `stats.index_stats` / `index_statistics` (the same numbers as pylance's for BTree, Bitmap, LabelList, INVERTED, IVF_FLAT, IVF_PQ and IVF_HNSW_SQ, on either library's index), `validate` |
| Transactions | `read_transaction`, `get_transactions` (as `Transaction` / `LanceOperation.*`, from the `_transactions/*.txn` file every commit writes; pylance and nanolance read either library's the same way), `transaction_properties=` and `commit_message=` on writes. Hand-built transactions: `LanceDataset.commit` of `Append`, `Overwrite`, `Delete`, `Update`, `Merge`, `Project`, `Rewrite`, `Restore`, `UpdateConfig`, `DataReplacement` and `CreateIndex` operations, or of a `Transaction`; `commit_batch` (Append transactions); `detached=True`; `max_retries`, `commit_timeout`. Each is applied as Lance's `build_manifest` applies it, after Lance's checks, and checked against the transactions committed since its read version by Lance's conflict rules (`lance.commit.CommitConflictError`, `retryable` or not); the transaction file records the operation as given. `MergeInsertBuilder.execute_uncommitted`. |
| Fragments | `get_fragments`, `get_fragment`; `LanceFragment`: `fragment_id`, `metadata` (`FragmentMetadata`, `DataFile`, `DeletionFile`, as pylance shapes them: `to_json` / `from_json`, pickling), `count_rows`, `physical_rows`, `num_deletions`, `to_table`, `to_batches`, `scanner` (`include_deleted_rows`), `head`, `take`, `validate`. Writes apart from any commit, for a commit to publish: `lance.fragment.write_fragments` (fragments or, with `return_transaction`, the transaction), `LanceFragment.create`, `create_from_file`, `DataFile.create`, `delete` / `delete_rows` (a new deletion file), `merge_columns` (SQL expressions, a function of each batch, or data), `merge` (a join), `update_columns` (Lance's tombstoned layout, `with_offsets`). `add_columns` with a function or `lance.batch_udf` (and its checkpoint file). |
| Filters | `filter=` on `to_table`, `to_batches`, `scanner`, `count_rows` and fragments: an SQL string or a pyarrow compute expression. Comparisons, `AND` / `OR` / `NOT` with SQL's three-valued logic, `IS [NOT] NULL`, `IN`, `BETWEEN`, `LIKE` / `ILIKE`, arithmetic, `CAST`, `DATE` / `TIMESTAMP` literals, struct fields (`s.a`), and the functions `lower`, `upper`, `length`, `abs`, `coalesce`, `starts_with`, `ends_with`, `contains`. With a filter, `offset` and `limit` count the rows that pass, as in pylance. |
| Changes | `delete`, `update` (SQL values), `merge_insert` (`when_matched_update_all`, with or without a condition over `source.*` / `target.*`, `when_matched_delete`, `when_matched_fail`, `when_not_matched_insert_all`, `when_not_matched_by_source_delete`, `write_mode`, `execute`; `on` defaults to the schema's unenforced primary key; a source with part of the columns keeps the matched rows' other values and gives inserted rows nulls there, written as whole rows in every `write_mode`), `add_columns` (SQL expressions, a `pa.field` / schema of null columns, or a reader), `merge` (new columns joined on a key; not yet into a dataset with deleted rows), `drop_columns`, `alter_columns` (rename, nullability, data type), `optimize.compact_files`, `optimize.optimize_indices`. Each is one version (compaction of indexed
fragments two: see "Keeping indexes up to date"), and writes what pylance writes: deletion files, a schema-only drop, a schema-only null column. |
| Blobs | Blob v2 columns, read and written, every storage kind: `lance.blob_field` (with `inline_size_threshold`, `dedicated_size_threshold`, `pack_file_size_threshold`), `lance.blob_array`, `Blob`, `BlobType`, `BlobArray`, `BlobColumn`. A blob given as bytes is stored as Lance stores it -- inline in the data file, in a shared packed sidecar or a dedicated sidecar by size, with the same descriptors, blob ids and sidecar files -- and a URI as an external reference (`allow_external_blob_outside_bases=True`, as pylance requires; `external_blob_mode="ingest"` copies the bytes in; `blob_pack_file_size_threshold`). Nulls, empties, whole-object externals, several blob columns, blob columns inside structs and lists (and lists of structs) -- paged as Lance pages them, with the parents' definition and the lists' repetition levels -- and beside any other column (a vector, a list, a struct), appends (either logical shape; a threshold that differs from the dataset's is refused), delete / update / merge_insert / compaction. The write-side checks and their errors are pylance's. Reading: `to_table` returns descriptions, `blob_handling="all_binary"` the bytes; `take_blobs` returns `lance.BlobFile` handles (`read`, `readall`, `readinto`, `seek`, `tell`, `size`, `read_range`, `read_ranges`), `read_blobs` the bytes (both also by a nested field path, `"info.blob"`), `read_blob_ranges` byte ranges per row, and `to_pandas(blob_mode="lazy" / "bytes" / "descriptions")` handles, bytes or descriptions. Legacy (v1) blob columns of formats 2.0 and 2.1 read the same ways; writing one is refused for 2.2, as Lance refuses it. |
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
one, its plan says why), batch and multivector queries, binary (Hamming) vectors, building an index on
float16 / float64 vectors (training a model on them is supported, below), and a vector index on a
dataset with stable row ids.

### Distributed index builds

As pylance 12 builds an index on many machines, and with the same results:

- **`create_index_uncommitted(column, index_type, fragment_ids=..., ...)`** builds one segment over
  those fragments under `_indices/<uuid>/` and returns its `Index` (uuid, fields, dataset version,
  `fragment_ids` as a `lance.bitmap.Bitmap`, `files`, `index_details`) without committing it. BTREE,
  BITMAP, LABEL_LIST, INVERTED (with the analyzer options), IVF_FLAT, IVF_PQ and IVF_HNSW_SQ.
  Vector segments take a shared model, `ivf_centroids` (and for IVF_PQ `pq_codebook`), or train
  their own. Lance's rules and errors: BTREE / LABEL_LIST take no `index_uuid`; scalar types need
  `fragment_ids`; a malformed UUID is "Invalid UUID ...".
- **`merge_existing_index_segments(segments)`** folds a group into one new segment (a new UUID, the
  union of their fragments, the oldest of their versions), with Lance's checks: one keyed field,
  disjoint coverage, one index type, and for vector segments one model -- metric, partition count,
  centroids and PQ codebook equal within 1e-5 ("IVF centroids mismatch across shards"; an
  IVF_HNSW_SQ merge takes the first segment's SQ bounds, as Lance's does). A single vector segment
  is returned as it is. nanolance merges by building the segment over the union of the fragments
  with the first segment's model and settings -- the index Lance's merge produces, not a
  byte-for-byte merge of the segments' files.
- **`commit_existing_index_segments(name, column, segments)`** publishes segments (nanolance's or
  pylance's `Index` objects) as one logical index, with Lance's replacement rules.
- **Legacy INVERTED flow**: `create_scalar_index(..., fragment_ids=[f], index_uuid=u)` per fragment
  stages the parts, `merge_index_metadata(u, "INVERTED", progress_callback=)` builds the index (with
  Lance's three progress stages), and `LanceDataset.commit(uri, LanceOperation.CreateIndex([...],
  []))` commits it (see "Transactions" for the other operations it takes); an `Index` without
  `index_details` gets them from its files, as Lance infers them. BTREE / vector types get Lance's
  "no longer supports merge_index_metadata".
- **`lance.indices.IndicesBuilder`** (pylance's own module, its native calls in
  `lance.lance.indices`): `train_ivf`, `train_pq`, `prepare_global_ivf_pq` on float16, float32 and
  float64 vectors, over chosen fragments; `transform_vectors`, `shuffle_transformed_vectors`,
  `load_shuffled_vectors`. `IvfModel` / `PqModel` save and load as Lance files either library reads.
  Not supported: accelerators, the hamming distance, multivector columns.
- **`centroids()`** / **`get_ivf_model()`** of a vector index.

`tests/test_distributed_index.py`: segments built, merged and committed by nanolance are answered
by pylance as the index it builds in one go (the same rows, scores, and for vector indexes with a
shared model the same rows and distances as pylance's own segments); pylance's segments are merged
and committed by nanolance and nanolance's committed by pylance; models trained by either load in
the other.

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

## DuckDB, Polars, pandas and LanceDB

A `nanolance.lance` dataset is a `pyarrow.dataset.Dataset` (its scanner a `pyarrow.dataset.Scanner`,
its fragments `pyarrow.dataset.Fragment`s), as pylance's is, so the engines that read pylance
datasets read nanolance's the same way, with projections and filters pushed into the scan:

```python
ds = nanolance.lance.dataset("data.lance")
duckdb.sql("SELECT s, count(*) FROM ds WHERE id > 500 GROUP BY s")   # replacement scan
duckdb.from_arrow(ds)                                               # a relation
ds.to_polars()                         # a Polars LazyFrame, as LanceDB's Table.to_polars()
polars.scan_pyarrow_dataset(ds)        # the same
ds.to_pandas()                         # as pylance's (blob columns per blob_mode)
```

Other Lance implementations read the files nanolance writes: DuckDB's `lance` community extension
(scans, filters, `lance_vector_search` and `lance_fts` over the IVF_PQ and INVERTED indexes
nanolance built) and LanceDB (`open_table` on a nanolance dataset: `to_arrow`, `to_polars`,
`to_pandas`, vector and full-text `search`, `where`; nanolance reads and appends to LanceDB's own
tables, and LanceDB to nanolance's). `tests/test_interop.py` checks each against pylance and against
the engine over the same table in memory; `tools/interop_suite.py` runs it in environments with the
DuckDB extension (built for DuckDB 1.5.0, not 1.5.5) and LanceDB installed.

## What is not implemented

These raise `NotImplementedError` (`nanolance.lance.NotSupportedError`) naming the feature. None of
them is silently ignored:

- Substrait filters, `LanceDataset.sql`, and SQL functions beyond those listed (the filter dialect
  now has the `json_*` functions, `regexp_match` / `regexp_like`, `::` / `arrow_cast` casts and
  `array_has_any` / `array_has_all` / `array_contains`). A function it lacks is refused by name.
- `LanceOperation.DataOverlay` (Lance 12's unstable overlay files), `commit_lock`, `update_columns`
  of a blob column, writing the legacy (v1) file format or V1 manifest names. (Racing writers are
  retried as Lance retries them: an append or overwrite is built again on the newer version, a
  change is rebased when the other writers only added fragments, and a delete, update, merge,
  compaction or index optimization whose fragments another writer rewrote runs again on the latest
  version, up to Lance's 20 tries.)
- Fuzzy full-text queries and the full-text features listed under "Full-text indexes",
  vector indexes other than IVF_FLAT and IVF_PQ, and scalar indexes other than BTree, Bitmap,
  LabelList and INVERTED. An index pylance built is kept, though: see "Indexes" below.
- Branches, stable row ids, multiple base paths, shallow and deep clones.
- External blobs under registered base paths, and the prepared-layout blob writers (`PackedBlobWriter`,
  `DedicatedBlobWriter`, `BlobDescriptorArrayBuilder`); blob data through `add_columns` or
  `write_fragments`.
- Object stores and namespaces (`s3://`, `gs://`, REST and directory namespaces).
- torch and Hugging Face integration, blob-file APIs, the memtable write-ahead log
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
| nanolance.lance | **603**, every one of which pylance also passes (483 before distributed index builds, 393 before blobs, 383 before the writer tail, 336 before the small dataset APIs, 316 before JSON columns and filter functions, 312 before compaction planning, 311 before transaction files, 305 before `order_by`, 286 before version housekeeping, 283 before nested paths, 271 before system columns, 259 before writes with part of the schema, 257 before phrase queries, 254 before optimize_indices and conditional merge_insert, 241 before full-text search, 216 before vector search, 189 before scalar indexes) |

By test file, where nanolance passes any:

| file | pylance | nanolance |
|---|---|---|
| test_dataset.py | 250 | 146 |
| test_blob.py | 214 | 87 |
| test_scalar_index.py | 189 | 82 |
| test_bitmap.py | 66 | 55 |
| test_file.py | 40 | 31 |
| test_vector_index.py | 97 | 31 |
| test_column_names.py | 27 | 27 |
| test_filter.py | 26 | 25 |
| test_indices.py | 27 | 23 |
| test_map_type.py | 19 | 17 |
| test_lance.py | 23 | 11 |
| test_json.py | 18 | 10 |
| test_fragment.py | 85 | 10 |
| test_coerce_query_vector.py | 10 | 10 |
| test_optimize.py | 22 | 8 |
| test_pydantic.py | 12 | 6 |
| test_schema.py | 4 | 4 |
| test_schema_evolution.py | 23 | 4 |
| test_vector.py | 9 | 3 |
| others | | 14 |

The main reasons tests fail today:

- Most need full-text features nanolance lacks (fuzzy matching, other
  tokenizers, list columns), scalar indexes other than BTree / Bitmap / LabelList / INVERTED or
  their build options (about 110 in `test_scalar_index.py`), vector index kinds other than
  IVF_FLAT / IVF_PQ / IVF_HNSW_SQ, indexes on float16 / float64 / binary / multivector columns
  (about 60), namespaces (about 130), object stores or the `mem_wal`.
- About 90 need the transaction API, fragment-level writes, stable row ids or multiple base paths.
- About 25 need filter functions nanolance lacks, or a data storage version other than 2.2.
- A tail of writer gaps in nanolance itself: dictionary columns inside structs or lists, null
  elements of a fixed-size list under a list or struct, bfloat16. Each is a real gap, listed by
  the run.

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
- **JSON columns did not cross between pylance and nanolance.** pylance stores a `pa.json_()`
  column as JSONB (logical type "json"); nanolance refused such a dataset ("unsupported on-disk
  logical type: json") and wrote its own JSON columns as plain text, which pylance refused in turn.
  Both now read and write the same bytes (`test_json_columns.py`).
- **A pylance commit failed outright after a nanolance commit.** nanolance wrote no transaction
  files; pylance reads the transaction of every version committed since its read version to
  decide whether its change still applies, and a version without one is a hard error ("Dataset
  version N does not have a transaction file"). Every nanolance commit now writes Lance's
  `_transactions/{read_version}-{uuid}.txn`, describing the change as pylance would
  (`test_transactions.py`).
- **Writers in different processes could overwrite each other's data files.** A data file was
  named `fragment-<n>.lance`, `n` one more than the highest number already in `data/`, so two
  processes writing at once took the same name and the second file replaced the first -- also
  one already committed. Data files are now named at random (128 bits from the system's entropy
  source on every call, as a seeded generator would be copied into forked processes), as Lance
  names them by UUID; deletion file ids take random bits too (`test_concurrent_writers.py`).

- **A dropped column's data files stayed in their fragments.** Lance's Project removes a data file
  left holding none of the schema's fields; nanolance kept it, so the two libraries' fragments of the
  same dataset differed (and a new fragment for it took different field ids). Now removed as Lance
  removes it.
- **A data file written on its own could not be read in a dataset.** A file from pylance's
  `LanceFileWriter` numbers its fields from 0; the manifest maps them to the dataset's ids by
  position, and nanolance looked the dataset's ids up in the file ("data file references unknown
  field id"). It now matches them as Lance does.
- **New SQL columns were typed differently.** `add_columns({"one": "1"})` gave a nullable field where
  Lance's is `not null`, and `value + 1` over an int32 column an int64 one where Lance keeps int32
  (its planner casts a literal to the column's type). Both now match, and the Merge transaction's
  nullability claim with them.
- **Data files are named as Lance names them**: a random UUID's first 3 bytes as 24 binary digits,
  then 26 hex digits (they were `fragment-<hex>.lance`).

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
