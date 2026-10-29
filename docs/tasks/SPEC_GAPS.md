# Spec gaps, bottom layer first (2026-10-28)

The `## Spec coverage` tables in the layer READMEs list 52 partial and 42 missing rows
(`python3 tools/check_spec.py` prints the counts). This file orders them: layer by layer from the
bottom (io up to api), and inside a layer by how often a user meets the gap, as `docs/ROADMAP.md`
weighs priorities. Reading data pylance writes comes first, then writing what pylance writes, then
options and breadth. Every item adds tests. When it closes a row, the row in the layer README changes
to `done` or `partial`, with what is left.

Items run as in `docs/DELEGATION.md`: pylance's behaviour is measured first and written into the
brief, a Haiku session implements one item in its own worktree, and the reviewer checks the diff,
re-runs the checks and commits.

## Not scheduled

These stay as they are until a user asks, or the Lance pin moves:

- **Lance 2.3 (unstable):** `encoding.layout.planar`, `encoding.compression.v2_3`, `encoding.compression.alp`,
  `encoding.compression.axis_delta`, `file.version.2_3`.
- **Writing legacy formats:** `encoding.v2_0.write` and writing format 2.0 files, `file.version.0_1`. Writing
  format 2.1 is planned conditionally, as W1n below. See "Datasets in formats 2.0 and 2.1".
- **Other object stores and external manifest stores:** `io.store.other_schemes`,
  `io.store.external_manifest_store`, `table.transaction.external_manifest_store`, `api.lance_c.object_stores`.
- **Newer feature flags, each a subsystem:** managed blobs (`file.blob.managed`, `table.blob.managed`),
  the fragment tree, clustering, spilled row ids (`table.rowid.spilled`, `scan.row_ids.spilled`), MemWAL
  SSTables, non-reusable field ids, fragment metadata maps. A dataset carrying one of these flags is refused by
  name today, which is correct.

## Wave 1: io, arrow, encoding, file

| id | rows | what | measured | status |
|---|---|---|---|---|
| W1a | `encoding.compression.constant`, `file.version.fsl_all_null_items` | Read the `Constant` CompressiveEncoding (field 3, `message Constant { optional bytes value = 1; }`) | pylance 12 writes a fixed_size_list whose items are all null as `FullZip{values=FixedSizeList{2xConstant,validity}}`, and nanolance refuses it ("unsupported FullZip value compression") | done 2026-10-28: FullZip FSL items; left: list<fsl>, small struct<fsl>, fsl<fsl>, fsl<string> all-null (strict xfail) |
| W1b | `arrow.types.string_binary` | Accept Utf8View / BinaryView on write and store them as `string` / `binary` | pylance 12 writes `string_view` / `binary_view` as `string` / `binary`, and reads them back as those types | done 2026-10-28 |
| W1c | `io.store.storage_options` | Every `AmazonS3ConfigKey` alias, and object_store's boolean spellings | object_store 0.12.4 `aws/builder.rs`: see the brief | done 2026-10-28; left: virtual-hosted style and skip_signature (nanos3reader has no switch) |
| W1d | (test coverage) | Direct C++ tests for the modules in `tools/tests_allow.txt` that can have one | -- | done 2026-10-28: 4 direct tests; found and fixed the projected-column status; the create-mode commit question is open |
| W1l | `file.version.2_0`, `file.version.2_1` | Test every modification of a 2.0 / 2.1 dataset against pylance, and document what works | measured 2026-10-29, see below | next (first: highest value, lowest risk) |
| W1e | `arrow.fields.primary_key`, `table.schema.primary_key` | Unenforced primary key: Arrow metadata to fields 12-15, and back | measured, see below | next (restarted: the first session's work was lost in a container restart) |
| W1m | `file.version.2_0`, `file.version.2_1` | Which older pylance / LanceDB releases read a dataset with nanolance's 2.2 files beside 2.0 / 2.1 ones | to measure | next; decides W1n |
| W1f | `file.structure.page_priority` | Write each page's priority as Lance does | to measure | next |
| W1g | `encoding.config` | Honour the `lance-encoding:*` field metadata Lance honours | to measure, per key | next |
| W1h | `encoding.compression.general.write` | LZ4 | to measure | next |
| W1i | `encoding.layout.full_zip.write` | Fixed-width full-zip and per-value compression | to measure | later |
| W1j | `encoding.semi_structural.packed_struct`, `variable_packed_struct` | Packed structs, read and write | to measure | later |
| W1k | `arrow.types.fixed_size_list`, `arrow.types.dictionary` | FSL of variable-width elements; dictionaries inside structs and lists | to measure | later |
| W1n | `file.version.2_1` | Write 2.1 files into a 2.1 dataset, so its writes stay 2.1 | the 2.2-only encodings pylance leaves out for 2.1: to measure | only if W1m finds readers that need it |

## Datasets in formats 2.0 and 2.1 (measured 2026-10-29)

pylance 12 wrote 2.0 and 2.1 datasets (300 rows in 3 fragments: int64, string, an 8-float vector, a list). Each
operation then ran once with nanolance and once with pylance, and the results were compared value by value.
pylance then appended to each result.

| operation | result |
|---|---|
| append, delete, update, merge_insert, add_columns, compaction | the same values as pylance's result, and pylance appends afterwards |
| drop_columns, BTREE, INVERTED and IVF_PQ index builds, overwrite | succeed, and pylance reads the result (row counts compared) |

nanolance writes every new data file in format 2.2 and sets the manifest's mixed-version flag, so such a dataset
holds 2.0 or 2.1 files beside 2.2 ones. pylance itself would keep writing the dataset's own version. pylance 12
reads the mix. A release that predates the flag would refuse it, and compaction moves the old files to 2.2.
Only the append case had a test (`test_format_v2_0.py::test_append_to_an_older_format`), which W1l fixes. W1m
decides whether writing 2.1 itself (W1n) is worth doing. Writing 2.0 needs encoders for the 2.0 page
encodings, which nanolance only reads, so it stays unscheduled until a user needs it.

Risk: W1l and W1m change no library code. W1n touches the writer's encoding choice behind a version switch,
so 2.2 output must stay byte-identical (`nl_bench_encoding` / `nlbench_gendata` A/B, as in the refactors).

## Wave 2 onwards

Waves 2 to 7 cover table, expr, scan, index, dataset and api, in that order. They are briefed when wave 1 is
reviewed, from the rows the README tables list at that point.

## Measured for W1e (unenforced primary key)

- Lance 12 `Field` proto: `bool unenforced_primary_key = 12`; `uint32 unenforced_primary_key_position = 13`
  (1-based; 0 means the field is part of the key and orders by field id); `bool unenforced_clustering_key = 14`
  (reserved); `uint32 unenforced_clustering_key_position = 15` (1-based; 0 is not in the key).
- Arrow metadata (`rust/lance-core/src/datatypes/field.rs`): `lance-schema:unenforced-primary-key:position`
  parsed as u32 first; otherwise `lance-schema:unenforced-primary-key` truthy gives position 0;
  `lance-schema:unenforced-clustering-key:position` as u32. pylance keeps the metadata on the Arrow field
  as given.
- pylance 12 write checks: a nullable key column is refused with `LanceError(Schema): Primary key column and all
  its ancestors must not be nullable: Field(id=0, name=id, type=int64)`. A list column is refused with
  `Primary key column must be a leaf: Field(id=0, name=l, type=list, children=[Field(id=1, name=item,
  type=int32), ])`. A non-null leaf inside a non-null struct is accepted.
