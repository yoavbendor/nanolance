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
| W1l | `file.version.2_0`, `file.version.2_1` | Test every modification of a 2.0 / 2.1 dataset against pylance, and document what works | measured 2026-10-29, see below | done 2026-10-29: 22 cases (2.0 and 2.1 x 11 operations) match pylance, no library change; left: nothing for these rows (writing 2.0 / 2.1 stays unscheduled, W1n) |
| W1e | `arrow.fields.primary_key`, `table.schema.primary_key` | Unenforced primary key: Arrow metadata to fields 12-15, and back | done 2026-10-29: fields 12, 13, 15 written and read; the Arrow key metadata read as pylance does; pylance's write checks; merge_insert's default key by position. Left: whether pylance writes the key into a data file's schema (not measured); a key nested in a struct as the merge_insert default |
| W1m | `file.version.2_0`, `file.version.2_1` | Which older pylance / LanceDB releases read a dataset with nanolance's 2.2 files beside 2.0 / 2.1 ones | measured 2026-10-29, see below | done: only pylance 12 and later read the mix |
| W1f | `file.structure.page_priority` | Write each page's priority as Lance does | to measure | next |
| W1g | `encoding.config` | Honour the `lance-encoding:*` field metadata Lance honours | to measure, per key | next |
| W1h | `encoding.compression.general.write` | LZ4 | to measure | next |
| W1i | `encoding.layout.full_zip.write` | Fixed-width full-zip and per-value compression | to measure | later |
| W1j | `encoding.semi_structural.packed_struct`, `variable_packed_struct` | Packed structs, read and write | to measure | later |
| W1k | `arrow.types.fixed_size_list`, `arrow.types.dictionary` | FSL of variable-width elements; dictionaries inside structs and lists | to measure | later |
| W1n | `file.version.2_1` | Write 2.1 files into a 2.1 dataset, so its writes stay 2.1 | the 2.2-only encodings pylance leaves out for 2.1: to measure | needed (W1m); waits on the decision below |

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

### Older readers (W1m, measured 2026-10-29)

pylance wrote 2.0 and 2.1 datasets. Copies were then appended to or updated by nanolance ("mixed": 2.2 files
beside the old ones, with the mixed-version flag). Each release read every copy in its own venv.

| pylance | pure 2.0 | pure 2.1 | mixed 2.0 or 2.1 (after any nanolance write) |
|---|---|---|---|
| 0.20.0, 0.25.2, 0.30.0, 0.35.0 | reads | fails ("Task was aborted") | refused: "This dataset cannot be read by this version of Lance. Please upgrade Lance to read this dataset" |
| 0.39.0, 1.0.4, 4.0.2, 7.1.0, 9.0.1, 11.0.0 | reads | reads | refused, the same message |
| 12.0.0, 14.0.2 | reads | reads | reads |

So after nanolance modifies a 2.0 or 2.1 dataset, only pylance 12 and later can read it. For a 2.1 dataset,
W1n (writing 2.1 files) keeps it readable back to pylance 0.39. For a 2.0 dataset, only a 2.0 writer keeps every
release reading it: 2.0 is what LanceDB wrote, and 281 of the 621 Lance tables on the Hugging Face Hub use it.

Options, cheapest first. They can be combined:

1. Warn when a write would add the first 2.2 file to a 2.0 or 2.1 dataset, and document the table above. Small.
2. W1n: write 2.1 into 2.1 datasets. Small to medium, behind a version switch, with 2.2 output kept
   byte-identical.
3. A 2.0 writer for the common flat types (integers, floats, strings, fixed-size-list vectors, plain lists and
   structs), refusing the rest by name. Medium to large: the 2.0 page encodings are written for the first time.
4. Refuse to write into a 2.0 / 2.1 dataset unless the caller opts in (for example `allow_mixed_versions=True`).
   Small, but it changes behaviour that works today with pylance 12+.

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
- Measured beyond the spec (pylance 12.0.0, 2026-10-29):
  - Raw manifest fields: `:position=1` writes field 12 = 1 and field 13 = 1; the flag alone writes 12 = 1; `:position=0`
    writes 12 = 1 (13 is left out); clustering `:position=1` writes field 15 = 1 only; `:position=2` with the flag writes 12 and 13 = 2.
  - Arrow metadata comes back exactly as written, for both spellings (nothing added).
  - The flag is truthy when it is `true`, `yes`, `on`, `1` or `y`, without case (`TRUE`, `On`, `Y` count). `0`, `no`, `off`,
    `false`, `t`, `f`, `2`, `enabled`, `01`, `1 `, ` true`, `true\n` and `""` are not keys.
  - A position that parses (Rust `u32::from_str`: an optional `+`, digits, 32 bits; `+2` is 2, `007` is 7) decides: a key at
    that position, whatever the flag says (`:position=3` with flag `false` is a key at 3; `:position=0` with flag `false`
    is a key). A position that does not parse (`abc`, `-1`, ` 2`, `4294967296`) falls back to the flag: `abc` with the flag
    `true` is a key at 0, and with no flag it is no key.
  - Position 0 reads back as no position (`unenforced_primary_key_position()` is None), as proto3 leaves it out; the key
    still counts for merge_insert. Clustering position 0 is not in the clustering key.
  - merge_insert without `on` takes every key column (the order of the `on` list is not measured: nanolance sorts by position,
    then field id); duplicate positions (1 and 1) and gaps are accepted.
  - Refusals, in the order pylance makes them: a key that is not a leaf (`must be a leaf`, also a struct key); an ancestor
    that is nullable, root first (`must be nullable`, naming the nullable ancestor, even when the key is nullable too); a
    list ancestor (`must not be in a list type`, naming the list); the key itself nullable (`must be nullable`, naming the
    key). The message's Field lists its children: `Field(id=0, name=s, type=struct, children=[Field(id=1, name=id, type=int64), ])`.
