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
- **Writing legacy formats:** `encoding.v2_0.write`, `file.version.2_0` / `2_1` writes, `file.version.0_1`.
  The roadmap puts writing data storage versions other than 2.2 last.
- **Other object stores and external manifest stores:** `io.store.other_schemes`,
  `io.store.external_manifest_store`, `table.transaction.external_manifest_store`, `api.lance_c.object_stores`.
- **Newer feature flags, each a subsystem:** managed blobs (`file.blob.managed`, `table.blob.managed`),
  the fragment tree, clustering, spilled row ids (`table.rowid.spilled`, `scan.row_ids.spilled`), MemWAL
  SSTables, non-reusable field ids, fragment metadata maps. A dataset carrying one of these flags is refused by
  name today, which is correct.

## Wave 1: io, arrow, encoding, file

| id | rows | what | measured | status |
|---|---|---|---|---|
| W1a | `encoding.compression.constant`, `file.version.fsl_all_null_items` | Read the `Constant` CompressiveEncoding (field 3, `message Constant { optional bytes value = 1; }`) | pylance 12 writes a fixed_size_list whose items are all null as `FullZip{values=FixedSizeList{2xConstant,validity}}`, and nanolance refuses it ("unsupported FullZip value compression") | delegated |
| W1b | `arrow.types.string_binary` | Accept Utf8View / BinaryView on write and store them as `string` / `binary` | pylance 12 writes `string_view` / `binary_view` as `string` / `binary`, and reads them back as those types | delegated |
| W1c | `io.store.storage_options` | Every `AmazonS3ConfigKey` alias, and object_store's boolean spellings | object_store 0.12.4 `aws/builder.rs`: see the brief | delegated |
| W1d | (test coverage) | Direct C++ tests for the modules in `tools/tests_allow.txt` that can have one | -- | delegated |
| W1e | `arrow.fields.primary_key`, `table.schema.primary_key` | Unenforced primary key: Arrow metadata to fields 12-15, and back | to measure | next |
| W1f | `file.structure.page_priority` | Write each page's priority as Lance does | to measure | next |
| W1g | `encoding.config` | Honour the `lance-encoding:*` field metadata Lance honours | to measure, per key | next |
| W1h | `encoding.compression.general.write` | LZ4 | to measure | next |
| W1i | `encoding.layout.full_zip.write` | Fixed-width full-zip and per-value compression | to measure | later |
| W1j | `encoding.semi_structural.packed_struct`, `variable_packed_struct` | Packed structs, read and write | to measure | later |
| W1k | `arrow.types.fixed_size_list`, `arrow.types.dictionary` | FSL of variable-width elements; dictionaries inside structs and lists | to measure | later |

## Wave 2 onwards

Waves 2 to 7 cover table, expr, scan, index, dataset and api, in that order. They are briefed when wave 1 is
reviewed, from the rows the README tables list at that point.
