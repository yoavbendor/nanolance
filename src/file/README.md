# file: how a .lance file is framed

<!-- layer-deps:start -->
Layer 5 of 11 (1 is the bottom, 11 the top). Uses: [base](../base/README.md), [io](../io/README.md), [arrow](../arrow/README.md), [encoding](../encoding/README.md). Used by: [table](../table/README.md), [scan](../scan/README.md), [index](../index/README.md), [dataset](../dataset/README.md), [api](../api/README.md). See [docs/LAYERS.md](../../docs/LAYERS.md).
<!-- layer-deps:end -->
Example: [examples/layers/file.cpp](../../examples/layers/file.cpp) (run with ctest -R nano_lance_example_file).

This layer reads and writes the container a Lance data file is: data pages, column metadata, the column
metadata and global buffer offset tables, global buffers (the schema in buffer 0) and the 40-byte footer, plus
the file-level storage of Blob v2 values (inline bytes in the data file, packed and dedicated sidecar files,
external references). It writes every column's pages with the encoding layer's encoders and hands the reader's
page decoders a `DataFileIo`; how a page is laid out inside is the encoding layer's, and which files make up a
dataset version is the table layer's. It uses base, io, arrow and encoding.

## Using it

- **Reading** (`nanolance/data_file_reader.hpp`): `read_lance_data_file_footer_and_descriptor` reads the tail,
  checks the magic and version (2.0, which the footer spells 0.3, or 2.1 / 2.2; legacy 0.1 / 0.2 and unknown
  versions are refused by name) and parses the schema from global buffer 0 into a `pb::FileDescriptor`, with
  the offsets in a `LanceDataFileFooterLayout`. Then `read_lance_data_file_column_metadatas` (all columns),
  `read_lance_file_global_buffer(index)` and `read_lance_data_file_bytes(offset, size)`. To decode a column,
  wrap the path in a `DataFileIo` and pass it to the encoding layer's `decode_lance_physical_column*`.
- **Open-file cache**: the reader keeps a per-thread cache of open handles, validated by size and mtime so a
  rewritten file under the same name is noticed. Wrap one read operation in a `DataFileReadScope` so each file
  is validated once per operation rather than once per page; outside a scope every lookup validates. After
  deleting files call `close_data_files_under(path_or_prefix)` so their handles (and on NFS, their space) are
  released.
- **Writing a data file** (`nanolance/data_file_writer.hpp`): `write_lance_data_file(dataset_path, file_name,
  mapping, column_values, rows, compression_level, compress, result, error, parallel_columns, extras)`. The
  caller passes one `ColumnValues` per physical field of `mapping` (from `map_arrow_schema`, in column order)
  -- a count that does not match is refused -- each holding `rows` rows. The file goes to
  `<dataset_path>/data/<file_name>` (or `extras->path`); `result` gets its relative path and size. The file is
  always format 2.2. With `parallel_columns` and more than one thread, columns are encoded side by side into
  memory; without, each streams to the file as it is encoded. The bytes are the same either way.
- **A standalone Lance file** (`nanolance/lance_file_writer.hpp`): `write_lance_file(schema, batches, extras,
  file_size, error)` writes Arrow batches as one file at `extras.path`, with `extras.schema_metadata` and
  `extras.global_buffers` (numbered from 1; buffer 0 is the schema) -- what Lance's index files are.
- **Blob v2** (`nanolance/blob_v2_external.hpp`): `append_blob_v2_batch_column_values` turns a batch's blob
  column into descriptor rows; the writer places pending bytes (`BlobV2Placer`: inline in the data file,
  packed into shared sidecars, or one dedicated sidecar each, by the field's thresholds) when the data file is
  written. On read, `blob_v2_locate` resolves a descriptor to a file and range and `blob_v2_read` reads it.
- Errors are a `false` return with `error` set; nothing throws. The reader treats every offset and size in the
  file as untrusted and checks it against the file's size. The writer writes a local file or an object-store
  object uploaded on commit (`io::open_output`); a failed write leaves no committed file.

## Modules

| module | what it holds | tests |
|---|---|---|
| `data_file_reader` | the footer, schema, column metadata and global buffer reads; byte reads through a per-thread cache of open files; `DataFileIo` | `nano_lance_data_file_reader`, `nano_lance_data_file_writer` |
| `data_file_writer` | `write_lance_data_file`: every column's pages, then the schema, column metadata, offset tables and footer | `nano_lance_data_file_writer` |
| `lance_file_writer` | `write_lance_file`: Arrow batches as one standalone Lance file with extra global buffers | `nano_lance_lance_file_writer` |
| `blob_v2_external` | Blob v2 descriptors: building them from batches, placing bytes inline or in sidecars, locating and reading them | `nano_lance_blob_v2_external_preprocess` |

## Spec coverage

Against the Lance format specification at https://lance.org/format/ (as of Lance 12.0.0).

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `file.structure.data_pages` | [Disk Pages](https://lance.org/format/file/#disk-pages) | done | `src/file/data_file_writer.cpp::write_lance_data_file`, `src/file/data_file_writer.cpp::align64`, `src/file/data_file_reader.cpp::read_lance_data_file_bytes` | `nano_lance_data_file_writer`, `nano_lance_data_file_reader`, `bindings/python/tests/test_pylance_compat.py::test_files_agree` | unit+interop | Page buffers at absolute offsets, 64-byte aligned ([Buffer Alignment](https://lance.org/format/file/#buffer-alignment)), no row groups. |
| `file.structure.page_priority` | [Column Metadata](https://lance.org/format/file/#column-metadata) | partial | `src/file/data_file_writer.cpp::write_nested_column` | `bindings/python/tests/test_pylance_compat.py::test_files_agree` | interop | The writer sets every page's `priority` to 0, not the top-level row of its first row; pylance reads the files regardless. The reader ignores it and counts rows from page lengths. |
| `file.structure.column_metadata` | [Column Descriptors](https://lance.org/format/file/#column-descriptors) | done | `src/file/data_file_reader.cpp::read_lance_data_file_column_metadatas`, `src/file/data_file_writer.cpp::write_lance_data_file` | `nano_lance_data_file_writer`, `nano_lance_data_file_reader`, `bindings/python/tests/test_format_v2_0.py::test_data_files_through_the_file_reader` | unit+interop | One `ColumnMetadata` per column: pages (buffer offsets and sizes, length, encoding), the column encoding, column buffers (read in 2.0, where a dictionary can live there; nanolance writes none). |
| `file.structure.offset_tables` | [Offsets & Footer](https://lance.org/format/file/#offsets-footer) | done | `src/file/data_file_reader.cpp::read_lance_data_file_column_metadatas`, `src/file/data_file_writer.cpp::write_lance_data_file` | `nano_lance_data_file_writer`, `nano_lance_lance_file_writer` | unit | The column metadata offset table and the global buffer offset table, (position, size) u64 pairs. |
| `file.structure.footer` | [Offsets & Footer](https://lance.org/format/file/#offsets-footer) | done | `src/file/data_file_reader.cpp::read_lance_data_file_footer_and_descriptor`, `src/file/data_file_writer.cpp::write_lance_data_file` | `nano_lance_data_file_writer`, `nano_lance_data_file_reader`, `bindings/python/tests/test_pylance_compat.py::test_files_agree` | unit+interop | The 40-byte footer: three offsets, global buffer and column counts, major and minor version, `LANC`. |
| `file.structure.global_buffers` | [External Buffers](https://lance.org/format/file/#external-buffers) | done | `src/file/data_file_reader.cpp::read_lance_file_global_buffer`, `src/file/lance_file_writer.cpp::write_lance_file` | `nano_lance_data_file_writer`, `nano_lance_lance_file_writer`, `bindings/python/tests/test_file_api.py::test_global_buffer_and_schema_metadata_match_pylance` | unit+interop | Global buffer 0 is the schema; extra buffers from 1, written and read. |
| `file.structure.schema` | [Identifiers and Type Systems](https://lance.org/format/file/#identifiers-and-type-systems) | done | `src/file/data_file_reader.cpp::read_lance_data_file_footer_and_descriptor`, `src/file/data_file_writer.cpp::write_lance_data_file` | `nano_lance_data_file_writer`, `bindings/python/tests/test_pylance_compat.py::test_schema_metadata_is_written_and_read`, `bindings/python/tests/test_file_api.py::test_global_buffer_and_schema_metadata_match_pylance` | unit+interop | The `FileDescriptor` (fields, row count, schema metadata) in global buffer 0. |
| `file.structure.statistics` | [File Structure](https://lance.org/format/file/#file-structure) | n/a | | | - | The spec allows column statistics in global buffers but defines no format for them; nanolance writes none and reads none. |
| `file.read.strategy` | [Reading Strategy](https://lance.org/format/file/#reading-strategy) | done | `src/file/data_file_reader.cpp::read_lance_data_file_footer_and_descriptor`, `src/file/data_file_reader.cpp::read_lance_data_file_column_metadatas` | `nano_lance_data_file_reader`, `bindings/python/tests/test_io_stats.py` | unit+interop | One tail read (the byte source's footer size, at least 512 bytes) for footer and offsets, then the column metadata, in one read when the columns are adjacent. |
| `file.version.0_1` | [Versioning](https://lance.org/format/file/versioning/) | missing | | | - | Legacy format 0.1 (footer 0.1 / 0.2) is refused by name. |
| `file.version.2_0` | [Versioning](https://lance.org/format/file/versioning/) | partial | `src/file/data_file_reader.cpp::read_lance_data_file_footer_and_descriptor`, `src/encoding/lance_v20_decoder.cpp::decode_column` | `nano_lance_lance_v20_decoder`, `bindings/python/tests/test_format_v2_0.py::test_whole_dataset_matches_pylance`, `bindings/python/tests/test_format_v2_0.py::test_pylance_writes_format_2_0` | unit+interop | Read (footer 0.3, every field a column); not written. |
| `file.version.2_1` | [Versioning](https://lance.org/format/file/versioning/) | partial | `src/file/data_file_reader.cpp::read_lance_data_file_footer_and_descriptor` | `bindings/python/tests/test_pylance_compat.py::test_pylance_written_shapes_read_back`, `bindings/python/tests/test_format_v2_0.py::test_append_to_an_older_format` | interop | Read; not written. Appending to a 2.0 or 2.1 dataset writes 2.2 files beside them (with the mixed-version manifest flag, table layer). |
| `file.version.2_2` | [Versioning](https://lance.org/format/file/versioning/) | done | `src/file/data_file_writer.cpp::write_lance_data_file`, `src/file/data_file_reader.cpp::read_lance_data_file_footer_and_descriptor` | `nano_lance_data_file_writer`, `bindings/python/tests/test_write_encoding_matrix.py::test_written_encoding_is_readable_by_both`, `bindings/python/tests/test_lance_list_writes.py::test_nested_column_round_trips`, `bindings/python/tests/test_pylance_compat.py::test_pylance_written_shapes_read_back` | unit+interop | Read and written: maps, large mini-block chunks, Blob v2. |
| `file.version.2_3` | [Versioning](https://lance.org/format/file/versioning/) | missing | | | - | Unstable; its footer version is refused. |
| `file.version.fsl_all_null_items` | [Compatibility Caveats](https://lance.org/format/file/versioning/#fixedsizelist-with-all-null-inner-values-lance-1110) | partial | `src/encoding/lance_column_decoder.cpp::full_zip_page_params`, `src/encoding/page_layout.cpp::convert_constant` | `bindings/python/tests/test_fsl_all_null_items.py` | interop | Pylance-written all-null FixedSizeList columns (Constant items, Lance 12) read equal to pylance over 248 shapes. Left (strict xfail): list<fsl> all null, small struct<fsl> with null elements, fsl<fsl>, fsl<string>. The Lance 11.1.0 spelling (per-row validity bytes) has no test. |
| `file.blob.v2_descriptor` | [Blob v2 data file descriptor](https://lance.org/format/table/blob/) | done | `src/file/blob_v2_external.cpp::blob_v2_descriptor_page_encoding`, `src/file/blob_v2_external.cpp::blob_v2_unpack_descriptor_row`, `src/file/data_file_writer.cpp::write_nested_blob_column` | `nano_lance_blob_v2_external_preprocess`, `nano_lance_blob_v2_layout`, `bindings/python/tests/test_blob_kinds.py::test_every_kind_written_and_read_both_ways`, `bindings/python/tests/test_pylance_compat.py::test_pylance_blobs_read_back` | unit+interop | The five-field packed struct marked `lance-encoding:blob` and `packed`, top-level and under lists or structs. |
| `file.blob.inline` | [Blob v2 kinds](https://lance.org/format/table/blob/) | done | `src/file/blob_v2_external.cpp::BlobV2Placer::place`, `src/file/blob_v2_external.cpp::blob_v2_locate` | `bindings/python/tests/test_blob_kinds.py::test_placement_by_size_matches`, `bindings/python/tests/test_blob_kinds.py::test_read_blob_ranges_and_pandas` | interop | Kind 0: bytes in an out-of-line buffer of the owning data file. |
| `file.blob.sidecars` | [Blob v2 sidecar files](https://lance.org/format/table/blob/) | done | `src/file/blob_v2_external.cpp::blob_v2_sidecar_path`, `src/file/blob_v2_external.cpp::BlobV2Placer::write_sidecar` | `bindings/python/tests/test_blob_writers.py::test_packed_writer_mixed_calls_match_pylance`, `bindings/python/tests/test_blob_writers.py::test_dedicated_writer_matches_pylance`, `bindings/python/tests/test_blob_kinds.py::test_placement_by_size_matches` | interop | Kinds 1 (packed) and 2 (dedicated): `.blob` sidecars under a directory named after the data file, ids as 32 binary digits LSB first. |
| `file.blob.external` | [Blob v2 kinds](https://lance.org/format/table/blob/) | done | `src/file/blob_v2_external.cpp::preprocess_blob_v2_external_row`, `src/file/blob_v2_external.cpp::blob_v2_read` | `nano_lance_blob_v2_external_preprocess`, `bindings/python/tests/test_blob_kinds.py::test_whole_external_object`, `bindings/python/tests/test_blob_kinds.py::test_ingest_copies_external_bytes` | unit+interop | Kind 3: an absolute URI or a path relative to a base. |
| `file.blob.managed` | [Blob v2 kinds](https://lance.org/format/table/blob/) | missing | | | - | Kinds 4 (Managed) and 5 (ManagedWithBase), `<base>/_blobs/<uuid>.blob`, `lance.blob.managed` and the `FLAG_MANAGED_BLOBS` feature flag. |
| `file.blob.legacy` | [Blob Page Layout](https://lance.org/format/file/encoding/#blob-page-layout) | done | `src/encoding/lance_column_decoder.cpp::decode_legacy_blob_column`, `src/encoding/lance_v20_decoder.cpp::is_blob_column` | `bindings/python/tests/test_blob_kinds.py::test_legacy_blob_columns_read`, `bindings/python/tests/test_format_v2_0.py::test_blob_column` | interop | Blob v1 in 2.0 and 2.1 files: (position, size) of bytes elsewhere in the data file. Read only; written as Blob v2, as Lance does for 2.2. |

## Known gaps

- **Managed blobs** (kinds 4 and 5, Lance-owned objects under `_blobs/`) are neither read nor written; a
  dataset that sets `FLAG_MANAGED_BLOBS` is refused by the table layer as an unknown reader feature flag. Needed once
  pylance writes them by default.
- **Page priority** is written as 0 for every page. Lance's readers use it only to order I/O, and pylance reads
  the files; writing each page's first top-level row would be a few lines in each page writer.
- **Older and newer formats**: 0.1 is refused; 2.0 and 2.1 are read but not written (nanolance writes 2.2
  everywhere, including appends to older datasets); 2.3 is unstable and refused.
- **The FixedSizeList all-null compatibility caveat** is untested in both spellings; a pylance-written file of
  that shape belongs in `test_lance_read_matrix.py`.
- **Statistics** in global buffers are not written: the specification leaves their format open.

## Performance

Measured by [bench/file/file_bench.cpp](../../bench/file/file_bench.cpp) (`nl_bench_file`). Four cases, each checked before
it is timed, each the median of 7 runs after one warm-up. The footer cases read the footer and schema of a 1-column,
100-row file and of a 50-column, 10,000-row file. The metadata case reads every column's metadata of the wide file. The
end-to-end case reads the footer, the metadata and one column of a one-column int64 file of 1,000,000 rows, and decodes it.
The reader keeps a per-thread cache of open handles and checks each one's size and mtime on every call, so after the first
call the footer cases open no file. Footer and metadata cases decode no values, so they have no decoded bytes or MB/s, and their rows/s counts the rows
of the file.

```
cmake --build build --target nl_bench_file
./build/nl_bench_file --json out.json
python3 tools/compare_bench.py bench/results/layers/file.json out.json
```

`--rows N` sets the rows of the end-to-end file (default 1000000), `--runs N` the timed runs, and `--quick` a small run.
ctest runs the quick mode as `nano_lance_bench_file_quick`.

Baseline from one full run, saved as [bench/results/layers/file.json](../../bench/results/layers/file.json), built the same
way as the encoding baseline. The numbers come from a 4-core cloud sandbox with a Release build (gcc 13.3.0), so they are
for comparison on the same machine, not absolute.

| case | rows | decoded bytes | median ms | rows/s | MB/s |
|---|---:|---:|---:|---:|---:|
| footer_small_1col | 100 | - | 0.0094 | 10,654,166 | - |
| footer_wide_50col | 10,000 | - | 0.0213 | 469,263,257 | - |
| column_metadata_wide_50col | 10,000 | - | 0.0427 | 234,055,003 | - |
| end_to_end_int64_1col | 1,000,000 | 8,000,000 | 2.5268 | 395,757,167 | 3,166 |
