# arrow: Arrow arrays and schemas, and how they map to Lance's types

<!-- layer-deps:start -->
Layer 3 of 11 (1 is the bottom, 11 the top). Uses: [base](../base/README.md). Used by: [encoding](../encoding/README.md), [file](../file/README.md), [table](../table/README.md), [expr](../expr/README.md), [scan](../scan/README.md), [index](../index/README.md), [dataset](../dataset/README.md), [api](../api/README.md). See [docs/LAYERS.md](../../docs/LAYERS.md).
<!-- layer-deps:end -->
Example: [examples/layers/arrow.cpp](../../examples/layers/arrow.cpp) (run with ctest -R nano_lance_example_arrow).

This layer is the boundary between Arrow (the C data interface, through nanoarrow) and Lance's schema. It maps
an Arrow schema to Lance fields (logical type, field id, parent id, nullability, metadata) and back, reads a
record batch's columns into `ColumnValues` for the encoders, slices batches without copying, builds Blob v2
arrays, and encodes JSON as Lance's JSONB. How the values are laid out on disk is the encoding layer's, and
where a schema is stored (the manifest) is the table layer's. It uses `base`.

## Using it

- **Schemas** (`include/nanolance/schema_mapper.hpp`): `map_arrow_schema(schema, mapping, error)` turns a
  record-batch schema (a struct) into a `LanceSchemaMapping`: one `LanceField` per field and nested child,
  ids assigned depth-first from 0, roots with `parent_id` -1, and a `column_index` for each field that has
  a column of its own (structs and lists have none; their leaves do). A type Lance cannot store is refused
  with the column's name, never mapped to something else. `lance_schema_mapping_from_manifest` goes the
  other way, and `lance_arrow_format_for_logical_type` names the Arrow format of a Lance logical type.
  `lance_on_disk_logical_type` gives the name written to the manifest (`string`, not the internal `utf8`).
- **Batches** (`include/nanolance/array_accessor.hpp`): `append_batch_column_values(batch, mapping,
  columns, error)` appends one batch's physical columns. Use `resolve_field_array` to read a field's array:
  Arrow does not slice a struct's children with the struct, so a child pointer taken directly reads the wrong
  rows of a sliced batch. The view it gives borrows the batch and must not be released. With
  `borrow_fixed_buffers`, a single-batch fixed-width column points at the caller's buffer, which must then
  stay alive and unchanged until the commit.
- **Column values** (`include/nanolance/column_values.hpp`): what the encoders take. Validity is Arrow's
  (bit set = valid); a nested column's `layers` list its list and struct layers outermost first, and then
  everything else describes the leaf items. Read fixed bytes through `fixed_data()` / `fixed_size()`, never
  `fixed` directly (it is empty when the buffer is borrowed).
- **Slices** (`include/nanolance/arrow_slice.hpp`): `slice_batch` makes zero-copy views of a
  `SharedBatch`; every view keeps the batch alive, so views and their children can be released in any order,
  as the C data interface requires. `slice_batches` trims a list of batches to a row range.
- **Blob v2** (`include/nanolance/blob_builder.hpp`): builds the write-side `lance.blob.v2` struct (inline
  data, or a URI with position and size) from `BlobV2Row`s.
- **JSON** (`include/nanolance/jsonb.hpp`): `jsonb::encode` parses JSON text as Lance's writer does and
  produces the same JSONB bytes; `to_text` prints it back as Lance does; `Item` and `select` walk a value
  for the `json_*` functions without decoding it.
- **Owned C structs** (`src/arrow/arrow_owned.hpp`, private): `OwnedSchema`, `OwnedArray`,
  `OwnedBatches` release what they hold; the builders there make the small arrays the index writers need.

## Modules

| module | what it holds | tests |
|---|---|---|
| `schema_mapper` | Arrow schema <-> Lance fields and logical types | `nano_lance_schema_mapper`, `nano_lance_null_rejection` |
| `array_accessor` | a batch's columns as `ColumnValues`; rebased field views | `nano_lance_array_accessor` |
| `column_values` | the values handed to the encoders | `nano_lance_column_values` |
| `arrow_slice` | zero-copy batch slices with shared ownership | `nano_lance_arrow_slice` |
| `blob_builder` | Blob v2 write-side arrays | `nano_lance_blob_builder` |
| `jsonb` | JSON text <-> JSONB, JSONPath selection | `nano_lance_jsonb` |
| `arrow_owned` | owning wrappers and small array builders | `nano_lance_arrow_owned` |
| `arrow_views` | Utf8View / BinaryView columns copied as string / binary, for the write path | `nano_lance_arrow_views` |

## Spec coverage

Against the Lance format specification at https://lance.org/format/ (as of Lance 12.0.0): the
[schema](https://lance.org/format/table/schema/) page, plus the Arrow C data interface.

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `arrow.types.primitive` | [Primitive Types](https://lance.org/format/table/schema/#primitive-types) | done | `src/arrow/schema_mapper.cpp::parse_format` | `nano_lance_schema_mapper`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_reads_what_nanolance_wrote`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_written_type_reads_back` | unit+interop | `null`, `bool`, `int8`..`int64`, `uint8`..`uint64`. |
| `arrow.types.float` | [Floating Point Types](https://lance.org/format/table/schema/#floating-point-types) | done | `src/arrow/schema_mapper.cpp::parse_format` | `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_reads_what_nanolance_wrote`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_written_type_reads_back` | interop | `halffloat`, `float`, `double`. |
| `arrow.types.string_binary` | [String and Binary Types](https://lance.org/format/table/schema/#string-and-binary-types) | done | `src/arrow/schema_mapper.cpp::parse_format`, `include/nanolance/schema_mapper.hpp::lance_on_disk_logical_type`, `src/arrow/arrow_views.hpp::ViewlessBatch`, `src/dataset/write_session.cpp::WriteSession::write_batch` | `nano_lance_schema_mapper`, `nano_lance_arrow_views`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_reads_what_nanolance_wrote`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_written_type_reads_back`, `bindings/python/tests/test_arrow_views.py::test_nanolance_writes_the_schema_pylance_writes`, `bindings/python/tests/test_arrow_views.py::test_view_data_is_the_plain_string_data` | unit+interop | `string`, `binary`, `large_string`, `large_binary`, and the view types Utf8View and BinaryView, which are written as `string` / `binary` as pylance 12 writes them (the view types are not kept in the schema; the data files are the plain string files for the same values). The conversion runs only for a batch with a view column. |
| `arrow.types.decimal` | [Decimal Types](https://lance.org/format/table/schema/#decimal-types) | done | `src/arrow/schema_mapper.cpp::parse_format` | `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_reads_what_nanolance_wrote`, `bindings/python/tests/test_expr_types.py::test_decimal_filters_equal_pylance` | interop | `decimal:128:p:s` and `decimal:256:p:s`. |
| `arrow.types.temporal` | [Date and Time Types](https://lance.org/format/table/schema/#date-and-time-types), [Timestamp Types](https://lance.org/format/table/schema/#timestamp-types) | done | `src/arrow/schema_mapper.cpp::parse_format` | `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_reads_what_nanolance_wrote`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_written_type_reads_back`, `bindings/python/tests/test_expr_types.py::test_duration_filters_equal_pylance` | interop | Dates, times, durations of every unit; timestamps with a time zone or `-`. |
| `arrow.types.struct` | [Complex Types](https://lance.org/format/table/schema/#complex-types) (struct) | done | `src/arrow/schema_mapper.cpp::map_field`, `src/arrow/array_accessor.cpp::resolve_field_array` | `nano_lance_schema_mapper`, `nano_lance_array_accessor`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_written_type_reads_back`, `bindings/python/tests/test_lance_lists.py::test_null_structs_read_as_null` | unit+interop | Nested and nullable structs; a struct with no fields is a column of its own, as in Lance. |
| `arrow.types.list` | [Complex Types](https://lance.org/format/table/schema/#complex-types) (list, large_list, `.struct`) | done | `src/arrow/schema_mapper.cpp::map_field`, `src/arrow/array_accessor.cpp::nested_leaf_view` | `bindings/python/tests/test_lance_lists.py::test_list_reads_back`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_reads_what_nanolance_wrote` | interop | |
| `arrow.types.fixed_size_list` | [Fixed-Size List Types](https://lance.org/format/table/schema/#fixed-size-list-types), [BFloat16](https://lance.org/format/table/schema/#bfloat16-type) | partial | `src/arrow/schema_mapper.cpp::parse_fixed_size_list` | `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_reads_what_nanolance_wrote`, `bindings/python/tests/test_write_encoding_matrix.py::test_written_encoding_is_readable_by_both`, `bindings/python/tests/test_fsl_in_lists.py::test_round_trip_both_ways`, `bindings/python/tests/test_fsl_read_shapes.py` | interop | Fixed-width elements, including `lance.bfloat16`; a null element inside a valid item is read under a list or struct (W1k1). Written under a list or struct with null elements when a row is under 256 bytes (W1k2: `FixedSizeList{N x Flat, validity}`, two buffers a chunk, pylance's layout, `test_nested_write_shapes.py`); 256 bytes or more is FullZip in pylance and is refused by name for now. `fixed_size_list<bool, N>` is read (W1k3) and written as pylance writes it (`FixedSizeList{N x Flat(1)}`, `test_fsl_bool_write.py`); `fixed_size_list<struct>` is read (W1k4), not written yet. Element validity is written whenever the Arrow child holds a null, as pylance does (pyarrow puts nulls under a null row). Missing: elements of a variable-width type are refused. |
| `arrow.types.fixed_size_binary` | [Fixed-Size Binary Type](https://lance.org/format/table/schema/#fixed-size-binary-type) | done | `src/arrow/schema_mapper.cpp::parse_format` | `nano_lance_schema_mapper`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_reads_what_nanolance_wrote` | unit+interop | |
| `arrow.types.dictionary` | [Dictionary Type](https://lance.org/format/table/schema/#dictionary-type) | partial | `src/arrow/schema_mapper.cpp::map_field` | `nano_lance_schema_mapper`, `bindings/python/tests/test_structural_dict.py::test_stock_lance_reads_a_dictionary_encoded_column`, `bindings/python/tests/test_lance_read_matrix.py::test_fixed_width_dictionary_reads_back` | unit+interop | `dict:<value>:<key>:<ordered>` for a top-level column of string, binary or fixed-width values with integer keys. A dictionary inside a struct or list (W1k2) is written for int32 indices over string or binary values, as pylance writes it (`test_nested_write_shapes.py`); other index types there, and dictionaries of nested values, are refused. |
| `arrow.types.map` | [Map Type](https://lance.org/format/table/schema/#map-type) | done | `src/arrow/schema_mapper.cpp::map_field` | `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_reads_what_nanolance_wrote`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_written_type_reads_back` | interop | `keys_sorted=true` is refused, as Lance refuses it. |
| `arrow.ext.blob_v2` | [Extension Types](https://lance.org/format/table/schema/#extension-types) (`lance.blob.v2`), [Blob identity namespace](https://lance.org/format/table/schema/#blob-identity-namespace) | done | `src/arrow/blob_builder.cpp::build_blob_v2_payload_array`, `src/arrow/schema_mapper.cpp::dematerialize_blob_v2_for_arrow_append` | `nano_lance_blob_builder`, `nano_lance_schema_mapper`, `bindings/python/tests/test_blob_kinds.py::test_every_kind_written_and_read_both_ways`, `bindings/python/tests/test_blob_kinds.py::test_nested_blobs_written_and_read_both_ways` | unit+interop | |
| `arrow.ext.blob_legacy` | [Extension Types](https://lance.org/format/table/schema/#extension-types) (legacy `blob`) | partial | `src/arrow/schema_mapper.cpp::map_field`, `include/nanolance/schema_mapper.hpp::lance_field_is_legacy_blob` | `bindings/python/tests/test_blob_kinds.py::test_legacy_blob_columns_read`, `bindings/python/tests/test_blob_kinds.py::test_legacy_blob_write_refused_as_pylance_refuses` | interop | Read; writing one for format 2.2 is refused, with pylance's message. |
| `arrow.ext.json` | [Extension Types](https://lance.org/format/table/schema/#extension-types) (`json`) | done | `src/arrow/jsonb.cpp::encode`, `src/arrow/jsonb.cpp::to_text`, `src/arrow/schema_mapper.cpp::map_field` | `nano_lance_jsonb`, `bindings/python/tests/test_json_columns.py::test_both_ways` | unit+interop | `lance.json` over large_binary holding JSONB, the same bytes Lance writes; read back as `arrow.json` text. |
| `arrow.ext.other` | [Arrow Extension Metadata](https://lance.org/format/table/schema/#arrow-extension-type-metadata) | done | `src/arrow/schema_mapper.cpp::map_field`, `src/arrow/schema_mapper.cpp::copy_metadata` | `bindings/python/tests/test_pylance_compat.py::test_extension_columns_keep_their_rows` | interop | An extension Lance does not define is stored as its storage type, with its `ARROW:extension:*` metadata. |
| `arrow.fields.ids` | [Field ID Assignment](https://lance.org/format/table/schema/#field-id-assignment) | done | `src/arrow/schema_mapper.cpp::map_field`, `src/arrow/schema_mapper.cpp::reroot_orphaned_fields` | `nano_lance_schema_mapper`, `bindings/python/tests/test_type_support_matrix.py::test_stock_lance_written_type_reads_back` | unit+interop | Depth-first from 0, `parent_id` -1 for roots; an absent `parent_id` reads as 0, as proto3 leaves it off. |
| `arrow.fields.non_reusable_ids` | [Field IDs](https://lance.org/format/table/schema/#field-ids) (`max_allocated_field_id`) | n/a | | | - | Allocating ids for a schema change is the table and dataset layers' (the manifest's high-water mark); see their READMEs. |
| `arrow.fields.nullability` | [Field Message](https://lance.org/format/table/schema/#field-message) (`nullable`) | done | `src/arrow/schema_mapper.cpp::is_nullable` | `nano_lance_null_rejection`, `bindings/python/tests/test_lance_nullable_matrix.py::test_nullable_column_matches_pylance` | unit+interop | The Arrow flag, as pylance writes it. |
| `arrow.fields.metadata` | [Field Metadata](https://lance.org/format/table/schema/#field-metadata), [Encoding Metadata](https://lance.org/format/table/schema/#encoding-metadata) | done | `src/arrow/schema_mapper.cpp::copy_metadata`, `src/arrow/schema_mapper.cpp::is_encoding_metadata_key` | `bindings/python/tests/test_pylance_compat.py::test_schema_metadata_is_written_and_read`, `bindings/python/tests/test_pylance_compat.py::test_encoding_hints_stay_out_of_the_schema` | interop | Field and schema metadata are kept; `lance-encoding:*` hints steer the writer and do not count as a schema change. |
| `arrow.fields.primary_key` | [Primary Key Metadata](https://lance.org/format/table/schema/#primary-key-metadata), [Clustering Key Metadata](https://lance.org/format/table/schema/#clustering-key-metadata) | done | `src/arrow/schema_mapper.cpp::read_key_metadata`, `src/arrow/schema_mapper.cpp::check_primary_keys`, `src/table/manifest_writer.cpp::manifest_field`, `generated/lance_minimal.pb.cpp::to_wire` | `nano_lance_schema_mapper`, `nano_lance_proto_roundtrip`, `bindings/python/tests/test_primary_key.py` | interop | The key is read from the `lance-schema:unenforced-primary-key` metadata as pylance 12 reads it (a position that parses wins; otherwise a truthy flag, without case: true, yes, on, 1, y); the position and clustering position are written to fields 12, 13 and 15. The write checks are pylance's: a nullable key or ancestor, a list ancestor, a key that is not a leaf. Left: a key nested in a struct is not the merge_insert default (top-level keys only). |
| `arrow.cdata.import` | ref: [Arrow C data interface](https://arrow.apache.org/docs/format/CDataInterface.html) (importing arrays, offsets of sliced structs) | done | `src/arrow/array_accessor.cpp::resolve_field_array`, `src/arrow/array_accessor.cpp::append_batch_column_values` | `nano_lance_array_accessor`, `bindings/python/tests/test_bool_bits.py::test_sliced_bools_round_trip`, `bindings/python/tests/test_write_encoding_matrix.py::test_every_shape_survives_sliced_multi_batch_writes` | unit+interop | |
| `arrow.cdata.export` | ref: [Arrow C data interface](https://arrow.apache.org/docs/format/CDataInterface.html) (release callbacks, children released independently) | done | `src/arrow/arrow_slice.cpp::slice_batch`, `src/arrow/arrow_owned.cpp::struct_batch` | `nano_lance_arrow_slice`, `nano_lance_arrow_owned` | unit | |

## Known gaps

Dictionaries are written only
for top-level columns of scalar values, and fixed-size lists only of fixed-width elements; both are refused
by name otherwise, never stored wrongly.
