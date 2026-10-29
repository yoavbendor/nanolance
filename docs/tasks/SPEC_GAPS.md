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
  format 2.1 is planned as W1n below. See "Datasets in formats 2.0 and 2.1".
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
| W1f | `file.structure.page_priority` | Write each page's priority as Lance does | measured 2026-10-29, see "Measured for W1f (page priority)" | done 2026-10-29: every page's `priority` is the top-level row of its first row, as pylance writes it (int64, list, string, FSL128 measured); single-page columns keep 0 |
| W1g | `encoding.config` | Honour the `lance-encoding:*` field metadata Lance honours | partial (Haiku session, 2026-10-29): done `rle-threshold`, `dict-divisor`, `dict-size-ratio` (refused by name outside (0, 1]), `dict-values-compression` (+ level); reader General over InlineBitpacking. Left: `dict` columns under `compression` (indices as General{codec,BSS{Flat(32)}}: needs the reader's dictionary-index path to unwrap General, then the writer), integer dictionaries (pylance dictionary-encodes low-cardinality ints; nanolance has none), `bss`, `structural-encoding`, multi-chunk RLE, the C++ tests for the refusal and the decode | next |
| W1h | `encoding.compression.general.write` | LZ4 | done for the value layouts measured (LZ4 writer; `compression=lz4` on 64-bit ints and variable-width). Left: General over dictionaries under the option, float zstd layout | done |
| W1i | `encoding.layout.full_zip.write` | Fixed-width full-zip and per-value compression | to measure | later |
| W1j | `encoding.semi_structural.packed_struct`, `variable_packed_struct` | Packed structs, read and write | to measure | later |
| W1k | `arrow.types.fixed_size_list`, `arrow.types.dictionary` | FSL of variable-width elements; dictionaries inside structs and lists | to measure | later |
| W1o | `file.version.2_0`, `file.version.2_1` | Warn when a write adds the first 2.2 file to a 2.0 / 2.1 dataset, naming the readers it cuts off | W1m table | done 2026-10-29: `MixedFormatWarning` before the commit of each write that adds the first 2.2 file to a 2.0 / 2.1 dataset (write_dataset append, update, merge_insert, add_columns, compaction, fragment writes); `error` filter aborts it; `_target_file_version` is the one line W1n changes |
| W1n | `file.version.2_1` | Write 2.1 files into a 2.1 dataset, so its writes stay 2.1 | measured 2026-10-29, see "W1n: what writing 2.1 needs" | deferred (decided 2026-10-29): after W1i, and only if a user needs older readers for 2.1 datasets |

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

Decided 2026-10-29: options 1 and 2, as W1o and W1n. The 2.0 writer (option 3) stays unscheduled until a user
needs older readers to keep reading a 2.0 dataset that nanolance modified.

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

## Measured for W1n (2.1 versus 2.2, what pylance writes)

pylance 12.0.0 wrote one 20,000-row table in 2.1 and in 2.2 (`build/nlance-pagelayout` on each). Encodings
only 2.2 used:

- a non-null constant column: 2.2 `Constant{inline 8B}` (ConstantLayout with an inline value); 2.1
  `MiniBlock{values=Rle{values=Flat(64),lengths=Flat(8)}}`. An all-null column is ConstantLayout in both.
- dictionaries compressed with General: 2.2 `dict=General{LZ4,Variable{...}}`; 2.1 `dict=Variable{...}` (plain).
- dictionaries of fixed-width values: 2.2 `i64_small` -> `InlineBitpacking(32),dict=General{LZ4,Flat(64)}x100`;
  2.1 `InlineBitpacking(64)` (no dictionary). The same for runs of int64 (2.2 RLE over a dictionary, 2.1 plain RLE).

The same in both: bitpacking, RLE, FSST, plain and string dictionaries, nulls, lists, structs, fixed-size-list
vectors, bool, dates. The reference for "valid 2.1": pylance 0.39 to 11.0 read pure 2.1 but nothing of 2.2.

## W1n: what writing 2.1 needs (found 2026-10-29)

Turning off the 2.2-only encodings is not enough. Every mini-block page nanolance writes uses the 2.2 chunk
grammar (`has_large_chunk`: u32 buffer sizes and u32 chunk words). pylance 12 refuses such a page in a 2.1 file:
"Large miniblock chunks are not part of the Lance v2.1 grammar" (`rust/lance-file/src/versions/v2_1/reader.rs:108`).
pylance 1.0.4 panics and 0.39.0 aborts on it. The 2.1 grammar has u16 buffer sizes and u16 chunk words
`(chunk_bytes / 8 - 1) << 4 | log2(values)`, so a chunk holds at most 32 KiB and a buffer at most 64 KiB.
nanolance reads that grammar (`src/encoding/layout/miniblock.cpp`) but writes only the large one.

Writing 2.1 therefore needs:

1. The u16 header and chunk words in `miniblock::` (small).
2. Chunk sizing under those caps, for every mini-block encoder (bitpacked, flat, FSST, dictionary and RLE values,
   variable width, fixed-size lists). A value too wide for a 32 KiB chunk has to go to FullZip, as Lance does.
   For fixed-width values that is W1i, which nanolance cannot write yet (medium).
3. Then the 2.2-only encodings off for 2.1, measured with old readers: ConstantLayout with a value (done in the
   W1n branch), General-compressed dictionaries (`write_dict_rle_column` always zstd-frames its dictionary),
   fixed-width dictionaries, and the rest of the list in "Measured for W1n".

A first implementation is on the local branch `haiku/w1n-finish` (commit e5da80e): the version switch,
`keep_dataset_format` for staged writes, the ConstantLayout gate, a 2.2 fallback for blob v2 columns, and a
28-column test matrix against pylance 12 and old readers. It is not merged: with item 1 missing, its 2.1 files are
unreadable.

## Measured for W1f (page priority)

Lance 12 (`rust/lance-encoding/src/encoder.rs` `write_page_to_data_buffer`) writes a page's `priority` as
`page.row_number`: "the top-level row number of the first row in the page". A top-level row is never split
across pages, so for a list column the page's first item's row is used, not the item index. nanolance writes 0
everywhere (`page.priority = 0` in about ten places in `src/file/data_file_writer.cpp`).

pylance 12.0.0 (priorities read by nanolance's C++ reader, `_file_page_priorities`), each page's priority is the
rows of the pages before it in its column:

| column | pylance's pages (rows, priority) |
|---|---|
| int64, 3,000,000 rows | 3 pages: (1,250,000, 0), (1,250,000, 1,250,000), (500,000, 2,500,000) |
| list<int64>, 300,000 rows | 22 pages of ~14,000 rows: (14,362, 0), (14,310, 14,362), ... last (7,192, 292,808) |
| string, 400,000 rows | 2 pages: (300,000, 0), (100,000, 300,000) |
| fixed_size_list<float,128>, 200,000 rows | 4 pages of 50,000: priority 0, 50,000, 100,000, 150,000 |
| int64 in runs of 50, 500,000 rows (RLE) | 1 page: (500,000, 0) |
| string from 50 values, 500,000 rows (dictionary) | 1 page: (500,000, 0) |

nanolance's pages differ in size (its list and string pages hold up to 32,768 rows; its FSL pages 1,024), but its
priorities follow the same rule. The interop test compares the rule, not the page splits.

## Measured for W1g and W1h (field encoding options)

pylance 12.0.0, one table of 50,000 rows, each column with one option (`build/nlance-pagelayout`):

| column, option | pylance's page |
|---|---|
| strings (5,000 distinct), none | `values=InlineBitpacking(32), dict=General{LZ4,Variable}x5000` |
| same, `compression=lz4` | `values=General{LZ4,ByteStreamSplit{Flat(32)}}`, dict as default |
| same, `compression=zstd` (level 9 too) | `values=General{ZSTD,ByteStreamSplit{Flat(32)}}`, dict as default |
| same, `compression=none` | `values=Flat(32)` (no bitpacking), dict as default |
| random int64, `compression=lz4` / `zstd` | `General{LZ4 / ZSTD,ByteStreamSplit{Flat(64)}}` |
| int64 in runs of 50, none | `Rle{values=Flat(32),lengths=Flat(8)}, dict=InlineBitpacking(64)x1000` |
| same, `rle-threshold=0` | `InlineBitpacking(32), dict=...` (no RLE); `rle-threshold=1` as default |
| strings, `dict-divisor=1000` | `Fsst{...}` (no dictionary); `dict-divisor=1` as default |
| strings, `dict-size-ratio=0.5` | as default; `dict-size-ratio=0.0` makes pylance panic ("not in the range (0, 1]") |
| strings, `dict-values-compression=zstd` / `none` | `dict=General{ZSTD,Variable}` / `dict=Variable` |
| random int64, `structural-encoding=fullzip` | `FullZip{values=Flat(64)}` (fixed-width FullZip: W1i); strings: ignored here |
| random float64, `bss=on` / `off` | `InlineBitpacking(64)` either way |
| int64, `minichunk-size=1024` | as default |

So the options that change the layout are `compression` (lz4 / zstd / none), `rle-threshold`, `dict-divisor`,
`dict-values-compression` and `structural-encoding`. `compression-level` changes only the codec level. nanolance
honours `compression=zstd` only, and has no LZ4 encoder (`nanolance/lz4_block.hpp` decodes only).

### W1h measured again (2026-10-29, same pylance 12.0.0, 50,000 rows, one column per dataset)

The table above does not reproduce for every type: its string rows used a dictionary that the 5,000-value string columns here do not get, and the option is ignored for 8/16/32-bit ints and for float32/float64 under lz4. Measured shapes (`compression=` lz4 / zstd / none):

| column | lz4 | zstd | none |
|---|---|---|---|
| int64 random (40-bit) | `General{LZ4,ByteStreamSplit{Flat(64)}}` | `General{ZSTD,ByteStreamSplit{Flat(64)}}` | `Flat(64)` |
| int64 full range | `Flat(64)` | `Flat(64)` | `Flat(64)` |
| int64 in 1,000 values (dictionary) | dict indices `General{LZ4,BSS{Flat(32)}}` | same, ZSTD | dict indices `Flat(32)` |
| int32 / int16 / int8 random | `InlineBitpacking` (ignored) | ignored | `Flat(N)` |
| float32 random | ignored | ignored | `Flat(32)` |
| float64 random | ignored | `General{ZSTD,InlineBitpacking(64)}` | `Flat(64)` |
| string, 50,000 unique values (`unique-NNNNNNNN-` plus up to 6 x) | `General{LZ4,Variable}` | `General{ZSTD,Variable}` | `Variable` (no FSST) |
| string, 12-letter values from 5,000 (random letters) | `Variable` (LZ4 does not shrink it) | `General{ZSTD,Variable}` | `Variable` |
| bool | `Flat(1)` | `Flat(1)` | `Flat(1)` |
| FixedSizeList<f32,4>, list<int32> | ignored | ignored | ignored |

Unknown values: `brotli` and `LZ4` are refused on int, float, bool and string columns (`Unknown compression scheme`), accepted and ignored on fixed-size-list and list. `compression-level` alone changes nothing; with `compression=zstd` a number is the level (`30` is accepted), and a non-number is accepted and ignored (default level). lz4 ignores the level.

### W1g measured (2026-10-29, pylance 12.0.0, 50,000 rows, one column per dataset)

Source for the thresholds: lance v12.0.0 `rust/lance-encoding/src/encodings/logical/primitive.rs` (`should_dictionary_encode`:
divisor default 2, size ratio default 0.8, min 100 rows, cap 100,000 distinct, a 4,096-row sample with unique ratio
>= 0.98 rejected and projected cardinality `unique_ratio * rows` above `rows / divisor` rejected) and
`rust/lance-encoding/src/compression.rs` (`rle_is_applicable`: with `rle-threshold` set, RLE only when
`run_count < rows * threshold`; without it, the size model alone).

| column, option | pylance 12 layout | nanolance after W1g |
|---|---|---|
| int64 runs of 2, default / `rle-threshold=1` | `Rle{Flat(64),Flat(8)}` | `InlineBitpacking(64)` (default unchanged); 25,000 runs do not fit one miniblock, so no RLE: a gap |
| int64 runs of 5, `rle-threshold=0` / `0.1` | `InlineBitpacking(32)` + `dict=Bitpacked(64)x10000` | `InlineBitpacking(64)`: the RLE is off as set, the dictionary is missing (no integer dictionary) |
| int64 runs of 5, `rle-threshold=0.5` / default | `Rle{Flat(32),Flat(8)}` + dict | no RLE: 10,000 runs do not fit one miniblock (multi-chunk RLE missing): gap |
| int64 runs of 50, default | `Rle{Flat(32),Flat(8)}` + `dict=InlineBitpacking(64)x1000` | `Rle{Flat(64)}` without dictionary: gap in the dictionary only |
| same, `rle-threshold=0` | `InlineBitpacking(32)` + dict | RLE off (asserted); dict missing |
| same, `rle-threshold=0.1` / `0.5` / `1` | RLE + dict | RLE kept (asserted) |
| 5,000-distinct strings (`unique-NNNNNNNN-`), default | `Fsst{...}` (sample projection 0.68 x 50,000 > 25,000) | `Variable` dictionary (full count 5,000 <= 25,000): a default that is not changed |
| same, `dict-divisor=1000` / `0` / `1` | FSST / FSST / `InlineBitpacking(32)` + `General{LZ4,Variable}` | FSST / (`0` reads as 1) / `InlineBitpacking(32)` + plain `Variable` (LZ4 default not adopted; `dict-values-compression=none` is the matching layout) |
| int64 1,000 values, default / `dict-divisor=1000` / `1` | dict `InlineBitpacking(64)x1000` / `InlineBitpacking(64)` / dict | no integer dictionary: gap for default and `1`; `1000` matches |
| same strings, `dict-size-ratio=0.1` (divisor 1) | `Fsst{...}` | `Fsst{...}` (asserted) |
| same, `dict-size-ratio=0.0` / `1.5` | panic `not in the range (0, 1]` | refused by name (asserted) |
| same, `dict-size-ratio=abc` | default | default (values and readability asserted; layout differs on the default, as above) |
| dict (divisor 1), `dict-values-compression=none` / `lz4` / `zstd` | `Variable` / `General{LZ4,Variable}` / `General{ZSTD,Variable}` | same (asserted); indices `InlineBitpacking(32)` in both |
| `dict-values-compression=brotli` | refused: `Unknown compression scheme: brotli` | refused by name, on any column that carries it |
| same, `dict-values-compression-level` | shape unchanged | accepted; lz4 ignores it, zstd takes it |
| float64, `compression=zstd` | `General{ZSTD,InlineBitpacking(64)}` | read (xfail removed); nanolance writes `Flat(64)` as before (writer gap, unchanged) |
| float64, `compression=lz4` | `InlineBitpacking(64)` (lz4 ignored) | unchanged |
| string dictionary, `compression=lz4` / `zstd` / `none` | `values=General{LZ4 / ZSTD,BSS{Flat(32)}}` (or `Flat(32)`), dict `General{LZ4,Variable}` | not honoured yet: the option is erased on dictionary columns (write_session.cpp), so these stay as before |
| int64 1,000 values, `compression=lz4` / `zstd` / `none` | `General{..,BSS{Flat(32)}}` / `Flat(32)` indices, dict `InlineBitpacking(64)x1000` | not honoured yet (no integer dictionary) |

Byte-identical check: `nlbench_gendata <dir> 50000` on the base commit and on this branch writes the same data-file md5
(one data file, `84b4dce7912cd80d047e779f01a143fb`); manifests and transactions differ only by their random names and
timestamps, as they always do.
