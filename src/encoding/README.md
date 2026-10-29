# encoding: how one page of one column is laid out, in both directions

<!-- layer-deps:start -->
Layer 4 of 11 (1 is the bottom, 11 the top). Uses: [base](../base/README.md), [arrow](../arrow/README.md). Used by: [file](../file/README.md), [scan](../scan/README.md), [dataset](../dataset/README.md). See [docs/LAYERS.md](../../docs/LAYERS.md).
<!-- layer-deps:end -->
Example: [examples/layers/encoding.cpp](../../examples/layers/encoding.cpp) (run with ctest -R nano_lance_example_encoding).

This layer turns a page's bytes into a column's values and back: the value encodings (bitpacking, run-length,
dictionary, FSST, general zstd / LZ4 compression, byte-stream split), the page layouts around them (mini-block,
full-zip, constant and all-null, the legacy blob layout), repetition and definition levels, and the format 2.0
`ArrayEncoding` tree. It does no file I/O of its own: it reads a column's pages through `EncodingsIo`, which
the file layer implements over a data file, and it does not frame files (footer, column metadata, global
buffers -- the file layer) or assemble Arrow arrays (the arrow and scan layers). It uses base and arrow.

Lance's own encoding crate (`lance-encoding`) separates two things, and so does this directory:

- **value encodings** (`values/`): how a buffer of values is compressed. Lance's `encodings/physical/`.
- **page layouts** (`layout/`): how a page is structured around those buffers -- mini-block chunks with their
  levels, full-zip rows. Lance's `encodings/logical/` and the `PageLayout` messages of `encodings_v2_1.proto`.

## Using it

- **Reading a column** (`nanolance/lance_column_decoder.hpp`): `decode_lance_physical_column` (the whole
  column), `decode_lance_physical_column_range` (rows `[first, first + count)`, only the pages and, for list
  pages with a repetition index, only the chunks that hold them), `decode_lance_physical_column_rows` (strictly
  ascending row numbers; a full-zip page of large values is read row by row). Each takes an `EncodingsIo`, the
  column's on-disk `pb::Field` and its `pb::ColumnMetadata` (both from the file layer's
  `read_lance_data_file_*`), and fills a `ColumnValues` (`nanolance/column_values.hpp`). A format 2.0 column is
  routed to `nano_lance::v20` (`nanolance/lance_v20_decoder.hpp`) by these same calls; a caller only calls the
  `v20::` functions directly for a 2.0 leaf below lists or inside a packed struct, where it must hand over the
  `LeafContext` (the list columns above it).
- **The I/O contract** (`nanolance/encodings_io.hpp`): `EncodingsIo::read(offset, size)` returns exactly those
  bytes or fails with a message, which the decoder passes on; `identity` and `name` key caches. The file
  layer's `DataFileIo` (`nanolance/data_file_reader.hpp`) is the implementation for data files; a test or an
  in-memory store can implement it in a few lines (`tests/encoding/test_encodings_io.cpp`).
- **Writing a page**: the encoders here (`values/*::encode*`, `layout/*`) are called by the file layer's
  writer (`file/data_file_writer.cpp`); the page descriptor a writer puts on each page is built with
  `encoding/lance_descriptors.hpp` (`descriptor::page_encoding(MiniBlock | Constant | FullZip)`, composed
  from `flat`, `variable`, `general`, `rle`, ... -- private, C++23, not installed). What to encode with is
  chosen in `encoding/encoding_choice.hpp`.
- **Reading a descriptor**: `page_layout::decode_page_layout` (`nanolance/page_layout.hpp`) parses a page's
  `/lance.encodings21.PageLayout`; a variant it does not model is kept by field number (`kUnknown`,
  `unknown_layout_field`) so the decoder refuses it by name rather than misreading the page.
- Everything read is untrusted: sizes, counts and widths come from the file, and every decoder refuses
  malformed input with a message (`bool` result plus `std::string& error`) instead of trusting it. Decoded
  sizes are bounded by `read_safety.hpp`'s limits, which are thread-local; `parallel::for_each` carries them
  to worker threads. The functions keep no shared mutable state (scratch is `thread_local`), so columns
  decode in parallel.
- `ColumnValues.rows` is not set by every 2.1 decode path; count rows from `fixed` (fixed width), `offsets`
  (variable width) or `layers` (nested), as the scan layer does.

### A module

One module per encoding, `values/<name>.{hpp,cpp}` or `layout/<name>.{hpp,cpp}`, namespace
`nano_lance::<name>`, holding **both directions** of that encoding and nothing else:

- `encode_*` functions: values (or a page's parts) in, the encoded bytes out, as Lance writes them.
- `decode_*` functions: the encoded bytes in, values out, refusing malformed input with a message (the read
  path's input is untrusted: sizes and counts come from the file).
- No I/O, no schema mapping, no choice of encoding: pure functions on buffers, so a test can run them in
  memory.
- The header says the byte layout in a few lines, and names Lance's source it follows.

### Its test

Each module has a pair test, `tests/encoding/test_<name>.cpp` (ctest `nano_lance_encoding_<name>`, label
`smoke`), that checks encode then decode gives back the input:

- for every value width and the parameter edges the format has (bit widths, run lengths, dictionary sizes,
  block boundaries: 0, 1, a block minus one, a block, more than a block);
- the decoder's refusals: truncated buffers, impossible widths or counts, sizes that do not match;
- where the module only decodes (Lance writes something nanolance does not), hand-built bytes from the
  layout the header describes.

That a page round-trips through nanolance and through pylance (the interop half) stays with the binding
tests (`bindings/python/tests/test_lance_parity.py`, `test_lance_read_matrix.py`,
`test_write_encoding_matrix.py`, `test_writer_tail.py`), which read files each library wrote with the other.

## Modules

| module | what it holds | tests |
|---|---|---|
| `values/bitpack` | FastLanes bitpacking: `encode_block` / `decode_block` for an inline block, `decode_series` for a buffer of blocks, `decode_out_of_line` for a descriptor-width buffer | `nano_lance_encoding_bitpack` |
| `values/general` | zstd / LZ4 general compression and nanolance's `[u64 size][zstd]` frame | `nano_lance_encoding_general` |
| `values/byte_stream_split` | byte-stream split of fixed-width values | `nano_lance_encoding_byte_stream_split` |
| `values/rle` | run-length values and run-length definition levels | `nano_lance_encoding_rle` |
| `values/dictionary` | dictionary blocks of fixed and variable-width values, and their indices | `nano_lance_encoding_dictionary` |
| `values/scalar_value` | a constant page's one value (Lance's scalar value buffer) | `nano_lance_encoding_scalar_value` |
| `layout/miniblock` | mini-block chunk framing (header, levels, buffers, padding) and the metadata words that index the chunks | `nano_lance_encoding_miniblock` |
| `layout/full_zip` | full-zip rows (control word, value slot or length-prefixed value) and the repetition index | `nano_lance_encoding_full_zip` |
| `fsst` | FSST symbol tables: train, compress, the 2312-byte serialized table, decompress (nanom's FSST) | `nano_lance_fsst` |
| `lz4_block` | LZ4 block decompression (Lance's `[u32 size][block]` and bare blocks) | `nano_lance_lz4_block` |
| `repdef` | repetition / definition levels: `serialize` (write) and `unravel` (read) into per-layer offsets and validity | `nano_lance_repdef` |
| `page_layout` | the parse of a page's `/lance.encodings21.PageLayout` descriptor into a typed tree | `nano_lance_page_layout` |
| `lance_descriptors` | the writer's descriptor builders over nanom's model of `encodings_v2_1.proto` | `nano_lance_lance_descriptors` |
| `encodings_io` | the byte-source interface the decoders read through | `nano_lance_encodings_io` |
| `lance_column_decoder` | the format 2.1 / 2.2 page decoders: dispatch on the descriptor, flat and nested columns, ranges and takes | `nano_lance_encodings_io`, `nano_lance_data_file_writer` (file layer) |
| `lance_v20_decoder` | the format 2.0 `ArrayEncoding` decoder | `nano_lance_lance_v20_decoder` |
| `column_slice` | trimming and compacting decoded `ColumnValues` to a row range or a row set | `nano_lance_column_slice` |
| `encoding_choice` | whether bitpacking, RLE, a constant or a dictionary pays for a column | `nano_lance_encoding_choice` |
| `blob_v2_layout` | the Blob v2 descriptor row bytes, and legacy (v1) blob rows as descriptor rows | `nano_lance_blob_v2_layout` |

## Spec coverage

Against the Lance format specification at https://lance.org/format/ (as of Lance 12.0.0). Read and write are
separate rows where nanolance does only one.

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `encoding.layout.miniblock` | [Mini Block Page Layout](https://lance.org/format/file/encoding/#mini-block-page-layout) | done | `src/encoding/layout/miniblock.cpp::read_chunk_header`, `src/encoding/layout/miniblock.cpp::chunk_header`, `src/encoding/lance_column_decoder.cpp::read_page_chunks_with_validity`, `src/file/data_file_writer.cpp::build_nested_page` | `nano_lance_encoding_miniblock`, `bindings/python/tests/test_write_encoding_matrix.py::test_written_encoding_is_readable_by_both`, `bindings/python/tests/test_lance_read_matrix.py::test_stock_lance_column_reads_back`, `bindings/python/tests/test_multichunk_pages.py::test_fixed_width_pages_hold_many_chunks` | unit+interop | Chunk metadata words (buffer 0), chunks padded to 8 bytes (buffer 1), levels sliced into chunks. |
| `encoding.layout.miniblock.large_chunk` | [Mini Block protobuf](https://lance.org/format/file/encoding/#protobuf) | done | `src/encoding/layout/miniblock.hpp::decode_control_word`, `src/encoding/lance_column_decoder.cpp::read_page_chunks_with_validity` | `nano_lance_encoding_miniblock`, `bindings/python/tests/test_pylance_compat.py::test_pylance_written_shapes_read_back`, `bindings/python/tests/test_structural_dict.py::test_stock_lance_reads_a_dictionary_encoded_column` | unit+interop | Reads both the 2.1 u16 and the 2.2 `has_large_chunk` u32 metadata words; writes only the u32 form (Lance 2.2 refuses the u16 form in a 2.2 file). |
| `encoding.layout.miniblock.repetition_index` | [Repetition Index](https://lance.org/format/file/encoding/#buffer-2-or-3-repetition-index-optional) | done | `src/encoding/lance_column_decoder.cpp::take_windows`, `src/file/data_file_writer.cpp::build_nested_page` | `bindings/python/tests/test_take_large_list_pages.py::test_take_from_rusts_large_list_pages`, `bindings/python/tests/test_take_large_list_pages.py::test_take_from_nanolance_list_pages`, `bindings/python/tests/test_lance_list_writes.py::test_long_rows_span_chunks_and_pages` | interop | Depth 1, as the spec supports; written for every list page. |
| `encoding.layout.miniblock.dictionary` | [Dictionary buffer](https://lance.org/format/file/encoding/#buffer-2-dictionary-optional) | done | `src/encoding/values/dictionary.cpp::decode`, `src/encoding/lance_column_decoder.cpp::decode_dictionary_block`, `src/file/data_file_writer.cpp::plan_item_dictionary` | `nano_lance_encoding_dictionary`, `nano_lance_dict_roundtrip`, `bindings/python/tests/test_lance_read_matrix.py::test_fixed_width_dictionary_reads_back`, `bindings/python/tests/test_lance_read_matrix.py::test_bitpacked_dictionary_reads_back`, `bindings/python/tests/test_structural_dict.py::test_structural_dict_roundtrip_and_size` | unit+interop | Reads fixed and variable-width dictionaries, flat, bitpacked or RLE indices, LZ4 / zstd / plain dictionary values. Writes variable-width dictionaries (strings) with zstd or plain values, not Lance's default LZ4. |
| `encoding.layout.full_zip.read` | [Full Zip Page Layout](https://lance.org/format/file/encoding/#full-zip-page-layout) | done | `src/encoding/lance_column_decoder.cpp::full_zip_page_params`, `src/encoding/lance_column_decoder.cpp::decompress_full_zip_value`, `src/encoding/layout/full_zip.hpp::control_bytes` | `nano_lance_encoding_full_zip`, `bindings/python/tests/test_large_values.py::test_reading_rusts_per_value_compressed_pages`, `bindings/python/tests/test_writer_tail.py::test_fixed_size_list_null_elements`, `bindings/python/tests/test_lance_lists.py::test_full_zip_lists_across_pages` | unit+interop | Fixed width (`bits_per_value`) and variable width (`bits_per_offset`), control words with rep/def, per-value General compression. |
| `encoding.layout.full_zip.write` | [Full Zip Page Layout](https://lance.org/format/file/encoding/#full-zip-page-layout) | partial | `src/file/data_file_writer.cpp::write_full_zip_variable_column`, `src/file/data_file_writer.cpp::write_full_zip_fixed_column`, `src/file/data_file_writer.cpp::fixed_column_wants_full_zip`, `src/file/data_file_writer.cpp::write_nested_blob_column`, `src/encoding/layout/full_zip.cpp::append_level` | `nano_lance_encoding_full_zip`, `nano_lance_data_file_writer`, `bindings/python/tests/test_fullzip_fixed.py`, `bindings/python/tests/test_large_values.py::test_large_values_round_trip`, `bindings/python/tests/test_blob_kinds.py::test_nested_blobs_written_and_read_both_ways` | unit+interop | Written for variable-width values averaging 4 KiB or more, for Blob v2 descriptors under lists or structs, and (W1i) for fixed-width values of 256 bytes or more: a fixed_size_list of a flat fixed-width child (with or without element validity, nulls in the rows as def_bits=1) and fixed_size_binary, as `FullZip{values=FixedSizeList{NxFlat(b)}}` / `FullZip{values=Flat(b)}`, the layout text equal to pylance's for 21 measured cases. Not yet under a list or struct (W1k2 stopped there): FullZip for a fixed_size_list row of 256 bytes or more, which needs a nested FullZip writer (pylance writes `list<fsl256>` as FullZip even without null elements; nanolance writes MiniBlock). Not yet: the page split (pylance cut a fixed-width column at its input batches, measured; nanolance cuts at 8 MiB of rows), variable-width values under `structural-encoding=fullzip`, a bool column or list of lists under fullzip, and per-value compression of a fixed-width FullZip page. |
| `encoding.layout.full_zip.repetition_index` | [Full-zip repetition index](https://lance.org/format/file/encoding/#repetition-index-buffer-1) | done | `src/encoding/layout/full_zip.cpp::repetition_index`, `src/encoding/layout/full_zip.cpp::repetition_index_entry`, `src/encoding/lance_column_decoder.cpp::take_full_zip_rows` | `nano_lance_encoding_full_zip`, `bindings/python/tests/test_take.py::test_take_images`, `bindings/python/tests/test_take.py::test_take_matches_pylance` | unit+interop | 1, 2, 4 or 8-byte entries read; takes read only the requested rows. |
| `encoding.layout.constant` | [Constant Page Layout](https://lance.org/format/file/encoding/#constant-page-layout) | done | `src/encoding/lance_column_decoder.cpp::apply_constant_definition_levels`, `src/encoding/lance_column_decoder.cpp::constant_def_buffer_index`, `src/encoding/values/scalar_value.cpp::decode_variable`, `src/file/data_file_writer.cpp::constant_layout_message` | `nano_lance_encoding_scalar_value`, `nano_lance_constant_roundtrip`, `nano_lance_encodings_io`, `bindings/python/tests/test_lance_constant_nulls.py::test_constant_column_with_nulls_matches_pylance`, `bindings/python/tests/test_large_constant_lists.py::test_constant_list_pages_read_as_pylance` | unit+interop | Inline value (at most 32 bytes) or a value buffer, with rep/def level buffers. |
| `encoding.layout.all_null` | [Constant Page Layout (all null)](https://lance.org/format/file/encoding/#constant-page-layout) | done | `src/file/data_file_writer.cpp::all_null_constant_layout_message`, `src/encoding/lance_column_decoder.cpp::apply_constant_definition_levels` | `bindings/python/tests/test_write_encoding_matrix.py::test_one_dataset_holding_every_shape_at_once`, `bindings/python/tests/test_lance_lists.py::test_list_reads_back`, `bindings/python/tests/test_blob_kinds.py::test_all_null_blob_column_added_later` | interop | A ConstantLayout with no inline value. |
| `encoding.layout.blob` | [Blob Page Layout](https://lance.org/format/file/encoding/#blob-page-layout) | done | `src/encoding/lance_column_decoder.cpp::legacy_blob_inner_shape`, `src/encoding/lance_column_decoder.cpp::decode_legacy_blob_column`, `src/encoding/blob_v2_layout.cpp::blob_append_legacy_row` | `nano_lance_blob_v2_layout`, `bindings/python/tests/test_blob_kinds.py::test_legacy_blob_columns_read`, `bindings/python/tests/test_blob_kinds.py::test_legacy_blob_write_refused_as_pylance_refuses` | unit+interop | Read only (position, size descriptions; zero size with a position is null). Writing one is refused as pylance refuses it; Blob v2 is written instead. |
| `encoding.layout.planar` | [Planar Page Layout](https://lance.org/format/file/encoding/#planar-page-layout) | missing | | | - | Lance 2.3 (unstable): dense / sparse structure planes and chunked / zipped / indexed / constant / none value planes. A page with it is refused by layout field number. |
| `encoding.repdef.definition` | [Definition Levels](https://lance.org/format/file/encoding/#definition-levels) | done | `src/encoding/repdef.cpp::unravel`, `src/encoding/repdef.cpp::serialize`, `src/file/data_file_writer.cpp::pack_definition_levels` | `nano_lance_repdef`, `bindings/python/tests/test_lance_nullable_matrix.py::test_nullable_column_matches_pylance`, `bindings/python/tests/test_lance_lists.py::test_null_structs_read_as_null`, `bindings/python/tests/test_lance_parity.py::test_null_struct_round_trips` | unit+interop | Nullable items, nullable structs at any depth, bitpacked or RLE level buffers. |
| `encoding.repdef.repetition` | [Repetition Levels](https://lance.org/format/file/encoding/#repetition-levels) | done | `src/encoding/repdef.cpp::unravel`, `src/encoding/repdef.cpp::serialize` | `nano_lance_repdef`, `bindings/python/tests/test_lance_lists.py::test_list_reads_back`, `bindings/python/tests/test_lance_list_writes.py::test_nested_column_round_trips` | unit+interop | Lists, large lists, maps, empty and null lists, lists of structs and structs of lists. |
| `encoding.semi_structural.dictionary` | [Dictionary Encoding](https://lance.org/format/file/encoding/#dictionary-encoding) | done | `src/encoding/encoding_choice.cpp::variable_column_dict_beneficial`, `src/encoding/lance_column_decoder.cpp::lance_column_dictionary_entries` | `nano_lance_encoding_choice`, `bindings/python/tests/test_writer_tail.py::test_dictionary_columns`, `bindings/python/tests/test_lance_parity.py::test_dictionary_column_round_trips` | unit+interop | Applied before structural encoding; Arrow dictionary columns keep their dictionary on read. |
| `encoding.dictionary.fixed_width` (W1p) | [Dictionary Encoding](https://lance.org/format/file/encoding/#dictionary-encoding) | partial | `src/encoding/encoding_choice.cpp::fixed_column_dict_beneficial`, `src/file/data_file_writer.cpp::write_dict_fixed_column`, `src/encoding/values/bitpack.cpp::encode_out_of_line` | `nano_lance_encoding_choice`, `nano_lance_encoding_bitpack`, `bindings/python/tests/test_dict_64bit.py` | unit+interop | int64, uint64, timestamp and date64 columns without nulls; the indices are RLE'd (`Rle{Flat(32),Flat(8)}`, W1p2) when they have runs, `rle_runs_beneficial`; the 50,000-row cutoff for sorted columns is not pylance's yet (SPEC_GAPS); no `lance-encoding:compression` (dictionary values as Lance's rule: InlineBitpacking / OutOfLineBitpacking / General{LZ4,Flat}). Float64, 128-bit and nullable columns: not yet (W1p3). |
| `encoding.semi_structural.packed_struct` | [Struct Packing](https://lance.org/format/file/encoding/#struct-packing) | missing | | | - | `PackedStruct` (CompressiveEncoding 12) of fixed-width children, opt-in with `lance-encoding:packed`, is neither read nor written in 2.1 / 2.2 files (the 2.0 one is read, `encoding.v2_0`). |
| `encoding.semi_structural.variable_packed_struct` | [Struct Packing](https://lance.org/format/file/encoding/#struct-packing) | partial | `src/encoding/lance_column_decoder.cpp::decode_nested_blob_column`, `src/file/blob_v2_external.cpp::blob_v2_descriptor_page_encoding` | `bindings/python/tests/test_blob_writers.py::test_descriptor_field_is_the_blob_v2_struct`, `bindings/python/tests/test_pylance_compat.py::test_pylance_blobs_read_back` | interop | Only as the Blob v2 descriptor struct (kind, position, size, blob_id, blob_uri); a user struct packed this way is not read or written. |
| `encoding.semi_structural.fixed_size_list` | [Fixed Size List](https://lance.org/format/file/encoding/#fixed-size-list) | done | `src/encoding/page_layout.cpp::decode_page_layout`, `src/encoding/lance_column_decoder.cpp::classify_from_descriptor`, `src/file/data_file_writer.cpp::item_value_encoding` | `nano_lance_lance_descriptors`, `bindings/python/tests/test_writer_tail.py::test_fixed_size_list_null_elements`, `bindings/python/tests/test_fsl_in_lists.py::test_round_trip_both_ways`, `bindings/python/tests/test_write_encoding_matrix.py::test_written_encoding_is_readable_by_both` | unit+interop | `FixedSizeList{items_per_value, has_validity}`; nullable children's validity as a separate buffer. |
| `encoding.compression.flat` | [Flat](https://lance.org/format/file/encoding/#flat) | done | `src/encoding/lance_column_decoder.cpp::append_flat_values`, `src/encoding/lance_descriptors.hpp::flat` | `nano_lance_lance_descriptors`, `nano_lance_bool_roundtrip`, `bindings/python/tests/test_write_encoding_matrix.py::test_written_encoding_is_readable_by_both`, `bindings/python/tests/test_bool_bits.py::test_rust_written_bools` | unit+interop | Including 1-bit booleans. |
| `encoding.compression.variable` | [Variable](https://lance.org/format/file/encoding/#variable) | done | `src/encoding/lance_column_decoder.cpp::decode_variable_width_page`, `src/file/data_file_writer.cpp::build_variable_chunks` | `nano_lance_compression_roundtrip`, `bindings/python/tests/test_lance_parity.py::test_large_offset_types_round_trip`, `bindings/python/tests/test_lance_parity.py::test_large_offset_types_written_by_stock_lance` | unit+interop | 32 and 64-bit offsets, interleaved chunk (offsets then bytes). The 2.3 generic offset codec is not read. |
| `encoding.compression.constant` | [Constant](https://lance.org/format/file/encoding/#constant) | partial | `src/encoding/page_layout.cpp::convert_constant`, `src/encoding/lance_column_decoder.cpp::full_zip_page_params` | `nano_lance_lance_descriptors`, `bindings/python/tests/test_fsl_all_null_items.py` | unit+interop | Field 3 is read, and decoded for the FullZip FixedSizeList item shape pylance writes for an all-null list. Read under a list (W1k1): list<fsl> with null elements, and all-null list<fsl>. Still refused: fsl<fsl>, fsl<string> (strict xfail in the test); small struct<fsl> with null elements now reads. A block / mini-block Constant was not seen in any pylance shape tried. Constant pages (`encoding.layout.constant`) are done. |
| `encoding.compression.inline_bitpacking` | [Bitpacking](https://lance.org/format/file/encoding/#bitpacking) | done | `src/encoding/values/bitpack.cpp::decode_block`, `src/encoding/values/bitpack.cpp::encode_block`, `src/file/data_file_writer.cpp::page_layout_bytes_inline_bitpacking` | `nano_lance_encoding_bitpack`, `nano_lance_bitpacking_roundtrip`, `bindings/python/tests/test_rust_mixed_pages.py::test_bitpacked_pages_then_a_flat_one`, `bindings/python/tests/test_write_encoding_choice.py::test_stock_lance_still_reads_both_choices` | unit+interop | FastLanes 1024-value blocks with the width inline. |
| `encoding.compression.out_of_line_bitpacking` | [Bitpacking](https://lance.org/format/file/encoding/#bitpacking) | done | `src/encoding/values/bitpack.cpp::decode_out_of_line`, `src/encoding/lance_column_decoder.cpp::decode_levels`, `src/file/data_file_writer.cpp::levels_encoding` | `nano_lance_encoding_bitpack`, `bindings/python/tests/test_lance_nullable_matrix.py::test_definition_levels_spanning_several_fastlanes_blocks`, `bindings/python/tests/test_lance_parity.py::test_reads_nullable_columns_written_by_stock_lance` | unit+interop | For levels and dictionary indices. |
| `encoding.compression.fsst` | [Fsst](https://lance.org/format/file/encoding/#fsst) | done | `src/encoding/fsst.cpp::parse_symbol_table`, `src/encoding/lance_column_decoder.cpp::expand_fsst_values`, `src/file/data_file_writer.cpp::plan_item_fsst` | `nano_lance_fsst`, `bindings/python/tests/test_fsst_writes.py::test_fsst_pages_round_trip_and_match_pylance_size`, `bindings/python/tests/test_lance_parity.py::test_reads_fsst_string_columns_written_by_stock_lance` | unit+interop | One symbol table per page, in the descriptor. |
| `encoding.compression.rle` | [Run Length Encoding](https://lance.org/format/file/encoding/#run-length-encoding-rle) | done | `src/encoding/values/rle.cpp::decode`, `src/encoding/values/rle.cpp::decode_levels`, `src/file/data_file_writer.cpp::page_layout_bytes_rle` | `nano_lance_encoding_rle`, `nano_lance_rle_roundtrip`, `nano_lance_dict_rle_roundtrip`, `bindings/python/tests/test_lance_read_matrix.py::test_dictionary_with_rle_indices_reads_back`, `bindings/python/tests/test_lance_parity.py::test_reads_run_length_encoded_definition_levels`, `bindings/python/tests/test_rle_multichunk.py::test_multichunk_rle_matches_pylance`, `nano_lance_encoding_rle` | unit+interop | Values, dictionary indices and definition levels in mini-blocks. nanolance's choice (`encoding_choice`) uses pylance's size model (`runs * (bytes + 1) < rows * bytes`) unless `lance-encoding:rle-threshold` is set (W1g). RLE splits into mini-block chunks as pylance 12 does (W1q: a power of two up to 4,096 rows, at most 8,192 encoded bytes); the chunk words are u32 (nanolance writes 2.2), pylance's are u16. Float64 runs are still written flat. |
| `encoding.compression.byte_stream_split` | [Byte Stream Split](https://lance.org/format/file/encoding/#byte-stream-split-bss) | done | `src/encoding/values/byte_stream_split.cpp::decode`, `src/file/data_file_writer.cpp::page_layout_bytes_bss_general` | `nano_lance_encoding_byte_stream_split`, `app_bss_zstd_pylance_interop_smoke`, `bindings/python/tests/test_write_encoding_matrix.py::test_written_encoding_is_readable_by_both` | unit+interop | Under General(zstd), as Lance applies it; written for float columns when compression is on. |
| `encoding.compression.general.read` | [General](https://lance.org/format/file/encoding/#general) | done | `src/encoding/values/general.cpp::decompress`, `src/encoding/lz4_block.cpp::decompress_block`, `src/encoding/lance_column_decoder.cpp::decompress_dictionary_block` | `nano_lance_encoding_general`, `nano_lance_lz4_block`, `bindings/python/tests/test_lance_parity.py::test_reads_lz4_compressed_dictionaries_written_by_stock_lance`, `bindings/python/tests/test_lance_read_matrix.py::test_a_zstd_frame_over_64_mib_streams_back` | unit+interop | zstd and LZ4, the two schemes `CompressionScheme` defines, in mini-block chunks, dictionaries and full-zip values, and General over InlineBitpacking chunks (W1g, pylance's float64 zstd layout). Any other scheme number is refused. |
| `encoding.compression.general.write` | [General](https://lance.org/format/file/encoding/#general) | partial | `src/encoding/lz4_block.cpp::compress_block`, `src/encoding/values/general.cpp::zstd_frame`, `src/encoding/values/general.cpp::lz4_frame`, `src/file/data_file_writer.cpp::page_layout_bytes_variable_zstd`, `src/file/data_file_writer.cpp::page_layout_bytes_bss_general` | `nano_lance_lz4_block`, `nano_lance_encoding_general`, `nano_lance_compression_roundtrip`, `bindings/python/tests/test_column_codec.py`, `bindings/python/tests/test_file_api.py::test_zstd_column_option_matches_pylance` | unit+interop | LZ4 and zstd in mini-block chunks: variable-width columns, 64-bit integers and floats under byte-stream-split (floats only with the writer-wide compress flag). Not written: General over dictionaries under the option (pylance also LZ4-wraps dictionaries by default), General over a float's bitpacking (pylance: `zstd` on float64), and per-value compression in full-zip pages. |
| `encoding.config` | [Compression Configuration](https://lance.org/format/file/encoding/#compression-configuration) | partial | `src/file/data_file_writer.cpp::read_column_codec`, `src/file/data_file_writer.cpp::encode_column`, `src/file/blob_v2_external.cpp::blob_v2_thresholds` | `bindings/python/tests/test_column_codec.py`, `bindings/python/tests/test_encoding_options.py`, `bindings/python/tests/test_fullzip_fixed.py`, `bindings/python/tests/test_file_api.py::test_zstd_column_option_matches_pylance`, `bindings/python/tests/test_blob_kinds.py::test_placement_by_size_matches` | interop | Honoured: `lance-encoding:compression` = lz4 / zstd / none (`none` makes fixed-width values flat; lz4 and zstd change 64-bit integers and variable-width values, and nothing else; bool is always bit-packed; an unknown value is refused, as pylance refuses it; nested columns ignore it, as pylance does), `compression-level` for zstd (a number sets the level, anything else keeps the writer's level), and the blob size thresholds. Also honoured (W1g): `rle-threshold` (0 turns RLE off for the column; a positive t takes RLE only below t x rows, pylance's guard, on top of nanolance's size check), `dict-divisor` (a dictionary holds at most rows / divisor distinct values; 0 reads as 1), `dict-size-ratio` (a value outside (0, 1] is refused by name; pylance panics), `dict-values-compression` = none / lz4 / zstd (+ `dict-values-compression-level`) on a dictionary column, and the reader decodes General{ZSTD / LZ4, InlineBitpacking} (pylance's float64 zstd layout). Also honoured (W1i): `structural-encoding` = fullzip / miniblock on a fixed-width column (fullzip forces FullZip, miniblock keeps mini-block, any other value is refused as pylance refuses it; a fullzip request nanolance cannot write yet is refused by name); on variable-width columns it is not honoured. Also honoured (W1r): `compression` on a string dictionary column (the dictionary indices take pylance's layout: `General{LZ4 / ZSTD, ByteStreamSplit{Flat(32)}}` for lz4 / zstd, `Flat(32)` for none; the dictionary buffer follows `dict-values-compression`). The reader unwraps the General and byte-stream-split per index chunk, for dictionaries of any width. Writer-wide compression does not override a column's own option. Not honoured: `bss`, the integer dictionary pylance picks for low-cardinality ints (nanolance has no integer dictionary; W1p), `dict-rle` columns under `compression` (the option is still erased there; not measured), and the `LANCE_ENCODING_*` / `LANCE_MINIBLOCK_MAX_VALUES` variables. |
| `encoding.compression.v2_3` | [Generic Unsigned Blocks](https://lance.org/format/file/encoding/#generic-unsigned-blocks) | missing | | | - | Lance 2.3 (unstable) block codecs: generic unsigned blocks, [Range](https://lance.org/format/file/encoding/#range), [Delta](https://lance.org/format/file/encoding/#delta), [Block Dictionary](https://lance.org/format/file/encoding/#block-dictionary), generic variable offsets. |
| `encoding.compression.alp` | [ALP](https://lance.org/format/file/encoding/#alp) | missing | | | - | Lance 2.3 (unstable) float compression. |
| `encoding.compression.axis_delta` | [Axis Delta](https://lance.org/format/file/encoding/#axis-delta-unstable-23) | missing | | | - | Lance 2.3 (unstable) delta along a fixed-size-list axis. |
| `encoding.search_cache` | [Search Cache](https://lance.org/format/file/encoding/#search-cache) | n/a | | | - | A reader's caching strategy, not bytes on disk; nanolance keeps its own caches of decoded pages and open files. |
| `encoding.v2_0` | [Versioning](https://lance.org/format/file/versioning/) | done | `src/encoding/lance_v20_decoder.cpp::decode_column`, `src/encoding/lance_v20_decoder.cpp::parse_page_encoding` | `nano_lance_lance_v20_decoder`, `bindings/python/tests/test_format_v2_0.py::test_whole_dataset_matches_pylance`, `bindings/python/tests/test_format_v2_0.py::test_compressed_strings`, `bindings/python/tests/test_format_v2_0.py::test_packed_struct` | unit+interop | Read only. Flat, Nullable, FixedSizeList, List, Struct, Binary, Dictionary, Fsst, PackedStruct, Bitpacked, FixedSizeBinary, BitpackedForNonNeg, Constant. File-level (type 2) buffers are not read. The spec no longer documents the 2.0 strategy; nanolance follows `encodings_v2_0.proto`. |
| `encoding.v2_0.write` | [Versioning](https://lance.org/format/file/versioning/) | missing | | | - | nanolance writes format 2.2 only. |

## Known gaps

- **Lance 2.3 (unstable)**: planar / sparse pages, the generic block codecs, ALP and axis delta are not read or
  written. A 2.3 page is refused by name (field number), so nothing is misread. Worth doing once 2.3 is stable
  and pylance writes it by default.
- **Packed structs in 2.1 / 2.2** (`PackedStruct`, `VariablePackedStruct` beyond the blob descriptor): opt-in
  in Lance (`lance-encoding:packed`), so rare in the wild; reading them needs a CompressiveEncoding variant in
  `page_layout` and a row-major split in the decoder.
- **The block / mini-block `Constant` CompressiveEncoding** is not modelled. nanolance has not met a file that
  uses it; one would be refused by field number.
- **Writer subsets**: no fixed-width full-zip, no per-value compression in full-zip, zstd but not LZ4 (dictionary values
  included), and most `lance-encoding:*` field options ignored. Every choice made is one Lance reads;
  the cost is size or random-access speed against pylance on some shapes, not correctness.
- **Format 2.0** is read, not written (nanolance writes 2.2, and appends 2.2 files to older datasets).

## Performance

Measured by [bench/encoding/decode_bench.cpp](../../bench/encoding/decode_bench.cpp) (`nl_bench_encoding`). It writes one
`.lance` file per column, one column per kind of encoding this layer decodes. The writer picks each column's kind from its
field tag (`nanolance:packing`). The benchmark reads each column's page layout to name its kind, checks the values read
back, and times `decode_lance_physical_column` on each column: the median of 7 runs after one warm-up, with the file in the
OS cache. Each column is sized to the same decoded bytes, 16 MiB, so the cases take milliseconds and compare by bytes per
second. A column gets the rows that fill 16 MiB (at least 1000), so the int64 columns hold about 2 million rows and the
4200-byte strings about 4000. Decoded bytes are the values of a fixed-width column, or the offsets and text of a
variable-width one. The timing includes allocating the decoded buffers. The full-zip column holds 4200-byte strings, since
the writer lays a column out as FullZip only when its values average 4096 bytes or more.

On a shared machine the numbers move between runs: two back-to-back runs of the same build on the 4-core cloud sandbox
differed by up to 34% on some cases. Before calling a change a regression there, run each build several times and
compare the best median of each; on a dedicated machine the 15% threshold of `tools/compare_bench.py` applies as is.

```
cmake --build build --target nl_bench_encoding
./build/nl_bench_encoding --json out.json
python3 tools/compare_bench.py bench/results/layers/encoding.json out.json
```

`--mb N` sets the decoded MiB per column (default 16). `--rows N` forces one row count for every column instead, which
overrides `--mb`. `--runs N` sets the timed runs, and `--quick` a small run (0.5 MiB per column, 2 runs). ctest runs the
quick mode as `nano_lance_bench_encoding_quick`.

Baseline from one full run, saved as [bench/results/layers/encoding.json](../../bench/results/layers/encoding.json). The
library sources are unchanged since commit 7330b61, and the benchmark code was uncommitted when it ran. The numbers come from
a 4-core cloud sandbox with a Release build (gcc 13.3.0), so they are for comparison on the same machine, not absolute.

| column | kind | rows | decoded bytes | median ms | rows/s | MB/s |
|---|---|---:|---:|---:|---:|---:|
| int64_seq | bitpack | 2,097,152 | 16,777,216 | 5.670 | 369,890,584 | 2,959 |
| int64_random | flat | 2,097,152 | 16,777,216 | 4.880 | 429,719,342 | 3,438 |
| int64_constant | constant | 2,097,152 | 16,777,216 | 1.860 | 1,127,684,172 | 9,021 |
| int64_runs | rle | 2,097,152 | 16,777,216 | 1.146 | 1,830,628,907 | 14,645 |
| float64_random | byte-stream-split+zstd | 2,097,152 | 16,777,216 | 6.259 | 335,048,770 | 2,680 |
| bool | bool-packed | 16,777,216 | 16,777,216 | 2.912 | 5,760,644,970 | 5,761 |
| str_few_values | dict | 1,198,373 | 16,777,226 | 8.466 | 141,550,863 | 1,982 |
| str_runs | dict-rle | 1,864,136 | 16,777,228 | 3.367 | 553,605,538 | 4,982 |
| str_unique | variable | 1,277,738 | 16,777,226 | 12.889 | 99,131,321 | 1,302 |
| str_320_bytes | variable | 51,782 | 16,777,372 | 5.046 | 10,262,010 | 3,325 |
| str_4200_bytes | full-zip | 3,991 | 16,778,168 | 5.048 | 790,630 | 3,324 |
