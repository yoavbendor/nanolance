# dataset: the operations a user calls on a dataset, each committed as one version

<!-- layer-deps:start -->
Layer 10 of 11 (1 is the bottom, 11 the top). Uses: [io](../io/README.md), [arrow](../arrow/README.md), [encoding](../encoding/README.md), [file](../file/README.md), [table](../table/README.md), [expr](../expr/README.md), [scan](../scan/README.md), [index](../index/README.md). Used by: [api](../api/README.md). See [docs/LAYERS.md](../../docs/LAYERS.md).
<!-- layer-deps:end -->
Example: [examples/layers/dataset.cpp](../../examples/layers/dataset.cpp) (run with ctest -R nano_lance_example_dataset).

The dataset layer is where a user's request becomes a new version: write sessions (create, append, overwrite),
delete, update, merge_insert, adding, altering and dropping columns, compaction, keeping indexes up to date and
committing index segments, restore, config and metadata changes, and the version inspection the pylance- and
lance-c-compatible APIs present. Each operation reads a version, writes the files of its change (through the file
layer and the scan layer's reads), and commits through the table layer, which writes the manifest and the
transaction and decides concurrent-commit conflicts. It also fills in the scan layer's index selector
(`dataset_scan.cpp`). Index builds themselves live in the index layer; cleanup, tags, branches and clones in the
table layer; the C APIs in the api layer. The directory catalog (namespaces) is implemented in the Python
bindings, over these operations.

## Using it

- Entry points, by header (all in `include/nanolance/`):
  - `dataset.hpp`: `dataset_info`, `dataset_versions`, `dataset_latest_version`, `dataset_transaction`,
    `dataset_fields`, `dataset_data_stats`, `dataset_restore`, `dataset_update_config`,
    `dataset_update_table_metadata`, `dataset_update_schema_metadata`, `dataset_update_field_metadata`.
  - `dataset_ops.hpp`: `dataset_delete`, `dataset_update`, `dataset_merge_insert`, `dataset_add_columns_sql` /
    `_nulls` / `_stream`, `dataset_alter_columns`, `dataset_drop_columns`, `dataset_compact_files` (and
    `dataset_compact_plan` / `_execute` / `_commit`), and the uncommitted one-fragment changes
    (`fragment_delete_rows`, `fragment_add_columns_sql`, `fragment_write_columns`).
  - `index_optimize.hpp`: `dataset_optimize_indices`. `index_segments.hpp`: `dataset_build_index_segment`,
    `dataset_merge_index_segments`, `dataset_commit_index_segments`, `dataset_index_segments`,
    `dataset_train_vector_model`, and the staged INVERTED partition files.
  - `dataset_stitcher.hpp`: `stitch_datasets` (merge free-standing nanolance datasets into one, no re-encode).
  - `lance_table_reader.hpp`: `lance_dataset_scan` and `LanceTableStream::open_request` are defined here
    (`dataset_scan.cpp`): the scan layer's reads with the index layer's `index_candidates` filled in.
  - `src/dataset/write_session.hpp` (private): `WriteSession`, what the C writer handle and the operations above
    write with.
- Every change commits exactly one new version (compaction with `reindex` commits two: the rewrite, then the
  indexes) and returns it in `new_version`. A refused change commits nothing.
- `read_version`: pass the version the caller's handle was opened at, as pylance commits from it. A concurrent
  commit since then is decided by Lance's rules (the table layer's `commit_conflicts`): a compatible one is
  rebased, a retryable one re-runs the operation on the latest version (a few attempts, with back-off; then
  "commit conflict"), an incompatible one fails with "Incompatible transaction:". Without `read_version` the
  latest version is read.
- Predicates and values are nanolance's SQL (`nanolance/expr.hpp`). A source `ArrowArrayStream` passed to
  merge_insert or add_columns is consumed whatever the outcome.
- Errors: a `false` return and `error` set (`SegmentErrorKind` says what kind for index segments). Nothing throws.
- Threads: the calls are independent; two processes or threads writing one dataset are serialized by the commit
  (put-if-not-exists of the next manifest), not by a lock.

```cpp
std::uint64_t deleted = 0, version = 0;
std::string error;
if (!nano_lance::dataset_delete("data.lance", "id < 10", deleted, version, error)) { /* error */ }
```

## Modules

| module | what it holds | tests |
|---|---|---|
| `dataset` | version inspection, the commit of a changed manifest with Lance's conflict handling, restore, config and metadata updates | `nano_lance_dataset` |
| `dataset_ops` | delete, update, merge_insert, add / alter / drop columns, compaction, one-fragment uncommitted changes | `nano_lance_dataset_ops` |
| `dataset_scan` | `lance_dataset_scan` / `open_request`: the scan with the index selector filled in | `nano_lance_lance_table_reader`, `nano_lance_index_segments` (no header of its own; see Known gaps) |
| `dataset_stitcher` | stitch many datasets into one by moving their data files and writing one manifest | `nano_lance_stitch` |
| `index_optimize` | optimize_indices: fold appended and compacted fragments into an index | `nano_lance_index_optimize` |
| `index_segments` | build, merge, commit and list an index's segments; train vector models | `nano_lance_index_segments` |
| `write_session` | one dataset write: buffered batches committed as fragments, staged writes for the operations | `nano_lance_write_session` |

## Spec coverage

Against the Lance format specification at https://lance.org/format/ (as of Lance 12.0.0), from the side of the
operation a user calls: [Transactions](https://lance.org/format/table/transaction/) for what each commits (the
table layer's `table.transaction.*` rows hold the operation's encoding and conflict rules), the
[Directory Catalog](https://lance.org/format/catalog/dir/), and pylance 12.0.0's `LanceDataset` for behavior.

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `dataset.write.create` | [Transactions: Overwrite](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/write_session.cpp::WriteSession`, `src/dataset/write_session.hpp::kCommitCreate` | `nano_lance_write_session`, `nano_lance_writer_lifecycle`, `bindings/python/tests/test_pylance_compat.py::test_versions_and_modes_agree` | unit+interop | A new dataset (refused when one exists); config, schema metadata, stable row ids and base paths set at creation. Commits Overwrite (`table.transaction.overwrite`). |
| `dataset.write.append` | [Transactions: Append](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/write_session.cpp::WriteSession`, `src/dataset/write_session.hpp::kCommitAppend` | `nano_lance_writer_append`, `bindings/python/tests/test_lance_mutations.py::test_a_nanolance_append_onto_a_pylance_dataset_still_reads`, `bindings/python/tests/test_partial_schema.py::test_append_part_of_the_schema` | unit+interop | New fragments on the latest version, part of the schema allowed (the rest read as null). `table.transaction.append`. |
| `dataset.write.overwrite` | [Transactions: Overwrite](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/write_session.cpp::WriteSession`, `src/dataset/write_session.hpp::kCommitOverwrite` | `bindings/python/tests/test_lance_mutations.py::test_overwrite_sees_the_new_data_not_the_old`, `bindings/python/tests/test_index_preservation.py::test_overwrite_drops_the_indices` | interop | A version holding only the new fragments; the indexes go. `table.transaction.overwrite`. |
| `dataset.delete` | [Transactions: Delete](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/dataset_ops.cpp::dataset_delete`, `src/dataset/dataset_ops.cpp::apply_deletions` | `nano_lance_dataset_ops`, `bindings/python/tests/test_index_preservation.py::test_delete_keeps_every_index`, `bindings/python/tests/test_commit_conflicts.py::test_delete_rebased_over_pylance_delete_merges_deletion_files` | unit+interop | Deletion files with the fragment's earlier deletions; a fragment left empty is removed. A concurrent Delete on the same fragment is rebased by merging deletion vectors (`merge_deletion_vectors`). `table.transaction.delete`. |
| `dataset.update` | [Transactions: Update](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/dataset_ops.cpp::dataset_update` | `nano_lance_dataset_ops`, `bindings/python/tests/test_index_preservation.py::test_update_and_merge_insert_keep_every_index`, `bindings/python/tests/test_small_items_2.py::test_c9_update_by_rowid_equals_pylance` | unit+interop | Rows rewritten into a new fragment and deleted from their old ones (REWRITE_ROWS), keeping stable row ids and created-at versions; a row mask selects by `_rowid` / `_rowaddr`. `table.transaction.update`. |
| `dataset.merge_insert` | [Transactions: Update](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/dataset_ops.cpp::dataset_merge_insert` | `nano_lance_dataset_ops`, `bindings/python/tests/test_partial_schema.py::test_merge_insert_part_of_the_schema`, `bindings/python/tests/test_merge_insert_rewrite_columns.py::test_patches_columns_in_place`, `bindings/python/tests/test_small_items_2.py::test_b7_merge_insert_delete_counts_equal_pylance` | unit+interop | Every when_matched mode (UpdateIf included), insert, delete by source; rewrite_columns mode (REWRITE_COLUMNS); uncommitted (execute_uncommitted); a source without `on` uses the unenforced primary key. |
| `dataset.add_columns` | [Transactions: Merge](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/dataset_ops.cpp::dataset_add_columns_sql`, `src/dataset/dataset_ops.cpp::dataset_add_columns_nulls`, `src/dataset/dataset_ops.cpp::dataset_add_columns_stream` | `nano_lance_dataset_ops`, `bindings/python/tests/test_lance_mutations.py::test_add_columns_puts_a_column_in_a_second_file_of_the_same_fragment`, `bindings/python/tests/test_index_preservation.py::test_column_changes` | unit+interop | From SQL, all-null, or a stream (no deleted rows allowed): one new data file per fragment beside its others. `table.transaction.merge`. |
| `dataset.alter_columns` | [Transactions: Project](https://lance.org/format/table/transaction/#transaction-types) | partial | `src/dataset/dataset_ops.cpp::dataset_alter_columns` | `nano_lance_dataset_ops`, `bindings/python/tests/test_index_preservation.py::test_column_changes` | unit+interop | Rename and nullability (schema only) and a top-level type change (the column rewritten, its index dropped). Changing the type of a nested field is refused. |
| `dataset.drop_columns` | [Transactions: Project](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/dataset_ops.cpp::dataset_drop_columns` | `nano_lance_dataset_ops`, `bindings/python/tests/test_index_preservation.py::test_column_changes` | unit+interop | Top-level and `struct.child`; the data stays in the files, unreferenced; an index on a dropped column goes. `table.transaction.project`. |
| `dataset.compaction` | [Transactions: Rewrite](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/dataset_ops.cpp::dataset_compact_files`, `src/dataset/dataset_ops.cpp::dataset_compact_plan`, `src/dataset/dataset_ops.cpp::dataset_compact_execute`, `src/dataset/dataset_ops.cpp::dataset_compact_commit` | `nano_lance_dataset_ops`, `bindings/python/tests/test_compaction_plan.py::test_compaction_matches_pylance`, `bindings/python/tests/test_index_preservation.py::test_compaction_reindexes_the_rewritten_rows`, `bindings/python/tests/test_frag_reuse.py` | unit+interop | Lance's planner (target rows, deletion threshold, source budgets, excluded fragments), ReserveFragments then Rewrite, and the indexes remapped by reindexing or by a fragment reuse index (`defer_index_remap`, not with stable row ids). `table.transaction.rewrite`. |
| `dataset.index.create` | [Transactions: CreateIndex](https://lance.org/format/table/transaction/#transaction-types) | partial | `src/index/scalar/scalar_index.cpp::dataset_create_scalar_index`, `src/index/fts/fts_index_build.cpp::dataset_create_inverted_index`, `src/index/vector/vector_index_build.cpp::dataset_create_vector_index` | `nano_lance_index_optimize`, `nano_lance_lance_c_search`, `bindings/python/tests/test_scalar_index.py::test_pylance_lists_and_validates_nanolance_indexes` | unit+interop | Built in the index layer and committed as CreateIndex. BTREE, BITMAP, LABEL_LIST, ZONEMAP, BLOOMFILTER, NGRAM, INVERTED, IVF_FLAT, IVF_PQ, IVF_HNSW_SQ; other vector kinds (IVF_SQ, IVF_HNSW_PQ, IVF_HNSW_FLAT) are refused. `table.transaction.create_index`. |
| `dataset.index.optimize` | [Transactions: CreateIndex](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/index_optimize.cpp::dataset_optimize_indices` | `nano_lance_index_optimize`, `bindings/python/tests/test_index_optimize.py::test_optimize_after_append`, `bindings/python/tests/test_index_optimize.py::test_selection_rules`, `bindings/python/tests/test_index_optimize.py::test_vector_model_kept_or_retrained` | unit+interop | `num_indices_to_merge`, `retrain`, all indexes in one version; an index of a kind nanolance cannot build fails the call. |
| `dataset.index.segments` | [Transactions: CreateIndex](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/index_segments.cpp::dataset_commit_index_segments`, `src/dataset/index_segments.cpp::dataset_build_index_segment`, `src/dataset/index_segments.cpp::dataset_merge_index_segments` | `nano_lance_index_segments`, `nano_lance_lance_c_search`, `bindings/python/tests/test_distributed_index.py::test_scalar_segments_merged_and_committed`, `bindings/python/tests/test_distributed_index.py::test_vector_segments_with_a_shared_model` | unit+interop | Lance 12's commit_existing_index_segments rules: distinct UUIDs, disjoint coverage, one type, replace / keep / refuse existing segments. |
| `dataset.index.drop` | [Transactions: CreateIndex](https://lance.org/format/table/transaction/#transaction-types) | done | `src/index/scalar/scalar_index.cpp::dataset_drop_index` | `bindings/python/tests/test_scalar_index.py::test_names_replace_and_refusals`, `bindings/python/tests/test_ngram_json_index.py::test_drop_index_of_missing_name_raises_runtime_error_as_pylance` | interop | Every segment of the index removed in a CreateIndex with no new indexes; the files stay. |
| `dataset.restore` | [Transactions: Restore](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/dataset.cpp::dataset_restore` | `nano_lance_dataset`, `bindings/python/tests/test_pylance_compat.py::test_restore`, `bindings/python/tests/test_index_preservation.py::test_restore_brings_back_that_versions_indices` | unit+interop | A new version with the old version's schema, fragments and indexes; the stable row id high-water mark is kept. `table.transaction.restore`. |
| `dataset.config` | [Transactions: UpdateConfig](https://lance.org/format/table/transaction/#transaction-types) | done | `src/dataset/dataset.cpp::dataset_update_config`, `src/dataset/dataset.cpp::dataset_update_table_metadata`, `src/dataset/dataset.cpp::dataset_update_schema_metadata`, `src/dataset/dataset.cpp::dataset_update_field_metadata` | `nano_lance_dataset`, `bindings/python/tests/test_pylance_compat.py::test_table_config_and_metadata_survive`, `bindings/python/tests/test_small_apis.py::test_update_field_metadata` | unit+interop | Config upsert / delete, table and schema metadata upsert / replace, field metadata. `table.transaction.update_config`. |
| `dataset.cleanup` | ref: pylance 12.0.0 LanceDataset.cleanup_old_versions | done | `src/table/dataset_refs.cpp::cleanup_old_versions`, `src/table/dataset_refs.cpp::run_auto_cleanup` | `nano_lance_dataset_refs`, `bindings/python/tests/test_version_housekeeping.py::test_cleanup_matches_pylance`, `bindings/python/tests/test_version_housekeeping.py::test_auto_cleanup_on_commit` | unit+interop | Implemented in the table layer; no transaction (versions and unreferenced files removed, tags and branches kept). |
| `dataset.clone` | [Transactions: Clone](https://lance.org/format/table/transaction/#transaction-types) | done | `src/table/dataset_refs.cpp::shallow_clone`, `src/table/dataset_refs.cpp::deep_clone` | `bindings/python/tests/test_branches.py::test_shallow_clone_writes_the_manifest_pylance_writes`, `bindings/python/tests/test_branches.py::test_deep_clone_writes_pylance_layout` | interop | Implemented in the table layer. `table.transaction.clone`. |
| `dataset.hand_built` | [Transactions](https://lance.org/format/table/transaction/#transaction-types) | done | `src/table/dataset_transaction.cpp::dataset_commit_hand_built`, `src/dataset/dataset_ops.cpp::fragment_write_columns`, `src/dataset/dataset_ops.cpp::fragment_delete_rows` | `bindings/python/tests/test_fragment_writes.py::test_flow_matches_pylance`, `bindings/python/tests/test_fragment_writes.py::test_data_replacement_and_rewrite`, `bindings/python/tests/test_data_overlay.py::test_each_engine_reads_the_other_engines_overlays`, `bindings/python/tests/test_multi_base.py::test_add_bases_by_nanolance_pylance_reads` | interop | pylance's `LanceDataset.commit` of a built operation (DataReplacement, DataOverlay, UpdateBases, Merge, ...), with the one-fragment changes that write its files. The operations are the table layer's. |
| `dataset.commit.conflicts` | [Transactions: Conflict resolution](https://lance.org/format/table/transaction/#conflict-resolution) | done | `src/dataset/dataset.cpp::commit_next_version`, `src/dataset/dataset.cpp::merge_deletion_vectors` | `nano_lance_commit_conflicts`, `bindings/python/tests/test_commit_conflicts.py::test_pylance_writer_racing_nanolance_writer`, `bindings/python/tests/test_commit_conflicts.py::test_nanolance_writer_racing_pylance_writer`, `bindings/python/tests/test_concurrent_writers.py::test_processes` | unit+interop | Rebase, retry (the operation re-run on the latest version) or refuse, by the table layer's verdict on each concurrent transaction. |
| `dataset.versions` | ref: pylance 12.0.0 LanceDataset.versions | done | `src/dataset/dataset.cpp::dataset_versions`, `src/dataset/dataset.cpp::dataset_info`, `src/dataset/dataset.cpp::dataset_data_stats` | `nano_lance_dataset`, `bindings/python/tests/test_pylance_compat.py::test_one_version_per_write`, `bindings/python/tests/test_manifest_transactions.py::test_manifest_needs_migration_is_false_for_current_writers` | unit+interop | Versions with timestamps and transaction files, a version's fragments and config, data_stats, manifest_needs_migration. |
| `dataset.stitch` | ref: nanolance stitch_datasets (no Lance counterpart) | done | `src/dataset/dataset_stitcher.cpp::stitch_datasets` | `nano_lance_stitch` | unit | Many nanolance datasets of one schema into one, by moving data files and writing one manifest. |
| `dataset.catalog.dir_listing` | [Directory Catalog: V1 Directory Listing](https://lance.org/format/catalog/dir/) | done | `bindings/python/nanolance/lance/_namespace_dir.py::PyDirectoryNamespace` | `bindings/python/tests/test_namespace.py::test_catalog_reads_the_same_in_both`, `bindings/python/tests/test_namespace.py::test_errors_match_pylance` | interop | Root tables at `<name>.lance`, `.lance-reserved` and `.lance-deregistered` markers. In the Python bindings, not this layer's C++. |
| `dataset.catalog.manifest` | [Directory Catalog: V2 Manifest](https://lance.org/format/catalog/dir/) | partial | `bindings/python/nanolance/lance/_namespace_dir.py::_ManifestNamespace` | `bindings/python/tests/test_namespace.py::test_both_libraries_write_one_catalog`, `bindings/python/tests/test_namespace.py::test_concurrent_writers_from_both_libraries` | interop | The `__manifest` table (object_id, object_type, location, metadata, base_objects), nested namespaces, `<hash>_<object_id>` directories, put-if-not-exists commits. Missing: the manifest table's BTREE / Bitmap / LabelList indexes (each change rewrites it as an Overwrite, which carries none). |
| `dataset.catalog.managed_versions` | [Directory Catalog: Table Version Management](https://lance.org/format/catalog/dir/) | done | `bindings/python/nanolance/lance/_namespace_dir.py::PyDirectoryNamespace` | `bindings/python/tests/test_namespace.py::test_managed_versioning_publishes_through_the_namespace` | interop | `table_version_tracking_enabled`: versions published through create / describe / list / batch-delete table version entries. |
| `dataset.catalog.migration` | [Directory Catalog: Compatibility Mode](https://lance.org/format/catalog/dir/) | partial | `bindings/python/nanolance/lance/_namespace_dir.py::PyDirectoryNamespace` | `bindings/python/tests/test_namespace.py::test_catalog_reads_the_same_in_both` | interop | Directory listing and manifest together (manifest wins) is the default mode and tested; `dir_listing_to_manifest_migration_enabled` is implemented but no test exercises it. |

## Known gaps

- **Alter columns** (`dataset.alter_columns`): a nested field's type cannot be changed. It would mean rewriting
  the struct column with one child cast; Lance allows it, and it is rare.
- **Index kinds** (`dataset.index.create`): IVF_SQ, IVF_HNSW_PQ and IVF_HNSW_FLAT are not built (and an index of a
  kind nanolance cannot build makes `dataset_optimize_indices` and compaction's reindex refuse or leave it with
  fewer fragments). Indexes pylance builds of those kinds are kept and used where nanolance can search them.
- **Directory catalog** (`dataset.catalog.*`): the `__manifest` table has no indexes, so a large catalog is
  scanned; the V1-to-V2 migration switch has no test. The catalog lives in the Python bindings; C and C++ callers
  have none.
- **`dataset_scan` has no test in `tests/dataset/` that the checker sees**: it has no header of its own (it defines
  functions `lance_table_reader.hpp` declares), so no test can include it by name. Every indexed scan in
  `nano_lance_index_segments`, `nano_lance_index_optimize` and `nano_lance_lance_table_reader` goes through it.
