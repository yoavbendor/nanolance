# io: where bytes live: local files and object stores (S3 is a registered backend)

<!-- layer-deps:start -->
Layer 2 of 11 (1 is the bottom, 11 the top). Uses: [base](../base/README.md). Used by: [file](../file/README.md), [table](../table/README.md), [scan](../scan/README.md), [dataset](../dataset/README.md), [api](../api/README.md). See [docs/LAYERS.md](../../docs/LAYERS.md).
<!-- layer-deps:end -->
Example: [examples/layers/io.cpp](../../examples/layers/io.cpp) (run with ctest -R nano_lance_example_io).

The io layer reads and writes whole files and byte ranges, and knows nothing of what they hold. A dataset's
manifests, data files, deletion files and index files are all opened through it, by a local path or a
`scheme://` URI, so the same reader runs on a local directory, on S3 and on an in-memory store a test
registers. It also fetches external blobs (`file://`, `s3://`) and confines manifest-supplied relative paths
under their dataset. Which files a dataset has (`_versions/`, `data/`, `_deletions/`, base paths, the version
hint) is the table layer's; this layer uses only `base`.

## Using it

- **Reading** (`include/nanolance/byte_source.hpp`): `open_source(path_or_uri, error)` gives a
  `ByteSource`; read with `read_at(offset, n, dst, error)`, which reads exactly `n` bytes or fails. A
  source keeps no position, so one source can serve readers on many threads. `read_all`, `exists`,
  `stat_local` and `list_dir` (a missing local directory lists as empty) cover the rest.
- **Writing** (`include/nanolance/byte_sink.hpp`): `open_output(path_or_uri, error)` gives an
  `OutputFile` (an `std::ostream`); call `commit(error)` when done. An object becomes visible only on
  `commit`, and an output destroyed without it leaves nothing behind (a local file is written in place). The
  publish step of a commit is `put_if_absent`: it never overwrites, and sets `already_exists` when another
  writer got there first, which the table layer turns into a commit conflict. `remove_output` is not an error
  for a missing file.
- **Other schemes**: `register_source_factory`, `register_list_factory` and `register_sink_backend` add a
  scheme (an empty function removes it). A path with no registered `scheme://` prefix is a local file. A
  lister must return every entry under the directory. `s3` registers itself on first use when nanolance is
  built with S3 (`NANOLANCE_ENABLE_S3` and nanos3reader); otherwise an `s3://` URI is refused by name.
- **S3 settings** (`include/nanolance/object_store.hpp`): `object_store_options_from(pairs)` reads
  pylance's storage-option keys; `set_object_store_options(uri, options, error)` stores them for that URI's
  bucket, process-wide (default options clear them, and the AWS environment chain applies). An `http://`
  endpoint needs `allow_http`. Credentials are resolved as docs/s3_reader_notes.md says.
- **Untrusted paths** (`include/nanolance/path_safety.hpp`): join any path read from a manifest under its
  dataset with `safe_join_under(base, relative)`; it refuses empty, absolute and `..` paths. Never join
  such a path with `std::filesystem::path::operator/` directly.
- **External blobs** (`src/io/external_blob.hpp`, private): `fetch_external_blob` and `external_blob_size`
  return a `BlobStatus`; `file://` paths may not contain `..` and must lie under `NANO_LANCE_BLOB_BASE_DIR`
  when that is set. Open objects are cached per thread.
- Errors are a `false` return (or a null pointer) with the reason in `error`; nothing here throws on I/O.

## Modules

| module | what it holds | tests |
|---|---|---|
| `byte_source` | `ByteSource`, local files, the scheme registry, listings | `nano_lance_byte_source_remote`, `nano_lance_byte_sink` |
| `byte_sink` | `OutputFile`, `put_if_absent`, `remove_output`, the sink registry | `nano_lance_byte_sink` |
| `object_store` | S3 storage options per bucket | `nano_lance_object_store` |
| `path_safety` | `safe_join_under`, the jail for manifest paths | `nano_lance_path_safety` |
| `external_blob` | external-blob fetches by URI | `nano_lance_external_blob_io` |
| `s3_byte_source` | the `s3` source and lister (HEAD, range GETs, ListObjectsV2) | none in C++: `bindings/python/tests/test_object_store.py` |
| `s3_byte_sink` | the `s3` write backend (buffered PUTs, multipart, create-only PUT) | none in C++: `bindings/python/tests/test_object_store_commits.py` |

## Spec coverage

Against the Lance format specification at https://lance.org/format/ (as of Lance 12.0.0): its layout page
for how a dataset's files are addressed, and otherwise the object-store behavior pylance has.

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `io.layout.dataset_root` | [Dataset Root](https://lance.org/format/table/layout/#dataset-root) | done | `src/io/byte_source.cpp::open_source`, `src/io/byte_source.cpp::open_local_file`, `src/io/s3_byte_source.cpp::open_s3_source` | `nano_lance_byte_source_remote`, `bindings/python/tests/test_object_store.py::test_whole_table_matches_pylance` | unit+interop | A dataset root is a local directory or a URI of a registered scheme; every file of the dataset is opened through it. |
| `io.layout.relative_paths` | [Dataset Portability](https://lance.org/format/table/layout/#dataset-portability) | done | `include/nanolance/path_safety.hpp::safe_join_under` | `nano_lance_path_safety`, `nano_lance_byte_source_remote` | unit | Paths in a manifest are relative and are joined under their base, never allowed out of it. Resolving `base_id` is the table layer's. |
| `io.layout.listing` | [Object Store Considerations](https://lance.org/format/table/layout/#object-store-considerations) | done | `src/io/byte_source.cpp::list_dir`, `src/io/s3_byte_source.cpp::list_s3` | `nano_lance_byte_source_remote`, `bindings/python/tests/test_object_store.py::test_two_versions_match_pylance` | unit+interop | Listing a directory such as `_versions/`: a local directory, or one ListObjectsV2 per page with delimiter `/`. The version hint is the table layer's. |
| `io.store.range_reads` | ref: pylance object-store reads (HEAD, then ranged GETs) | done | `src/io/s3_byte_source.cpp::open_s3_source`, `include/nanolance/byte_source.hpp::small_read_block`, `include/nanolance/byte_source.hpp::footer_read_size` | `nano_lance_byte_source_remote`, `bindings/python/tests/test_object_store.py::test_filtered_scan_reads_a_fraction_of_the_dataset`, `bindings/python/tests/test_object_store.py::test_take_matches_pylance` | unit+interop | Small reads coalesce into one block; a remote footer read takes a larger tail. |
| `io.store.storage_options` | ref: pylance `storage_options` (object_store's `AmazonS3ConfigKey` names) | partial | `src/io/byte_source.cpp::object_store_options_from`, `src/io/s3_byte_source.cpp::set_object_store_options` | `nano_lance_object_store`, `bindings/python/tests/test_object_store.py::test_storage_option_spellings_agree` | unit+interop | Reads `access_key_id`, `secret_access_key`, `session_token`, `region`, `endpoint` (each also `aws_`-prefixed) and `allow_http` (`true` / `True` / `1`). Missing: the aliases `aws_endpoint_url`, `endpoint_url`, `aws_default_region`, the other truthy spellings object_store takes (`yes`, `on`, `TRUE`), and every other key (virtual-hosted style, timeouts, proxies, `aws_server_side_encryption`, ...), which are ignored. |
| `io.store.credentials` | ref: the AWS credential chain as object_store resolves it | partial | `src/io/s3_byte_source.cpp::s3_client_for_uri` | `bindings/python/tests/test_object_store.py::test_storage_option_spellings_agree`, `bindings/python/tests/test_object_store_commits.py::test_create_append_overwrite_read_by_pylance` | interop | Environment, profiles, `credential_process`, ECS / EKS and IMDSv2 (in nanos3reader). SSO and assume-role profiles are not resolved (docs/s3_reader_notes.md). |
| `io.store.conditional_put` | ref: Lance 12 commits, put-if-not-exists of the next manifest (lance-table `io/commit.rs`; docs/OBJECT_STORE_WRITES.md) | done | `src/io/byte_sink.cpp::put_if_absent`, `src/io/s3_byte_sink.cpp::conditional_puts_honoured` | `nano_lance_byte_sink`, `bindings/python/tests/test_object_store_commits.py::test_racing_writers_lose_and_double_nothing`, `bindings/python/tests/test_object_store_commits.py::test_pylance_and_nanolance_append_to_one_dataset`, `bindings/python/tests/test_object_store_commits.py::test_a_store_that_ignores_if_none_match_is_refused`, `bindings/python/tests/test_object_store_commits.py::test_lost_reply_of_the_manifest_put_commits_once` | unit+interop | Local: a temporary file hard-linked into place. S3: a PUT with `If-None-Match: *`; a bucket that accepts a second create-only PUT is refused for every commit. |
| `io.store.uploads` | ref: object_store multipart uploads (pylance's writer) | done | `src/io/s3_byte_sink.cpp::s3_sink_backend` | `bindings/python/tests/test_object_store_commits.py::test_a_data_file_larger_than_the_memory_buffer_uploads_whole`, `bindings/python/tests/test_object_store_commits.py::test_create_append_overwrite_read_by_pylance` | interop | Buffered in memory, then spooled to a temporary file and sent as one PUT or a multipart upload from 64 MiB; nothing is visible before `commit`. |
| `io.store.other_schemes` | ref: pylance's object stores (`gs://`, `az://`, `memory://`, `file-object-store://`) | missing | | | - | Only local paths and `s3://` (and S3-compatible endpoints) are built in; `memory://` exists only in the lance-c layer, as a private local directory. |
| `io.store.external_manifest_store` | ref: pylance's `s3+ddb://` commit store (DynamoDB) | missing | | | - | Stores without conditional PUT are refused rather than committed to through an external lock. |
| `io.blob.external` | ref: Lance Blob v2 external blobs (a URI with position and size) | done | `src/io/external_blob.cpp::fetch_external_blob`, `src/io/external_blob.cpp::external_blob_size` | `nano_lance_external_blob_io`, `bindings/python/tests/test_blob_kinds.py::test_whole_external_object`, `bindings/python/tests/test_blob_kinds.py::test_every_kind_written_and_read_both_ways` | unit+interop | `file://` and `s3://`; a `..` component is refused, and `NANO_LANCE_BLOB_BASE_DIR` confines `file://` paths. |

## Known gaps

The storage options pylance users pass beyond the basic credential, region and endpoint keys are ignored
rather than refused, so a setting such as virtual-hosted-style addressing or a request timeout has no effect;
adding the aliases and the truthy spellings is a small change in `object_store_options_from`, the rest
needs nanos3reader support. Only S3 is an object store here: Google Cloud Storage and Azure would each be a
new registered backend (source, lister and sink), and a store without conditional PUT would need an external
commit lock such as pylance's DynamoDB store. Credentials from SSO and assume-role profiles have to be
exported first. The two S3 modules have no C++ test: they are not compiled without nanos3reader, and they
are tested against moto by the Python object-store tests (skipped unless `NANOLANCE_S3_TEST_ENDPOINT` is set).
