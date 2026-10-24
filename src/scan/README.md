# scan: reading the rows of a dataset's fragments, given a row selection

The scan layer turns a version's manifest into Arrow batches: it plans which fragments and data files a read
touches, decodes their columns through the file and encoding layers, joins a fragment's columns across its data
files, fills columns no file holds with nulls, applies deletion files, data overlays, row-address masks, a filter
and a row range, and adds `_rowid` / `_rowaddr`. It does not parse manifests, deletion files or row id sequences
(the table layer does), evaluate expressions (expr), or know the indexes: a filter's index answer comes in through
the request as a function pointer, which the dataset layer fills in. It uses base, io, arrow, encoding, file,
table and expr.

## Using it

- Everything is declared in `include/nanolance/lance_table_reader.hpp` and defined in
  `src/scan/lance_table_reader.cpp`, except `lance_dataset_scan` and `LanceTableStream::open_request`, which are
  defined by the dataset layer in `src/dataset/dataset_scan.cpp`.
- **`lance_dataset_scan` vs `lance_dataset_scan_core`.** A `LanceScanRequest` carries `index_candidates`, the
  function that answers a filter from the version's scalar indexes. `lance_dataset_scan` (and
  `LanceTableStream::open_request`) set it to the index layer's `index_candidates` (`index_search.hpp`) when the
  caller left it null, so a filtered scan uses BTree, Bitmap and LabelList indexes by default.
  `lance_dataset_scan_core` (and `open_request_core`) consult only the selector the request names: with none, a
  filtered scan reads every row. Call `lance_dataset_scan` in normal code; call the `_core` functions only in a
  build that does not link the index layer (about 1 MB less) or from code below the dataset layer. Either way
  the rows returned are the same: an index only narrows which rows are read, the whole filter is still applied
  to them, and an index that cannot be read is ignored (the fragment is scanned).
- A request (`LanceScanRequest`) names the version (`has_version` / `version`, else the latest), the top-level
  columns (null: all; their children come along), the fragments and their order (null: all, by id), a logical
  row range, an SQL filter, `with_row_id` / `with_row_address`, `include_deleted_rows`, a `RowAddrMask`, and how
  Blob v2 columns come back (`BlobHandling`). With a filter or a mask, the range counts the rows that pass.
- Other entry points: `lance_dataset_take` (logical row numbers), `lance_dataset_take_rows` (row addresses),
  `lance_dataset_schema`, `lance_table_read_schema` and `lance_table_count_rows` (the manifest alone, no data file
  opened), `LanceTableStream` (one fragment per `next()`, for larger-than-memory reads), and `lance_file_read` /
  `lance_file_take` / `lance_file_info` for a standalone `.lance` file.
- Ownership: on success the caller owns `out_schema` and every batch and releases them. On failure `out_schema` is
  left released and `out_batches` empty: do not release them again. A stream's `next()` returns a batch with a
  null `release` at the end.
- Errors are a `false` return with `error` set; nothing throws across these calls. Input is untrusted: sizes,
  paths and counts from the manifest are checked (a data file path may not escape the dataset, a deletion file
  may not name a row past the fragment). `trusted_input` skips only the resource budgets, never a bounds check.
- Thread safety: each call is independent and may run on any thread; fragments are decoded side by side on the
  process's thread pool (`nano_lance_set_threads`). A `LanceTableStream` is used from one thread at a time.

```cpp
nano_lance::LanceScanRequest request;
const std::string filter = "id > 10";
request.filter = &filter;
request.with_row_id = true;
ArrowSchema schema{};
std::vector<ArrowArray> batches;
std::string error;
if (!nano_lance::lance_dataset_scan("data.lance", request, schema, batches, error)) { /* error */ }
```

## Modules

| module | what it holds | tests |
|---|---|---|
| `lance_table_reader` | the read plan (manifest to fragments, files, ranges, index candidates), the per-fragment decode and join across data files, deletions, overlays, masks, filters, row id columns, take, the stream, and standalone file reads | `nano_lance_lance_table_reader` |

## Spec coverage

Against the Lance format specification at https://lance.org/format/ (as of Lance 12.0.0). The rows are the read
rules of the table format: what a reader must do with a manifest's fragments to return the right rows.

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `scan.manifest.feature_flags` | [Table format: Manifest](https://lance.org/format/table/#manifest) | done | `src/table/manifest_reader.cpp::check_manifest_readable`, `src/scan/lance_table_reader.cpp::load_request_manifest` | `nano_lance_feature_flags`, `bindings/python/tests/test_feature_flags.py::test_unknown_reader_bit_refuses_open_and_read`, `bindings/python/tests/test_feature_flags.py::test_known_bits_pylance_writes_still_read_and_write` | unit+interop | Every read loads its manifest through the check: bits 0-8 are understood; any other reader bit (fragment reuse 1<<9, spilled row lineage 1<<11, fragment_tree 1<<12, 1<<13, zone statistics 1<<16) refuses the dataset, as the specification asks of a reader without them. |
| `scan.fragments.plan` | [Table format: Fragments](https://lance.org/format/table/#fragments) | done | `src/scan/lance_table_reader.cpp::open_read_plan_from_manifest` | `nano_lance_lance_table_reader`, `bindings/python/tests/test_lance_parity.py::test_lance_pylance_reader`, `bindings/python/tests/test_row_range.py::test_stock_lance_agrees_with_the_rows_a_range_returns` | unit+interop | Fragments in id order (or the order asked); live rows are `physical_rows - num_deleted_rows`, a count above `physical_rows` is refused; a range skips fragments it does not touch without opening them. |
| `scan.schema.fields` | [Table format: Schema & Fields](https://lance.org/format/table/#schema-fields) | done | `src/scan/lance_table_reader.cpp::build_schema_from_mapping`, `src/scan/lance_table_reader.cpp::lance_fields_arrow_schema` | `bindings/python/tests/test_lance_parity.py::test_reads_nullable_columns_written_by_stock_lance`, `bindings/python/tests/test_lance_read_matrix.py::test_stock_lance_column_reads_back` | interop | The Arrow schema from the manifest's fields: logical types, nullability, field and schema metadata. |
| `scan.data_files.join` | [Table format: Data Evolution](https://lance.org/format/table/#data-evolution) | done | `src/scan/lance_table_reader.cpp::read_data_file_batches` | `nano_lance_dataset_ops`, `bindings/python/tests/test_lance_mutations.py::test_add_columns_puts_a_column_in_a_second_file_of_the_same_fragment` | unit+interop | A fragment's columns across all of its data files, each file a distinct set of field ids; files that disagree on their row count are refused. |
| `scan.data_files.missing_fields` | [Table format: Data Evolution](https://lance.org/format/table/#data-evolution) | done | `src/scan/lance_table_reader.cpp::fields_missing_from`, `src/scan/lance_table_reader.cpp::null_column_values` | `bindings/python/tests/test_partial_schema.py::test_take_and_filter_over_missing_columns`, `bindings/python/tests/test_partial_schema.py::test_append_part_of_the_schema` | interop | A schema field no data file of the fragment holds reads as all NULL (a struct or list as a null value). |
| `scan.data_files.tombstones` | [Table format: Data Evolution](https://lance.org/format/table/#data-evolution) | done | `src/scan/lance_table_reader.cpp::read_data_file_batches` | `nano_lance_dataset_ops`, `bindings/python/tests/test_merge_insert_rewrite_columns.py::test_patches_columns_in_place` | unit+interop | A field id the schema does not have (-2 tombstones, other negative ids, dropped columns) is skipped in that file, not refused; the live copy comes from the file that has it. |
| `scan.data_files.column_indices` | [Table format: Data Files](https://lance.org/format/table/#data-files) | done | `src/scan/lance_table_reader.cpp::v20_leaf_context`, `src/scan/lance_table_reader.cpp::v20_packed_struct_sources` | `bindings/python/tests/test_format_v2_0.py::test_whole_dataset_matches_pylance`, `bindings/python/tests/test_format_v2_0.py::test_packed_struct` | interop | `fields` map to `column_indices` positionally; 2.0 files give every field a column (structs and lists too), 2.1+ only leaves and packed structs. A length mismatch or an index out of range is refused. V1 (legacy) data files are not read. |
| `scan.data_files.mixed_versions` | [Table format: Manifest](https://lance.org/format/table/#manifest) | done | `src/table/manifest_reader.cpp::check_manifest_readable`, `src/scan/lance_table_reader.cpp::read_data_file_batches` | `bindings/python/tests/test_format_v2_0.py::test_append_to_an_older_format` | interop | Each data file is decoded by its own footer's version (bit 1<<8), not the manifest's default. |
| `scan.data_files.metadata_hint` | [Table format: Data Files](https://lance.org/format/table/#data-files) | done | `src/scan/lance_table_reader.cpp::read_data_file_batches` | `bindings/python/tests/test_lance_parity.py::test_lance_pylance_reader` | interop | The footer is always read and is authoritative; `file_metadata_size_bytes` is not used, `file_size_bytes` of 0 is accepted. |
| `scan.base_paths` | [Table format: Manifest](https://lance.org/format/table/#manifest) | done | `src/scan/lance_table_reader.cpp::fragment_data_dirs`, `src/table/base_paths.cpp::data_file_dir`, `src/table/base_paths.cpp::deletion_file_dir` | `nano_lance_base_paths`, `bindings/python/tests/test_multi_base.py::test_initial_bases_all_targets`, `bindings/python/tests/test_multi_base.py::test_deletions_of_rows_in_base_fragments` | unit+interop | Data and deletion files resolve through their `base_id`; files of one fragment may live in different bases. |
| `scan.deletions.arrow` | [Table format: Deletion Files](https://lance.org/format/table/#deletion-files) | done | `src/table/deletion_vector.cpp::parse_arrow_ipc_uint32_column`, `src/scan/lance_table_reader.cpp::read_data_file_batches` | `nano_lance_deletion_vector`, `bindings/python/tests/test_lance_mutations.py::test_sparse_deletions_arrow_format` | unit+interop | ARROW_ARRAY (`.arrow`) deletion files: the offsets are filtered out. |
| `scan.deletions.bitmap` | [Table format: Deletion Files](https://lance.org/format/table/#deletion-files) | done | `src/table/deletion_vector.cpp::parse_roaring_bitmap`, `src/scan/lance_table_reader.cpp::read_data_file_batches` | `nano_lance_deletion_vector`, `bindings/python/tests/test_lance_mutations.py::test_dense_deletions_roaring_bitmap_format` | unit+interop | BITMAP (`.bin`) deletion files, a roaring bitmap. |
| `scan.deletions.order` | [Table format: Deletion Files](https://lance.org/format/table/#deletion-files) | done | `src/scan/lance_table_reader.cpp::read_data_file_batches`, `src/scan/lance_table_reader.cpp::physical_rows_of` | `bindings/python/tests/test_lance_mutations.py::test_deletions_compose_with_a_row_range`, `bindings/python/tests/test_take.py::test_take_across_fragments_and_deletions` | interop | Deletions are applied before a range or a take (both count live rows); a deletion vector is refused when the data files' row count is not `physical_rows`, or it names a row past it. `read_version` is not used on read (the page gives it no read rule). |
| `scan.row_ids.address` | [Row ID & Lineage: Row Addresses](https://lance.org/format/table/row_id_lineage/#row-address) | done | `src/scan/lance_table_reader.cpp::row_address`, `src/scan/lance_table_reader.cpp::add_row_id_columns`, `src/scan/lance_table_reader.cpp::lance_dataset_take_rows` | `nano_lance_lance_table_reader`, `bindings/python/tests/test_system_columns.py::test_take`, `bindings/python/tests/test_take.py::test_take_rows_leaves_out_deleted_addresses` | unit+interop | `_rowaddr` is (fragment id << 32) + offset, deleted rows counted; without stable row ids `_rowid` is the address. |
| `scan.row_ids.stable` | [Row ID & Lineage: Row ID Sequences](https://lance.org/format/table/row_id_lineage/#row-id-sequences) | done | `src/table/row_ids.cpp::fragment_row_ids`, `src/scan/lance_table_reader.cpp::add_row_id_columns` | `nano_lance_row_ids`, `nano_lance_lance_c_stable_row_ids`, `bindings/python/tests/test_stable_row_ids.py::test_nanolance_reads_what_pylance_wrote` | unit+interop | Inline `RowIdSequence`s, every segment kind, in fragment row order; deleted positions keep their place. |
| `scan.row_ids.spilled` | [Row ID & Lineage: Row ID Sequences](https://lance.org/format/table/row_id_lineage/#row-id-sequences) | missing | | | - | Sequences spilled to hidden columns (field ids -3, -4, -5; flag 1<<11) are not read; such a dataset is refused by the feature flag check. |
| `scan.row_ids.lookup` | [Row ID & Lineage: Row ID Index](https://lance.org/format/table/row_id_lineage/#row-id-index) | done | `src/table/row_ids.cpp::RowIdIndex`, `src/table/row_ids.cpp::RowIdToAddress` | `nano_lance_lance_c_stable_row_ids`, `bindings/python/tests/test_stable_row_ids.py::test_take_rows_and_filters` | unit+interop | Stable ids resolve to addresses through an in-memory map of the snapshot's own sequences and deletions; the optional BTree reverse index on `_rowid` is not used. |
| `scan.row_ids.version_columns` | [Row ID & Lineage: Version Tracking](https://lance.org/format/table/row_id_lineage/#row-version-tracking) | partial | `src/table/row_ids.cpp::row_versions_at`, `bindings/python/nanolance/lance/dataset.py::_row_versions` | `bindings/python/tests/test_small_items_2.py::test_c8_delta_inserted_and_updated_columns_equal_pylance`, `bindings/python/tests/test_merge_insert_rewrite_columns.py::test_uncommitted_and_row_versions` | interop | `_row_created_at_version` / `_row_last_updated_at_version` come from the Python module, which asks the table layer per row address; a `LanceScanRequest` cannot ask for them, so the C++ scan and lance-c do not return them. |
| `scan.overlays.resolution` | [Data Overlay Files: Coverage and resolution](https://lance.org/format/table/data_overlay_file/) | partial | `src/scan/lance_table_reader.cpp::load_fragment_overlays`, `src/scan/lance_table_reader.cpp::apply_fragment_overlays` | `nano_lance_overlays`, `bindings/python/tests/test_data_overlay.py::test_dense_sparse_and_newest_wins`, `bindings/python/tests/test_data_overlay.py::test_strings_nulls_deletions_and_take` | unit+interop | Dense and sparse coverage, value at its rank, newest `committed_version` first and the later entry on a tie, a covered NULL overrides, deletions win. Missing: an overlay file is looked for only in `<dataset>/data` (its `base_id` is not resolved), and overlays are applied only to fields a base data file of the fragment holds (a field read as all NULL is not overlaid). |
| `scan.overlays.index_exclusion` | [Data Overlay Files: Index integration](https://lance.org/format/table/data_overlay_file/) | done | `src/table/overlays.cpp::fragments_stale_for_index`, `src/index/index_search.cpp::index_candidates` | `nano_lance_overlays`, `bindings/python/tests/test_data_overlay.py::test_stale_scalar_index_is_not_trusted`, `bindings/python/tests/test_data_overlay.py::test_stale_vector_index_is_not_trusted` | unit+interop | Coarser than the page and with the same rows: a fragment with an overlay of an indexed field committed after the index is not answered by the index at all and is scanned whole (the page excludes only the covered rows and re-evaluates them). |
| `scan.index.candidates` | [Indices](https://lance.org/format/index/) | done | `src/scan/lance_table_reader.cpp::open_read_plan_from_manifest`, `src/scan/lance_table_reader.cpp::read_candidate_rows`, `src/dataset/dataset_scan.cpp::with_indices` | `nano_lance_lance_table_reader`, `nano_lance_index_candidates`, `bindings/python/tests/test_scalar_index.py::test_nanolance_answers_from_either_builders_indexes`, `bindings/python/tests/test_scalar_index.py::test_rows_appended_after_the_index_are_found` | unit+interop | Per covered fragment, only the candidate rows are decoded (or the fragment read through when they are more than an eighth of it) and the whole filter still applies; fragments an index does not cover are read in full. Not used with `include_deleted_rows` or a row-address mask. |
| `scan.blobs` | [Blob v2](https://lance.org/format/table/blob/) | done | `src/scan/lance_table_reader.cpp::init_blob_schema`, `src/scan/lance_table_reader.cpp::append_blob_v2_row` | `nano_lance_external_blob`, `bindings/python/tests/test_blob_kinds.py::test_every_kind_written_and_read_both_ways`, `bindings/python/tests/test_blob_kinds.py::test_legacy_blob_columns_read` | unit+interop | A Blob v2 column as descriptions, bytes, locations or nanolance's ingest shape; inline, packed, dedicated and external storage. |
| `scan.fragments.ignored_fields` | [Table format: Fragments](https://lance.org/format/table/#fragments) | n/a | | | - | `clustering_id` and fragment `metadata` have no read rule ("may be ignored"); the scan ignores them. |

## Known gaps

- **Spilled row lineage** (`scan.row_ids.spilled`): datasets whose row id or version sequences live in hidden
  columns (flag 1<<11), and manifests with `fragment_tree` (1<<12), are refused rather than read. Reading them
  means decoding the -3/-4/-5 columns into the same `RowIdSequence` the inline arm gives, and checking their
  length against `physical_rows`. Lance 12 does not write either by default, so this matters only for datasets
  made with those options.
- **Row version columns in the C++ scan** (`scan.row_ids.version_columns`): only the Python module returns them.
  Adding `with_row_versions` to `LanceScanRequest` would give them to lance-c and C++ callers too.
- **Overlays** (`scan.overlays.resolution`): an overlay file in another base path, and an overlay on a field no
  base file of the fragment holds, are not handled. Overlays are experimental and no Lance release writes either
  case today; both would be a few lines in `load_fragment_overlays`. The index exclusion is per fragment rather
  than per row: correct, with more of the fragment scanned than the page's rule needs.
- V1 (legacy) data files are not read anywhere in nanolance.
