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
- **Parked (owner, 2026-10-30): `Writer` create mode onto an existing dataset.** Whether a create-mode commit
  (`commit(is_append=false)` / `nano_lance_writer_init`) on a path that already holds a dataset should fail, as
  pylance's `mode="create"` does, or keep its current behaviour. Raised in W1d (PR #83). Waits until a downstream
  project that depends on the current behaviour concludes; do not change it before then.

## Wave 1: io, arrow, encoding, file

| id | rows | what | measured | status |
|---|---|---|---|---|
| W1a | `encoding.compression.constant`, `file.version.fsl_all_null_items` | Read the `Constant` CompressiveEncoding (field 3, `message Constant { optional bytes value = 1; }`) | pylance 12 writes a fixed_size_list whose items are all null as `FullZip{values=FixedSizeList{2xConstant,validity}}`, and nanolance refuses it ("unsupported FullZip value compression") | done 2026-10-28: FullZip FSL items; left: list<fsl>, small struct<fsl>, fsl<fsl>, fsl<string> all-null (strict xfail) |
| W1b | `arrow.types.string_binary` | Accept Utf8View / BinaryView on write and store them as `string` / `binary` | pylance 12 writes `string_view` / `binary_view` as `string` / `binary`, and reads them back as those types | done 2026-10-28 |
| W1c | `io.store.storage_options` | Every `AmazonS3ConfigKey` alias, and object_store's boolean spellings | object_store 0.12.4 `aws/builder.rs`: see the brief | done 2026-10-28; left: virtual-hosted style and skip_signature (nanos3reader has no switch) |
| W1d | (test coverage) | Direct C++ tests for the modules in `tools/tests_allow.txt` that can have one | -- | done 2026-10-28: 4 direct tests; found and fixed the projected-column status; the create-mode commit question is parked (see "Not scheduled") |
| W1l | `file.version.2_0`, `file.version.2_1` | Test every modification of a 2.0 / 2.1 dataset against pylance, and document what works | measured 2026-10-29, see below | done 2026-10-29: 22 cases (2.0 and 2.1 x 11 operations) match pylance, no library change; left: nothing for these rows (writing 2.0 / 2.1 stays unscheduled, W1n) |
| W1e | `arrow.fields.primary_key`, `table.schema.primary_key` | Unenforced primary key: Arrow metadata to fields 12-15, and back | done 2026-10-29: fields 12, 13, 15 written and read; the Arrow key metadata read as pylance does; pylance's write checks; merge_insert's default key by position. Left: whether pylance writes the key into a data file's schema (not measured); a key nested in a struct as the merge_insert default |
| W1m | `file.version.2_0`, `file.version.2_1` | Which older pylance / LanceDB releases read a dataset with nanolance's 2.2 files beside 2.0 / 2.1 ones | measured 2026-10-29, see below | done: only pylance 12 and later read the mix |
| W1f | `file.structure.page_priority` | Write each page's priority as Lance does | measured 2026-10-29, see "Measured for W1f (page priority)" | done 2026-10-29: every page's `priority` is the top-level row of its first row, as pylance writes it (int64, list, string, FSL128 measured); single-page columns keep 0 |
| W1g | `encoding.config` | Honour the `lance-encoding:*` field metadata Lance honours | partial (Haiku session, 2026-10-29): done `rle-threshold`, `dict-divisor`, `dict-size-ratio` (refused by name outside (0, 1]), `dict-values-compression` (+ level); reader General over InlineBitpacking. Left (dict columns under `compression` done in W1r): integer dictionaries (pylance dictionary-encodes low-cardinality ints; nanolance has none), `bss`, `structural-encoding`, multi-chunk RLE, the C++ tests for the refusal and the decode | next |
| W1h | `encoding.compression.general.write` | LZ4 | done for the value layouts measured (LZ4 writer; `compression=lz4` on 64-bit ints and variable-width). Left: General over dictionaries under the option, float zstd layout | done |
| W1i | `encoding.layout.full_zip.write` | Fixed-width full-zip and per-value compression | measured and done 2026-10-29, see "Result for W1i" | partial (Haiku session, 2026-10-29): the 256-byte rule and `structural-encoding` for fixed-width FSL and fixed_size_binary, element validity included, layout text equal to pylance's for 21 cases. Left: the page split (pylance: one page per input batch), per-value compression of a fixed-width FullZip page, `structural-encoding=fullzip` on variable width, a bool column or a list of lists under fullzip, and a fixed-width FullZip scan that is slower than mini-block (measured) |
| W1j | `encoding.semi_structural.packed_struct`, `variable_packed_struct` | Packed structs: read pylance's first, then write them for `lance-encoding:packed` | measured 2026-10-29, see "Measured for W1j" | later, reviewer-led design: reading needs a packed-struct read plan with child fan-out (as `v20_packed_struct_sources` does for 2.0), and the VariablePackedStruct byte layout from Lance's source. Opt-in only, so behind the gaps every dataset meets |
| W1k1 | `arrow.types.fixed_size_list` (read) | Read what pylance writes: a null element inside a fixed-size list under a list or struct (also W1a's all-null list<fsl>), `fixed_size_list<bool>`, `fixed_size_list<struct>` | measured 2026-10-29, see "Measured for W1k" and "Result for W1k1" | partial 2026-10-29: null elements under a list read (and all-null list<fsl>); stopped: `fixed_size_list<bool>` and `fixed_size_list<struct>` (see "Result for W1k1") |
| W1k2 | `arrow.types.fixed_size_list`, `arrow.types.dictionary` (write) | Write them too, and dictionary columns inside structs and lists | measured 2026-10-29 | partial 2026-10-29: dictionaries under a struct or list written (int32 indices, string / binary values); list<fsl> and struct<fsl> element nulls written for rows under 256 bytes; the 256-byte-and-wider shapes stopped (nested FullZip writer missing), see "Result for W1k2"; fsl<bool> / fsl<struct> refused until W1k3 / W1k4 |
| W1k3 | `arrow.types.fixed_size_list` (read) | `fixed_size_list<bool, N>`: a bit-level item path in the mini-block chunk reader (`FixedSizeList{Nx Flat(1)}`) | "Result for W1k1" | later (rare shape; touches the shared chunk reader) |
| W1k4 | `arrow.types.fixed_size_list` (read) | `fixed_size_list<struct>`: assemble a struct child of rows x N items from the leaf columns under the fixed-size layer | "Result for W1k1" | later (refused by name now) |
| W1p | `encoding.config` | Integer and float dictionaries as Lance 12 writes them (files up to 1.9x pylance's size today) | measured 2026-10-29, see "Measured for W1p" and "Result for W1p" | partial 2026-10-29: int64, uint64, timestamp and date64 dictionaries (no nulls) write pylance's layout (all measured cases, see "Result for W1p, second pass"); W1p2 (partial, 2026-10-29): RLE over the u32 indices when the dictionary has runs (`Rle{Flat(32),Flat(8)}`, see "Result for W1p2/W1p3"), matching pylance in every measured run and sorted case, the 50,000-row cutoff included after review (Lance's raw-size budget, and RLE only when it beats bitpacking: see "W1p2 review"). W1p3 (done 2026-10-29): nullable columns, see "Result for W1p3". left: W1p4 (4,096-row RLE chunks with definition levels), float64 and 128-bit |
| W1q | `encoding.compression.rle` | RLE over more than one mini-block chunk: nanolance's RLE holds its runs in one chunk, so a column of 25,000 runs is not RLE-encoded | W1g | partial (Haiku session, 2026-10-29): plain RLE splits across mini-block chunks as pylance 12 splits them (same chunk values and bytes; read back both ways; take checked; see "Measured for W1q" and "Result for W1q"). The default cutoff is pylance's size model. Left: float64 runs (still flat), u32 control words where pylance 2.1 writes u16 (nanolance's 2.2 files), W1p2 (sorted RLE over dictionary indices) |
| W1q2 | `encoding.compression.rle` | Nullable plain RLE (no dictionary): `Rle{Flat(bpv*8),Flat(8)}` + repdef for a fixed-width column with nulls and short runs, as pylance 12 writes it (runs over the raw values of every slot, null slots included) | W1q, W1p3 | done (Haiku session, 2026-10-30): layouts equal pylance in every measured case (int64, float64, int32, uint8; runs 1, 2, 5, 50; 5% and 50% nulls; null slots 0 or holding their run value; 50,000 and 1,000,000 rows; see "Measured for W1q2" and "Result for W1q2"). The three strict xfails are removed. Left: the chunk split (1,024 rows per chunk with levels, pylance 4,096: W1p4), and the write time of runs of 2 with nulls (about 1.8x the flat write) |
| W1r | `encoding.config` | Dictionary indices under `lance-encoding:compression` (`General{codec,ByteStreamSplit{Flat(32)}}`, or `Flat(32)` for none): the reader's dictionary-index path has to unwrap them before the writer may emit them | W1g, W1h | partial (Haiku session, 2026-10-29): done for string, int64 and float64 dictionaries (reader unwraps General and byte-stream-split per index chunk; writer emits pylance's index layout under lz4 / zstd / none; the dictionary buffer unchanged; `dict-rle` keeps the option erased, not measured). Layout text equal to pylance's, read and take checked, see "W1r measured". Left: `dict-rle` under the option, and integer dictionaries written by nanolance (W1p) |
| W1s | `encoding.config` | float64 and float32 columns as Lance 12 writes them: the 64-bit dictionary, RLE over values and indices, and InlineBitpacking for values with a clear sign bit | W1p, W1p2, W1p3, W1q | partial (Haiku session, 2026-10-30): float64 and float32 layouts equal pylance in every measured case (see "Measured for W1s" and "Result for W1s"), except nullable plain RLE without a dictionary (3 cases, strict xfail; fixed by W1q2). Open: the write time of random and smooth floats is about 2.5x the flat write, over the 10% rule (the owner decides, see "Result for W1s") | next |
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

## Measured for W1q (RLE over several chunks, 2026-10-29, pylance 12.0.0, 2.1 files)

Pylance's chunk rule, fitted on 40+ files (runs of 1 to 8 and boundary cases; `build/nlance-pagelayout` for the
descriptor, the miniblock control buffer for the chunks):

- chunk rows = the largest power of two up to 4,096 whose encoding is at most 8,192 bytes; the encoding is an 8-byte
  header plus one (value, length) pair per run piece, padded to 8. The limit was pinned: 8,191 fails, 8,192 passes.
- pylance's control words are u16 (`has_large_chunk` false). The chunk payload is the same as nanolance's apart from
  nanolance's u32 buffer-size header (8 bytes more per chunk).
- default RLE choice (no `rle-threshold`): RLE when `runs * (bytes + 1) < rows * bytes`. Pinned at run ratios
  0.79 (RLE) / 0.81 (not) for int32 and 0.885 (RLE) / 0.895 (not) for int64.

| column, 50,000 rows (measured) | pylance 12 chunks: values per chunk | last chunk |
|---|---|---|
| int64 runs of 2 (default and `rle-threshold=1`) | 1,024 x 48 | 848 rows (3,824 bytes) |
| int64 runs of 3 | 2,048 x 24 | 848 rows |
| int64 runs of 4 | 2,048 x 23 | the rest |
| int64 runs of 5 | 4,096 x 12 | 848 rows (1,544 bytes) |
| int64 runs of 8 | 4,096 x 12 | 848 rows |
| int64 runs of 50 (pylance also dictionary-encodes: W1p2) | 4,096 x 12 | 848 rows |
| int32 runs of 2 | 2,048 x 23 | the rest |
| int32 runs of 3 / 8 | 4,096 x 12 | 848 rows / the rest |
| int64 runs of 2, 1,000,000 rows | 1,024 (chunk values asserted equal to pylance in `test_rle_multichunk.py`) | the rest |

The pylance descriptor for the int64 runs of 2 and runs of 5 files is `Rle{values=Flat(64),lengths=Flat(8)}` with
`buffers=2`; the other rows were checked by chunk count and bytes only. Float64 runs of 2 are pylance RLE (1,024 per
chunk), and nanolance still writes them flat (see Result).

Nanolance's pagelayout output for the same files is the same text for every int case above (asserted in
`bindings/python/tests/test_rle_multichunk.py`).

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

## W1r measured (dictionary indices under lance-encoding:compression)

pylance 12.0.0, 50,000 rows, `dict-divisor=1` on strings (1,000 distinct) so the column is a dictionary; `dict-values-compression=none`
on both sides for the layout comparison (pylance's default wraps the dictionary in LZ4; nanolance keeps the plain buffer, as W1g).

| column, option | pylance's page (`build/nlance-pagelayout`) |
|---|---|
| string, lz4 / zstd | `values=General{LZ4 / ZSTD,ByteStreamSplit{Flat(32)}}`, dict `General{LZ4,Variable}` by default |
| string, none | `values=Flat(32)` |
| int64 1,000 values, lz4 / zstd / none | indices as above (asserted by the read tests; nanolance has no integer dictionary to write: W1p) |
| float64 1,000 values, lz4 / zstd / none | indices as above (asserted by the read tests) |

Before W1r the reader classified `General{..,ByteStreamSplit}` as a plain byte-stream-split column and ignored the dictionary; the
dictionary path now unwraps it per chunk. Writer: a dictionary column keeps the option; the writer-wide `compression=True` flag
no longer overrides a column's own option. nanolance-written string dictionaries under each option match pylance's layout
(`dict-values-compression=none` on both sides), and pylance reads them.

Byte-identical without the option: `nlbench_gendata` data file md5 on the base commit and on this branch is the same
(`84b4dce7912cd80d047e779f01a143fb`).

## Measured for W1i (fixed-width FullZip)

Lance 12's rule (`rust/lance-encoding/src/encodings/logical/primitive.rs`, `is_narrow` / `prefers_miniblock` /
`prefers_fullzip`): a column is mini-block when its largest value is under 256 bytes
(`MINIBLOCK_MAX_BYTE_LENGTH_PER_VALUE`), and FullZip otherwise. `lance-encoding:structural-encoding` =
`miniblock` / `fullzip` overrides it. A requested `fullzip` that the value type cannot take (a dictionary, values
not byte-aligned, a fixed-size list without a flat fixed-width child) is an error, with the messages in that
function.

pylance 12.0.0, 2,000 rows each:

| column | bytes per value | pylance | nanolance today |
|---|---|---|---|
| fixed_size_list<float32, 16..63> | 64..252 | MiniBlock | MiniBlock |
| fixed_size_list<float32, 64> | 256 | `FullZip{values=FixedSizeList{64xFlat(32)},value_bits=2048}` | MiniBlock, 2,000 rows a page |
| fixed_size_list<float32, 1024> | 4,096 | FullZip (with `def_bits=1` when rows are null) | MiniBlock, 128 rows a page |
| fixed_size_list<float32, 4096> | 16,384 | FullZip | MiniBlock, **1 row a page** |
| fixed_size_list<int8, 768> | 768 | FullZip | MiniBlock |
| fixed_size_binary(100..200) | < 256 | MiniBlock | MiniBlock |
| fixed_size_binary(256 / 4096) | 256 / 4,096 | `FullZip{values=Flat(2048 / 32768)}` | MiniBlock |

Embeddings of 768 to 4,096 floats are therefore written very differently. nanolance's mini-block pages shrink
with the value width, down to one row per page.

## Result for W1i (2026-10-29, pylance 12.0.0, nanolance written and read)

Layouts (`nlance-pagelayout`, the first page of each column), pylance = nanolance for every case in
`bindings/python/tests/test_fullzip_fixed.py`: FSL<float32> 63 (mini-block), 64, 64 with row or item nulls,
768 (with row nulls, with item nulls), 1536, 4096; FSL<int8, 768>; fixed_size_binary 255 (mini-block), 256 (with
and without nulls), 4096; FSL<float32, 16> under `structural-encoding=fullzip` and `miniblock`; int64 under
`fullzip` (FullZip{Flat(64)}) and `miniblock`; fixed_size_binary(100) under `fullzip`.

Pylance behaviour found while matching it (not in the rule above):
- an item-validity FSL is FullZip when its values are 256 bytes (`FixedSizeList{64xFlat(32),validity}`, value_bits
  2112); 63 values with validity stay mini-block: the validity bits are not in the width;
- `structural-encoding=fullzip` on a dictionary column is ignored (MiniBlock dictionary), on a constant int64
  it is ignored (Constant); on a bool column it writes `FullZip{Flat(8)}`; on a list of two strings it crashes
  pylance 12 (`compute_stat should only be called once`, a pylance bug, nothing written);
- a fixed_size_list of bools with fullzip: `Full-zip fixed-width values must be byte aligned, got 2 bits per value`;
- a fixed_size_list of fixed_size_list with fullzip: `FullZip{FixedSizeList{2xFixedSizeList{4xFlat(32)}}}`
  (accepted; nanolance refuses it for now);
- an unknown `structural-encoding` value: `Invalid user input: Cannot determine structural encoding for field c.
  This typically indicates an invalid value of the field metadata key lance-encoding:structural-encoding`
  (nanolance uses this message).

Page split (not matched): pylance wrote 200,000 rows of FSL<float32,768> as ONE page (614 MB); the same rows in
20 input batches of 10,000 gave 20 pages of 10,000 rows. Pylance cuts a fixed-width FullZip page at its input
batch boundaries, with no byte limit measured. nanolance cuts at 1 MiB of row data (341 rows of 768 floats); see
the reviewer's measurements below for why not 8 MiB.
Only the layout text is compared, not the page count.

Performance (same machine, alternating base and new, one process per run, 200,000 rows, 1,000 random rows for take;
base = origin tree, new = this change; all six runs per width listed, so the best of three is visible):

| width | side | scan, seconds (3 runs) | take 1,000 rows, seconds (3 runs) |
|---|---|---|---|
| 768 | base | 0.163 (first, a warm-cache outlier), 0.746, 0.700 | 0.059, 0.216, 0.210 |
| 768 | new | 0.548, 0.657, 0.823 | 0.015, 0.018, 0.064 |
| 1536 | base | 0.607, 0.321, 0.341 | 0.112, 0.182, 0.077 |
| 1536 | new | 0.534, 0.734, 0.715 | 0.026, 0.054, 0.048 |

The first runs were noisy (1.2 GB files, page cache) and are kept above for the record. The reviewer then
compared both layouts with one build: the same column written as FullZip (the new default) and with
`structural-encoding=miniblock` (the old layout), 5 alternating rounds, best of 5, on a 15 GB machine.

| change | 768 floats: scan MB / FZ | 1536 floats: scan MB / FZ | take 1,000 rows MB / FZ (768, 1536) |
|---|---|---|---|
| W1i as delegated (8 MiB pages, row-by-row decode) | 0.106 / 0.141 (+33%) | 0.648 / 0.200 | 0.026 / 0.007, 0.164 / 0.010 |
| + whole-page copy in `decode_full_zip_column` | 0.099 / 0.115 (+16%) | 0.574 / 0.198 | 0.026 / 0.006, 0.181 / 0.010 |
| + 1 MiB pages (`kFullZipFixedPageBytes`) | 0.105 / **0.089** (-15%) | 0.380 / **0.191** | 0.028 / 0.008, 0.187 / 0.012 |

The whole-page copy applies to a fixed-width page with no control word (no nulls, no lists), no element
validity and no constant items: the page is its rows back to back, so it is copied in one step. The 1 MiB page
follows the writer's own earlier finding (`data_file_writer.cpp`, the list-page comment): pages of 8 MiB cost
nanolance's scan up to 2x in page-sized intermediate buffers.
pylance reads the new files faster too (best of 3, scan / take): 768 floats 0.717 / 0.035 s as mini-block, 0.571 /
0.020 s as FullZip; 1536 floats 1.675 / 0.043 s, 1.122 / 0.038 s.

Byte-identical check (narrow columns): `nlbench_gendata <out> 50000` data file md5 is equal between the base and
the new build (84b4dce7912cd80d047e779f01a143fb), and `test_write_encoding_matrix.py` passes unchanged.

## Measured for W1j (packed structs, 2026-10-29)

pylance 12.0.0, 20,000 rows. A struct field with `lance-encoding:packed` = `true` (or the legacy key `packed`):

| struct | pylance's page | nanolance reads it |
|---|---|---|
| struct<int32, float64> | `MiniBlock{values=PackedStruct}` (CompressiveEncoding 12) | no: "unsupported encoding ... (CompressiveEncoding variant 12)" |
| same, with null rows | `MiniBlock{values=PackedStruct, repdef=Bitpacked(16,Flat(1)), nullable}` | no |
| same, legacy key `packed=true` | as with `lance-encoding:packed` | no |
| struct<int32, string> | `FullZip{values=VariablePackedStruct, offset_bits=32}` (CompressiveEncoding 13) | no: "unsupported FullZip value compression" |
| struct<int32, fixed_size_list<float32,4>> | `FullZip{values=VariablePackedStruct, offset_bits=32}` | no |
| struct<int32, float64>, no option | one column per child (`a`, `b`), as nanolance writes | yes |

Lance 12 `encodings_v2_1.proto`:

```
message PackedStruct {               // fixed-width children concatenated row by row into one fixed-width block
  repeated uint64 bits_per_value = 1;  // bits each child contributes to a packed row
  CompressiveEncoding values = 2;      // the compression of the packed fixed-width values
}
message VariablePackedStruct {       // 2.2: each child value compressed on its own, then transposed row-major
  repeated FieldEncoding fields = 1;   // in struct order
  message FieldEncoding {
    CompressiveEncoding value = 1;
    oneof layout { uint64 bits_per_value = 2; uint64 bits_per_length = 3; }
  }
}
```

So W1j starts with reading: a dataset pylance wrote with a packed struct cannot be read by nanolance today.

## W1j follow-up measurements (2026-10-29, session haiku/w1j-packed)

pylance 12.0.0, data_storage_version 2.2, 20,000 rows, `lance-encoding:packed` = `true`. Layouts from `build/nlance-pagelayout`:

| struct | pylance's writes | layout |
|---|---|---|
| struct<int32, float64> | yes | `MiniBlock{values=Unknown(field 12)}` |
| same, with null rows | yes | `MiniBlock{values=Unknown(field 12), repdef=Bitpacked(16,Flat(1)), nullable}` |
| struct<int32, string> | yes | `FullZip{values=Unknown(field 13), offset_bits=32, layers=[valid-item]}` |
| struct<int32, fixed_size_list<float32,4>> | yes | `FullZip{values=Unknown(field 13), offset_bits=32, layers=[valid-item]}` |

Child types pylance accepts in a packed struct (one int32 sibling, each child in turn): int8, uint8, int16, uint16, uint32, int64, uint64, float32, string, fixed_size_list<int8,3>, fixed_size_binary(3). Refused by pylance: bool (writer panics, `TryFromIntError`), list<int32> (`Field with data type List(Int32) cannot be converted to data block`), a struct child (`Packed struct per-value compression should not be used for fixed-width-only struct`), and a nullable child (`Per-value compression not yet supported for block type Nullable`).

Not measured yet: the byte layout of `VariablePackedStruct` (field 13). The Lance 12 `packed.rs` source could not be fetched in this session (the sandbox refused the download, and the GitHub connector does not have `lance-format/lance` configured), so the reader for both forms is still open. W1j reader: not started; writer: not started.

## Measured for W1k (2026-10-29)

3,000 rows each. pylance 12.0.0 writes, then nanolance reads pylance's dataset, and writes the same table itself:

| column | pylance | nanolance reads pylance's | nanolance writes |
|---|---|---|---|
| struct<k: dictionary<string>, v: int64> | writes, reads back the dictionary | equal | refused: "dictionary-encoded column 'k' inside a struct or list is not supported yet; cast it to its value type" |
| list<dictionary<string>> | writes, reads back the dictionary | equal | refused, the same |
| fixed_size_list<bool, 4> | writes | **fails**: "unsupported fixed_size_list element type ... fixed_size_list:bool:4" | refused: element must be a fixed-width number |
| fixed_size_list<struct<x: int64>, 2> | writes | **fails**, with a misleading message ("a dictionary-encoded column cannot be read in the same batch as a skipped logical field") | refused |
| list<fixed_size_list<float32, 2>> with null elements | writes | **fails**: "a null element inside a fixed_size_list item is not read yet" | refused: "has a null element inside a row under a list or struct" |
| fixed_size_list<list<int64>, 2> | refuses ("Unsupported data type") | -- | -- |
| fixed_size_list<string, N> | panics (found in W1i) | -- | -- |

So W1k starts with reading (W1k1): three shapes pylance writes that nanolance cannot read.

## Result for W1k1 (2026-10-29, partial: one of the three shapes read)

pylance 12.0.0 writes, `nanolance.read_table`, `take` and a projection are compared with pylance's own read
(`bindings/python/tests/test_fsl_read_shapes.py`, 3,000 rows; 300,000 rows for list<fsl> with element nulls).

| shape | result |
|---|---|
| list<fixed_size_list<float32, N>> with null elements inside valid items (MiniBlock, and FullZip for N = 256) | **read**, equal to pylance (read, take, projection; multi-page) |
| list<fixed_size_list> with all items null, and all rows null | read (already); `list_fsl_all_null` and `struct_fsl_all_null` leave the strict xfail list in `test_fsl_all_null_items.py` |
| fixed_size_list<bool, 4> | **stopped, not read** (see below) |
| fixed_size_list<struct<x: int64>, 2> | **stopped, not read**; refused by name since the review: "column 'v': a fixed_size_list:struct:2 is not read yet" (other columns of the dataset still read) |

What changed: `decode_nested_fixed_list_validity` (`src/encoding/lance_column_decoder.cpp`) no longer drops or
refuses a fixed_size_list page's element validity. The bits go into the column's `item_validity` at the element
offset of each page, the representation a top-level fixed_size_list already uses. The writer's refusals are unchanged.

`nlance-pagelayout` for the shapes that are stopped:

```
fsl_bool4.lance  v (fixed_size_list:bool:4): MiniBlock{values=FixedSizeList{4xFlat(1)},rows=3000,buffers=1}
fsl_struct2.lance  v.item.x (int64): MiniBlock{values=InlineBitpacking(64),rows=6000,buffers=1}
```

- **fixed_size_list<bool, 4>.** The element is 1 bit, so the miniblock fixed_size_list path (which accepts whole-byte
  elements only, `lance_column_decoder.cpp`, the `kFixedSizeList` case) refuses it as an unsupported encoding. Reading it
  means a bit-level item path in the shared miniblock chunk reader: `decode_bool_packed_column` sizes chunks and
  pages by row count (`chunk.items`, `page.length`), which would have to scale by N, and element validity (a second buffer)
  would need the same. I stopped on that scope and did not size it.
- **fixed_size_list<struct<x: int64>, 2>.** The struct is a leaf field `v.item.x` of 6,000 items under the fixed-size
  layer. The per-row fallback in `src/scan/lance_table_reader.cpp` gives one message for its `Skip`, `FixedSizeList` and
  `Dictionary` plan kinds, and that message is the dictionary one. A working read needs the bulk assembly to build a struct
  child of `rows * N` items under a FixedSizeList. I tried refusing it by name in the plan builder; that branch was never
  reached for this column (the plan kind is not `FixedSizeList`), so I reverted it. Review: `v` has no column of its own
  (`column_index < 0`), so it took the `Skip` plan; the plan builder now refuses a fixed_size_list without a column by
  name (`test_fsl_of_struct_is_refused_by_name_and_other_columns_read`).
- **Correction to "Measured for W1k":** the all-null list<fsl> already read; only a null element inside a valid item failed.

Not started: the W1k2 writer side (both shapes are still refused on write), and C++ unit tests for the nested element
validity path (the Python tests cover it end to end).

## Result for W1k2 (2026-10-29, pylance 12.0.0, written and read back both ways)

`bindings/python/tests/test_nested_write_shapes.py` (22 tests): nanolance writes, nanolance and pylance read equal (take
included), the layout text equals pylance's (`nlance-pagelayout` on both files), and an append by nanolance to a
pylance-written dataset reads back in pylance. 3,000 and 300,000 rows.

| shape | result |
|---|---|
| `struct<k: dictionary<int32, string>, v: int64>` (10 and 1,000 distinct, with and without nulls) | **written**, layout equal |
| `list<dictionary<int32, string>>` (10 and 1,000 distinct, with and without nulls) | **written**, layout equal |
| `list<fixed_size_list<float32, 2>>` with null elements | **written**, layout equal (list and struct, 3,000 and 300,000 rows) |
| `struct<fixed_size_list<float32, 2>>` with null elements | **written**, layout equal |
| `list<fixed_size_list<float32, 256>>` and `struct<...256>` with null elements | **stopped**: refused by name (FullZip; see below) |
| `fixed_size_list<bool>`, `fixed_size_list<struct>` | refused, as before (W1k3 / W1k4) |

Layouts (pylance 12.0.0 measured; nanolance's output is equal in every row written; the leaf column is shown,
`rows=` dropped):

| case | pylance |
|---|---|
| struct<dict> no nulls | `s.k (dict:string:int32:false): MiniBlock{values=InlineBitpacking(32),dict=General{LZ4,Variable{offsets=Flat(32)}}x10}` |
| struct<dict> with nulls | `s.k ...: MiniBlock{values=InlineBitpacking(32),repdef=Bitpacked(16,Flat(1)),nullable,dict=General{LZ4,Variable{offsets=Flat(32)}}x10}` |
| list<dict> no nulls | `l.item (dict:string:int32:false): MiniBlock{values=InlineBitpacking(32),rep=Bitpacked(16,Flat(1)),dict=General{LZ4,...}x10,layers=[valid-item,valid-list]}` |
| list<dict> with nulls | `l.item ...: MiniBlock{values=InlineBitpacking(32),rep=Bitpacked(16,Flat(1)),repdef=Bitpacked(16,Flat(1)),nullable,dict=General{LZ4,...}x10,layers=[nullable-item,valid-list]}` |
| 1,000 distinct | as above, `x1000` |
| list<fsl2>, element nulls | `l.item (fixed_size_list:float:2): MiniBlock{values=FixedSizeList{2xFlat(32),validity},rep=Bitpacked(16,Flat(1)),layers=[valid-item,valid-list],buffers=2}` |
| struct<fsl2>, element nulls | `s.f (fixed_size_list:float:2): MiniBlock{values=FixedSizeList{2xFlat(32),validity},buffers=2}` |
| list<fsl256>, element nulls | `l.item (fixed_size_list:float:256): FullZip{values=FixedSizeList{256xFlat(32),validity},value_bits=8448,rep_bits=1,layers=[valid-item,valid-list]}` (not written) |
| struct<fsl256>, element nulls | `s.f ...: FullZip{values=FixedSizeList{256xFlat(32),validity},value_bits=8448,layers=[valid-item,valid-item]}` (not written) |

What changed:

- **Dictionary under a struct or list.** The refusal in `schema_mapper.cpp` is lifted for int32 indices over string or
  binary values. A nested dictionary leaf takes `write_nested_column` and `build_nested_page`: indices as
  InlineBitpacking(32), the Arrow dictionary's own entries (order and unused ones kept, as the top-level writer does)
  LZ4-framed as `General{LZ4, Variable}`. The values come from `append_dictionary_values`, the top-level dictionary
  appender. Other index types there keep the refusal.
- **Element validity under a list or struct.** `build_nested_page` writes two buffers per chunk (the element bits, then
  the values) and `FixedSizeList{N x Flat, validity}` for rows under 256 bytes. The same bits and layout as
  `write_fixed_size_list_column` uses at the top level.
- **A struct child with nulls is written nested (a fix found on the way).** Before, a struct with a nullable child and no
  struct-level nulls went to the top-level writer with flat levels. pylance's reader panics on that file ("index out of
  bounds" in `repdef.rs`), for `struct<int64>` as well as for a dictionary. Such a child now takes the nested pages.
  No other column changes.

Stopped: **FullZip under a list or struct.** pylance writes a fixed_size_list row of 256 bytes or more as FullZip
(with or without element nulls); nanolance writes FullZip for fixed-width values at the top level only
(`write_full_zip_fixed_column`), and has no nested FullZip writer (the rep and def bits per row slot). That is a new
writer, not a change to the chunk path, so the shapes keep a refusal ("...FullZip pages, which nanolance does not write
under a list or struct yet"). Measured, not fixed: `list<fixed_size_list<float32, 256>>` without nulls is FullZip in
pylance and MiniBlock in nanolance (`layers=[valid-item,valid-list]`, `rows` per page differ); `struct<fsl256>`
without nulls has `layers=[valid-item,valid-item]` in pylance and `[valid-item]` in nanolance, and nanolance's pages hold
1,024 rows where pylance's single page holds 3,000.

Also measured, not changed (top-level, so other columns): a top-level `dictionary<int32, string>` column is
`MiniBlock{values=Flat(32),dict=Variable{offsets=Flat(32)}x10}` in nanolance and
`MiniBlock{values=InlineBitpacking(32),dict=General{LZ4,Variable{offsets=Flat(32)}}x10}` in pylance (3,000 rows, 10 and 1,000
distinct). The nested dictionary above follows pylance; the top-level one is a separate gap.

nlbench check: `nlbench_gendata <dir> 50000` data file md5 `84b4dce7912cd80d047e779f01a143fb` on the base commit
(9d2b3f9) and after each of the two commits. Unchanged.

Not done: the W1k3 and W1k4 reads; nested FullZip (both fixed-size list widths, above); nested dictionaries with index
types other than int32 and with fixed-width values.

## Measured for W1p (integer and float dictionaries, 2026-10-29)

pylance 12.0.0, 50,000 rows a column; nanolance today in the last column:

| column | pylance | nanolance |
|---|---|---|
| int64, 10 / 100 distinct | `InlineBitpacking(32)` indices, `dict=General{LZ4,Flat(64)}` | `InlineBitpacking(64)` values |
| int64, 1,000 distinct (also with nulls: `x1001`) | `InlineBitpacking(32)` indices, `dict=InlineBitpacking(64)x1000` | values |
| int64, 5,000 / 10,000 / 20,000 distinct | `InlineBitpacking(64)` values (no dictionary) | the same |
| float64, 100 / 1,000 distinct | as int64 (`dict=General{LZ4,Flat(64)}` / `InlineBitpacking(64)`) | `Flat(64)` values |
| float64, 10,000 distinct | `InlineBitpacking(64)` | `Flat(64)` |
| int32, 100 / 1,000 distinct | `InlineBitpacking(32)` (never a dictionary) | the same |

The whole table: pylance 1.74 MB, nanolance 3.23 MB.

Lance 12's rule (`rust/lance-encoding/src/encodings/logical/primitive.rs`, `should_dictionary_encode`):
- only 64- and 128-bit fixed-width blocks (and variable width); never fewer than 100 values (`LANCE_ENCODING_DICT_TOO_SMALL`);
- threshold cardinality = values / `dict-divisor` (default 2), capped at 100,000 (`LANCE_ENCODING_DICT_MAX_CARDINALITY`);
- a deterministic sample: up to 4,096 values in 32 evenly spaced blocks (none if under 1,024 samples); if the
  sample's unique ratio is 0.98 or more, no dictionary; else projected cardinality = ratio x values, and above
  the threshold, no dictionary;
- then the dictionary is tried with a budget: at most `threshold` entries and an encoded size of at most
  `dict-size-ratio` (default 0.8) x the data size;
- the dictionary's values are compressed with `dict-values-compression` (default lz4) when it pays.

## Result for W1q (2026-10-29)

Written: `rle::chunk_rows` (src/encoding/values/rle.cpp), the chunked RLE writer (`write_rle_column`, one mini-block
chunk per entry, runs cut at chunk ends), the size-model cutoff and removal of the one-chunk guard in
`fixed_column_rle_plan`. Tests: `tests/encoding/test_rle.cpp::test_chunk_rows`,
`bindings/python/tests/test_rle_multichunk.py`.

- Layout text equal to pylance's for every measured int case, and the chunk values equal; the chunk bytes differ by
  nanolance's u32 header (8 bytes per chunk). Both libraries read both files; `take` checked.
- Kept u32 control words, not pylance's u16: nanolance writes data storage version 2.2, and the note on
  `control_buffer_for` records that stock Lance's 2.2 validator refuses u16 mini-block pages. The pagelayout text
  does not show this flag, so it is not part of the text comparison. Decision for the owner: u16 would match
  pylance's bytes exactly but only in a 2.1 file.
- Left: float64 runs stay flat (the float path has its own decision, not changed). W1p2 (dictionary indices) is in "Result for W1p2/W1p3".

Review (write time, 1,000,000 rows, one int64 column, best of 5; pylance 12 for the same column in brackets):

| column | before W1q | W1q as delivered | after review | size before / after |
|---|---|---|---|---|
| runs of 2 | 14.8 ms (InlineBitpacking) | 212 ms | 32.4 ms [29.3 ms] | 5.02 MB / 4.52 MB |
| runs of 50 | 72.1 ms (W1p dictionary) | 5.2 ms | 4.1 ms | 1.79 MB / 0.19 MB |
| random | 13.7 ms | 13.3 ms | 12.6 ms [35.8 ms] | unchanged |

The 212 ms was `miniblock::append_chunk` reserving exactly each chunk's bytes, so every chunk copied the page
(quadratic; harmless while RLE pages had one chunk). It now grows geometrically. `rle::chunk_rows` walks each
chunk once instead of once per candidate size (same chunks; `test_rle` unchanged). Control words stay u32: nanolance
writes 2.2. `nlbench_gendata` 50,000: data file md5 unchanged.
- md5 of `nlbench_gendata <dir> 50000` data file: `84b4dce7912cd80d047e779f01a143fb`, unchanged.

## Result for W1p (2026-10-29, stopped: no writer change)

Nothing was written: the 64-bit dictionary needs four things the brief does not settle, so this session stopped
(the brief's stop rule). What is settled, measured with pylance 12 (default file version 2.2, `build/nlance-pagelayout`):

**The decision (whether to dictionary-encode) is Lance's rule as written above.** A simulation of it (sample of 32
blocks x 128 values, unique ratio >= 0.98 rejects, projected cardinality = ratio x rows against min(rows / divisor, 100,000))
gives pylance's choice on all 10 measured shuffled, non-null int64 cases (50,000 and 1,000,000 rows; 10 / 100 / 1,000 /
5,000 / 25,000 distinct). Dictionary at 50,000 rows: 10, 100, 1,000 distinct; none at 5,000 (sample 0.68 -> 34,000 > 25,000)
and 25,000. At 1,000,000 rows: 10 and 100 only (1,000 distinct projects to 240,000 > 100,000, so no dictionary).

**Measured shapes** (int64, 50,000 rows unless stated; `w` = value width, the random values are `w`-bit):

| case | pylance layout | pylance bytes | nanolance bytes (unchanged) |
|---|---|---|---|
| 10 / 100 / 1,000 distinct, 40-bit | `InlineBitpacking(32)` + `dict=General{LZ4,Flat(64)}` / same / `dict=InlineBitpacking(64)x1000` | 26,548 / 46,069 / 69,230 | 252,296 |
| 5,000 / 25,000 distinct | `InlineBitpacking(64)` | 252,192 each | 252,296 |
| 1,000,000 rows, 10 distinct | `InlineBitpacking(32)` + `dict=General{LZ4,Flat(64)}x10` | 520,244 | 5,023,432 |
| 1,000,000 rows, 1,000 / 5,000 / 25,000 distinct | `InlineBitpacking(64)` | 5,022,113 each | 5,023,432 |
| 10 distinct, nulls (10%) | `InlineBitpacking(32)`, `repdef=Bitpacked(16,Flat(1))`, `nullable`, `dict=General{LZ4,Flat(64)}x11` | 32,832 | 258,632 |
| 1,000 distinct, nulls | same repdef, `dict=InlineBitpacking(64)x1001` | 75,514 | 258,632 |
| sorted, 10 / 1,000 / 5,000 distinct | `Rle{values=Flat(32),lengths=Flat(8)}` + the dictionary (`General{LZ4,Flat(64)}x10`, `InlineBitpacking(64)x1000`, `Bitpacked(64,Flat(40))x4999`) | 1,850 / 10,868 / 51,261 | 2,184 / 9,352 / 252,296 |

Dictionary values, by width and distinct count (50,000 rows, shuffled; the same for 16-, 24-, 32-, 40- and 63-bit values):

| values | 8 | 16 (10-200) | 16 (300-1,000) | 24 | 32 | 40 (10-500) | 40 (700-1,000) | 63 (10-1,000) | any width, 2,000 |
|---|---|---|---|---|---|---|---|---|---|
| dictionary at 500 entries | `InlineBitpacking(64)` | `General{LZ4,Flat(64)}` | `InlineBitpacking(64)` | `InlineBitpacking(64)` | `General{LZ4,Flat(64)}` | `General{LZ4,Flat(64)}` | `InlineBitpacking(64)` | `General{LZ4,Flat(64)}` | `Bitpacked(64,Flat(w))` |

**Why it stopped:**
1. The choice between `General{LZ4,Flat(64)}` and `InlineBitpacking(64)` for the dictionary values is not a size rule. LZ4 is
   larger than the bitpacked bytes in every measured case (`lz4` on the raw dictionary, measured offline), yet pylance
   picks LZ4 for 40-bit x 500 and InlineBitpacking for 40-bit x 700. No threshold on bits, count, bits x count or raw
   bytes separates the table above. The rule is in Lance's source, which is not in this environment.
2. Above 1,024 entries pylance writes `Bitpacked(64,Flat(w))` (one width for the whole dictionary, out of line). The
   reader reads that shape; the writer does not write it.
3. A nullable column gets a null entry in the dictionary (`x11`, `x1001`) and a `repdef` layer. nanolance keeps dictionaries
   off nullable columns (`!column_has_nulls` in `encode_column`), so this is a new combined page, not a generalisation.
4. A sorted column is RLE over the dictionary indices (`Rle{Flat(32),Flat(8)}` + dict). nanolance's `dict-rle` path is the
   string version; the fixed-width version is a new shape.

Items 2 to 4 need writer shapes beyond generalising `write_dict_column` from strings to fixed width, which the brief's stop
rule names. Item 1 needs the source. Next step: read `should_dictionary_encode` and the dictionary-values compressor
choice in Lance's `primitive.rs` / `compression.rs` (a rust checkout, not pylance's wheel), then implement items 1-4 as one change.

Byte-identical: no writer change in this session, so `nlbench_gendata` output is unchanged.

### W1p: how Lance stores the dictionary values (reviewer, from the Lance 12 source)

The dictionary block goes through `compress_required_block` with a synthetic field whose `compression` is
`dict-values-compression` (default `lz4`). For a fixed-width block, `try_bitpack_for_block`
(`rust/lance-encoding/src/compression.rs`) runs first:

- only 8/16/32/64-bit values;
- "too small": a single bit-width (one chunk, i.e. at most 1,024 values) whose `InlineBitpacking::min_size_bytes(width)`
  -- a whole 1,024-value chunk, 1024 x width / 8 bytes -- is at least the block's raw size. Then no bitpacking;
- otherwise `InlineBitpacking(bits)` when there are at most 1,024 values, else `OutOfLineBitpacking(max_width, bits)`.

When bitpacking is not taken, the general codec applies (LZ4 by default; `none` keeps `Flat`). This predicts every
case measured: 100 x 40-bit = 800 B < 5,120 B, so LZ4; 1,000 x 40-bit = 8,000 B > 5,000 B, so InlineBitpacking;
500 x 8-bit (1,024 B < 4,000 B) and 24-bit (3,072 B) bitpack, 32-bit (4,096 B) and 40-bit (5,120 B) are LZ4;
700 x 40-bit = 5,600 B > 5,120 B, so InlineBitpacking. It is not a size comparison with LZ4's output.

nanolance reads `OutOfLineBitpacking` (`bitpack::decode_out_of_line`) but has no encoder for it: one width for every
1,024-value chunk, no per-chunk header -- the inverse of the decoder.

## Result for W1p, second pass (2026-10-29, int64 / uint64 / timestamp / date64, no nulls)

Written: the decision (`encoding_choice::fixed_column_dict_beneficial`, Lance's rule as in "W1p: how Lance stores the
dictionary values"), the dictionary values (`InlineBitpacking(64)` for one block, `OutOfLineBitpacking(64, Flat(w))` above
1,024 entries, `General{LZ4,Flat(64)}` when bitpacking does not pay, `Flat(64)` under `dict-values-compression=none`),
and the indices (`InlineBitpacking(32)`, as the string dictionary). `bitpack::encode_out_of_line` is its inverse of
`decode_out_of_line`. A column with its own `lance-encoding:compression`, and any nullable column, keeps its layout.

Layout equal to pylance (`bindings/python/tests/test_dict_64bit.py`, 15 cases, values and take read back by both):

| case | pylance | nanolance (now) |
|---|---|---|
| 50,000 rows, 40-bit, 10 / 100 distinct | `InlineBitpacking(32)` + `General{LZ4,Flat(64)}` | same |
| 50,000 rows, 1,000 distinct | `dict=InlineBitpacking(64)x1000` | same |
| 50,000 rows, 2,000 distinct (40 / 63-bit) | `dict=Bitpacked(64,Flat(40 / 63))x2000` | same |
| 50,000 rows, 500 distinct, widths 8 / 16 / 24 / 32 / 40 | Inline (8, 16 at 300+, 24) or LZ4 (16 at 200, 32, 40) | same, all |
| 50,000 rows, 5,000 distinct | `InlineBitpacking(64)`, no dictionary | same |
| 1,000,000 rows, 10 / 100 distinct | `General{LZ4,Flat(64)}` | same |
| 1,000,000 rows, 1,000 / 5,000 / 25,000 distinct | no dictionary | no dictionary |

Sizes (data file, bytes; "before" is the 2,184 / 252,296 flat or the same flat page as before this change):

| case | pylance | nanolance before | nanolance now |
|---|---|---|---|
| 50,000 rows, 10 distinct | 26,548 | 252,296 | 26,632 |
| 50,000 rows, 100 distinct | 46,069 | 252,296 | 46,152 |
| 50,000 rows, 1,000 distinct | 69,230 | 252,296 | 69,320 |
| 1,000,000 rows, 10 distinct | 520,244 | 5,023,432 | 520,328 |
| 1,000,000 rows, 100 distinct | 896,117 | (flat, not measured separately) | 896,200 |

Write time, 1,000,000 rows x 4 int64 columns (cardinalities 10, 100, 5,000, 25,000), best of 3 (`/tmp/w1p/timeit.py`):
0.131 s now. The before figure needs a second build of the base commit and was not measured in this pass.

Reviewer A/B (1,000,000 rows, int64 columns at 10 / 100 / 5,000 / 25,000 distinct plus a timestamp, best of 3):

| build | write | scan | data size |
|---|---|---|---|
| before W1p | 0.069 s | 0.007 s | 25.5 MB |
| W1p as delivered (`unordered_map` index) | 0.160 s | 0.012 s | 12.4 MB |
| W1p + open-addressing index (committed) | 0.148 s | 0.011 s | 12.4 MB |

Per column: a rejected column costs 12 ms (the sample); a dictionary column 30-46 ms, against pylance's 34-36 ms for the
same columns. The write is ~2x a flat write, on par with pylance; the file is half the size (10x for low cardinality).

Byte-identical: `nlbench_gendata` at 50,000 rows writes the same data file (`84b4dce7912cd80d047e779f01a143fb`), so no
column in that benchmark is a 64-bit pylance dictionary.

Not done, and why:
- **Nullable (W1p3).** pylance adds a null entry to the dictionary (`x11`, `x1001`) and writes `repdef=Bitpacked(16,Flat(1))`.
  The decision refuses a column with nulls, so those layouts are unchanged.
- **Sorted (W1p2).** pylance writes RLE over the dictionary indices (`Rle{Flat(32),Flat(8)}` + dictionary). Sorted int64 columns still
  take the existing RLE path (no dictionary). Sorted columns at 5,000 distinct now take the new plain dictionary
  (98,952 bytes, against pylance's 51,261 RLE + dictionary and 252,296 before): valid (pylance and nanolance read it), smaller, but not
  pylance's layout until W1p2 lands.
- **Float64 and 128-bit.** Not written. Float64 would need its own RLE decision (pylance RLEs floats too), so it stays flat for now.
- **Edge refused by the decision:** an out-of-line dictionary whose tail is exactly as large as its packed size (the reader's
  decode is ambiguous there): nanolance writes no dictionary for that column (pylance's choice there is unknown).

## Measured for W1p2/W1p3 (2026-10-29, pylance 12.0.0, nanolance at 125ea1b, `build/nlance-pagelayout`)

int64 columns, data file layout text (pylance, then nanolance before W1p2). Runs: each run has its own random 40-bit
value, so the distinct count is rows / run length. Sorted: a sorted column of values drawn from `card` distinct values.

| column | pylance | nanolance before | pylance bytes / nanolance bytes |
|---|---|---|---|
| runs 2, 50,000 | `Rle{Flat(64),Flat(8)}`, no dict (25,000 distinct) | the same | 226,818 / 226,755 |
| runs 5, 50,000 | `Rle{Flat(32),Flat(8)}` + `dict=Bitpacked(64,Flat(40))x10000` | `Rle{Flat(64),Flat(8)}` | 102,421 / 91,075 |
| runs 50, 50,000 | `Rle{Flat(32),Flat(8)}` + `dict=InlineBitpacking(64)x1000` | `Rle{Flat(64),Flat(8)}` | 11,465 / 10,113 |
| runs 500, 50,000 | `Rle{Flat(32),Flat(8)}` + `dict=General{LZ4,Flat(64)}x100` | `Rle{Flat(64),Flat(8)}` | 3,088 / 2,945 |
| runs 2 / 5, 1,000,000 | `Rle{Flat(64),Flat(8)}`, no dict | the same | 4,520,326 / 4,520,261; 1,808,514 / 1,808,451 |
| runs 50, 1,000,000 | `Rle{Flat(32),Flat(8)}` + `dict=Bitpacked(64,Flat(40))x20000` | `Rle{Flat(64),Flat(8)}` | 210,711 / 189,058 |
| runs 500, 1,000,000 | `Rle{Flat(32),Flat(8)}` + `dict=Bitpacked(64,Flat(40))x2000` | `Rle{Flat(64),Flat(8)}` | 39,445 / 45,442 |
| sorted 1,000, 50,000 | `Rle{Flat(32),Flat(8)}` + `dict=InlineBitpacking(64)x1000` | `Rle{Flat(64),Flat(8)}` | 11,401 / 10,113 |
| sorted 5,000, 50,000 | `Rle{Flat(32),Flat(8)}` + `dict=Bitpacked(64,Flat(40))x4999` | `Rle{Flat(64),Flat(8)}` | 51,797 / 46,019 |
| sorted 25,000, 50,000 | `Rle{Flat(64),Flat(8)}`, no dict | the same | 195,841 / 195,715 |
| sorted 1,000 / 5,000 / 25,000, 1,000,000 | `Rle{Flat(32),Flat(8)}` + dict (`InlineBitpacking(64)x1000`, `Bitpacked(64,Flat(40))x5000` / `x25000`) | `Rle{Flat(64),Flat(8)}` | 34,960 / 46,851; 58,711 / 53,507; 259,416 / 233,475 |

Nullable int64, 50,000 rows, 5% and 50% nulls (`measure.py null`): pylance `InlineBitpacking(32)`,
`repdef=Bitpacked(16,Flat(1))`, `nullable`, and a dictionary whose count is the distinct count plus one (`x11` for 10
distinct, `x1001` for 1,000). The nanolance column is flat-bitpacked (`InlineBitpacking(64)`). Pylance's sizes: 33,368 (10 distinct, both
rates) and 76,050 (1,000 distinct, both rates). Varying the null slots' underlying value (`nullvar.py`): when the
slot holds an existing value the count is the distinct count; when it holds 0 or a fresh value the count gains one.
So pylance's null entry is the underlying value of the null slots, added as a dictionary entry like any value.

### Rule (W1p2, from the table above)

1. Dictionary decision first, as now (`fixed_column_dict_beneficial`). Pylance takes the dictionary only when the sample
   allows it: sorted columns pass (a block of consecutive sorted values has few distinct values), shuffled ones
   do not at 1,000,000 rows (25,000 distinct is refused, as measured for W1p). Runs of 2 at 50,000 rows are refused.
2. If a dictionary is taken, the u32 indices are RLE'd when `runs * (4 + 1) < rows * 4` (the size model at 4-byte values,
   runs split at 255). Measured: every dictionary case above is RLE'd (runs and sorted), and every shuffled dictionary case
   is not (it has no runs). The chunking is `rle::chunk_rows` at 4-byte values.
3. If no dictionary is taken, the values are RLE'd as W1q (`Rle{Flat(64),Flat(8)}`) under the 8/9 cutoff.

Consequence measured in the table: where pylance writes a dictionary over runs, its file is larger than nanolance's
flat-value RLE (for example runs 5 at 50,000: 102,421 against 91,075). Matching pylance's layout makes these files larger.

## Result for W1p3 (2026-10-29, reviewer: nullable 64-bit dictionaries)

The Haiku session stopped twice before changing code; the reviewer finished it from its notes. Lance's
`dictionary_encode` hashes every slot's raw value, a null slot's too, and so does nanolance now: the value under the
nulls is an entry when no valid row holds it (`x11` for 10 values over zeros, `x10` over an existing value). The
decision's null refusal and the writer's `!column_has_nulls` gate are gone; `write_dict_fixed_column` adds each
chunk's definition levels (`repdef=Bitpacked(16,Flat(1))`, `nullable`), for bitpacked and for RLE'd indices
(`encode_rle_chunks` takes the validity).

| case (50,000 rows unless noted) | pylance 12 | nanolance |
|---|---|---|
| 10 / 1,000 distinct, 5% / 50% nulls, zeros or an existing value under them | `InlineBitpacking(32)` + repdef + `dict=...x11 / x10 / x1001 / x1000` | same |
| sorted 1,000 distinct, runs of 50, 5% nulls | `Rle{Flat(32),Flat(8)}` + repdef + dict | same |
| 10 distinct, 5% nulls, 1,000,000 rows | as above | same |

`bindings/python/tests/test_dict_64bit_nulls.py` (layout, both readers, take, the entry under the nulls, an append
to a pylance dataset); `test_encoding_choice.cpp` (the entry count with nulls). `nlbench_gendata` md5 unchanged.

Write, 1,000,000 rows, best of 5 (pylance in brackets): 10 distinct 5% nulls 19.5 ms, 0.65 MB [44.1 ms, 0.65 MB; it was
5.1 MB flat before]; sorted 5,000 5% nulls 25.4 ms, 0.67 MB [50.0 ms, 0.65 MB]; random 5% nulls 26.3 ms [40.0 ms].

**W1p4 (left):** pylance's RLE chunks with definition levels hold 4,096 rows; nanolance's hold 1,024, because
`pack_definition_levels` packs one FastLanes block. Valid and read by both; the layout text is equal, the chunk split
is not. It needs the definition levels packed over several blocks, with Lance's tail rule per block.

## W1p2 review (2026-10-29): the cutoff, from the Lance 12 source

The open cutoff below was two rules of Lance's that nanolance did not have (lance v12.0.0
`rust/lance-encoding/src/encodings/logical/primitive/dict.rs` and `compression.rs`):

1. **The dictionary budget counts raw bytes.** `dictionary_encode` refuses once `entries * 8 + rows * 4` exceeds
   `floor(rows * 8 * dict-size-ratio)`, whatever the encodings would make of it. At 50,000 rows: sorted 15,000
   (14,485 entries, 315,880 B) is taken, sorted 20,000 (18,353, 346,824 B) and runs of 2 (25,000, 400,000 B) are
   refused. nanolance compared encoded sizes; it now uses Lance's sum.
2. **RLE must also beat bitpacking.** `rle_beats_raw_and_bitpacking`: RLE is taken only when its size is below the raw
   size and below InlineBitpacking's estimate (one word per 1,024 values plus each block at its own bit width, when
   that is below raw). A sorted column of small values (sorted 20,000 at 50,000 rows: 15-bit values) is bitpacked.
   W1q's rule lacked the second half.

With both, 50 of 52 int64 shapes (runs 2-500 of 40-bit and full-range values, sorted 1,000-25,000 of small and
large values, random, shuffled 2,000; 50,000 and 1,000,000 rows) have pylance's layout text; the two others differ only
in page row counts (random and shuffled at 1,000,000 rows: a page-size difference that predates this work). The
strict xfails are gone. `test_rle_multichunk.py` now compares with pylance's 2.2 files (it compared with 2.1, which
writes no 64-bit dictionary): chunk bytes and values equal.

Decided: pylance's layout over nanolance's flat RLE for runs of 50 and sorted columns (12% and 10% larger, as
pylance's own files are), for one predictable layout. Write times, 1,000,000 rows, best of 5 (pylance in brackets):
runs of 50 15-17 ms [30.1], sorted 5,000 distinct 12 ms [21.8], runs of 2 37-40 ms [32.1], random 13 ms [35.8].

## Result for W1p2/W1p3 (2026-10-29, stage 1 partial, stage 2 not started)

Written (stage 1): the dictionary decision now comes before the RLE decision for 64-bit columns, as pylance does. A
dictionary whose indices have runs (the W1q size model at 4-byte values, `runs * 5 < rows * 4`, split at 255) is written
as `Rle{Flat(32),Flat(8)}` over the indices, in the W1q chunks (`encode_rle_chunks`, factored out of `write_rle_column`
so both writers share it: no second RLE writer). Pieces: `StructuralDictPlan::index_rle` / `index_runs`
(include/nanolance/column_values.hpp), `rle_runs_beneficial` (the body of `fixed_column_rle_plan`, now shared),
`fixed_column_dict_beneficial` (sets the index RLE), `write_dict_fixed_column`, and the order in
`src/dataset/write_session.cpp`.

Layout text against pylance 12 (`bindings/python/tests/test_dict_64bit_rle.py`: 15 measured cases, layout equal, values
and take read both ways; append to a pylance-written dataset; 3 cutoff cases marked strict xfail):

| case | pylance | nanolance now |
|---|---|---|
| runs 5 / 50 / 500, 50,000 rows | `Rle{Flat(32),Flat(8)}` + dict | same |
| runs 50 / 500, 1,000,000 rows | same | same |
| runs 2 / 5, 1,000,000 rows | `Rle{Flat(64),Flat(8)}`, no dict | same |
| sorted 1,000 / 2,000 / 10,000 / 15,000, 50,000 rows | `Rle{Flat(32),Flat(8)}` + dict | same |
| sorted 1,000 / 5,000 / 25,000, 1,000,000 rows | same | same |
| sorted 100,000, 1,000,000 rows | `Rle{Flat(64),Flat(8)}`, no dict | same |
| runs 2, 50,000 rows | `Rle{Flat(64),Flat(8)}`, no dict | `Rle{Flat(32),Flat(8)}` + dict (25,000 entries): MISMATCH |
| sorted 20,000 / 25,000, 50,000 rows | `Rle{Flat(64),Flat(8)}`, no dict | `Rle{Flat(32),Flat(8)}` + dict: MISMATCH |

The open point: the dictionary decision (`fixed_column_dict_beneficial`, the W1p sample rule) is unchanged, and it
disagrees with pylance at the 50,000-row cutoff for sorted columns. Measured: pylance takes a dictionary for sorted
columns up to 15,000 distinct (14,485 actual) and refuses from 20,000 (18,353 actual); at 1,000,000 rows up to 50,000
distinct and refuses 100,000. The sample rule projects sorted 20,000 to about 20,000 (unique ratio 0.40) and
shuffled 2,000 to about 21,000 (taken by pylance). No single projected-cardinality threshold separates both
(it would have to refuse 20,000 sorted and accept 21,000 shuffled), and a boundary tweak (rows / 3, or `>=` for runs 2)
fits some cases and not others. So the cutoff is not inferred: STOP. The two MISMATCH rows are strict xfails in
`test_dict_64bit_rle.py::test_cutoff_layout_matches_pylance`. Next: pylance's `should_dictionary_encode` source for the
sorted case (the sample for fixed-width, or the budget); without it the cutoff stays as it is.

Performance (1,000,000 rows, int64, nanolance, best of 5, two runs each; `bench` in scratch; base = before this change):

| column | before (write, data size) | after (write, data size) |
|---|---|---|
| runs of 50 | 5.8 / 8.1 ms, 189,059 B | 15.7 / 17.1 ms, 211,395 B |
| sorted, 5,000 distinct | 4.4 / 6.7 ms, 53,507 B | 15.3 / 15.9 ms, 58,627 B |
| random 62-bit | 12.5 / 20.6 ms, 7,776,261 B | 14.1 / 14.0 ms, 7,776,261 B |
| 10 distinct, 5% nulls | 17.7 / 29.1 ms, 5,148,933 B | 18.1 / 19.7 ms, 5,148,933 B |

The first two rows break the performance rule: the write is slower and the file bigger. That is the cost of pylance's
layout there (pylance's own files are also bigger: 210,711 and 58,711 B at the same shapes, against nanolance's flat
RLE). Matching pylance and the performance rule conflict on these shapes; the owner decides which to keep. The
random and null rows are the same code path (noise within the two runs; the 64-bit dictionary sample now runs before
the RLE scan, which is cheap on random data). Byte-identical: `nlbench_gendata 50000` data file md5
`84b4dce7912cd80d047e779f01a143fb` before and after.

Not done (stage 2, W1p3): nullable 64-bit dictionaries. Measured above: pylance adds the null slots' underlying value
as a dictionary entry (`x11`, `x1001`; a null slot holding 0 or a fresh value adds one entry, an existing value adds none),
with `repdef=Bitpacked(16,Flat(1))` and `nullable`. The position of that entry in the dictionary (first appearance,
as for every value) is assumed, not byte-checked. Not implemented: the decision refuses nulls, and the nullable
dictionary path needs the definition-level layer over the dictionary indices (a new combined page).

## Measured for W1s (2026-10-30, float64 and float32 layouts, pylance 12.0.0 at 2.2, `build/nlance-pagelayout`)

Before this change. Scripts in scratch (`measure.py`, one pylance and one nanolance dataset per case). Values: `random` =
uniform in [0, 1); `distinctN` = N random values, drawn at random; `runsN` = runs of N; `sortedN` = sorted draws of N
distinct values. 5% nulls where marked. Layout text, `MiniBlock` and the buffer count removed.

**Rule, from the saved Lance 12 source.** The float path is the int64 path on raw bits. Nothing in the dictionary or RLE
decisions tests for a float type; only byte-stream-split does, and it needs `lance-encoding:compression` (see below).

1. **Dictionary, float64 only.** `primitive.rs` `should_dictionary_encode` (6066-6130): a 64-bit fixed-width block is a
   candidate, floats included; 32-bit is excluded (`bits_per_value != 64 && != 128` returns None, 6078-6087). The 2.2
   dense path uses `Include64Bit` (`DenseU32PrimitiveEncoding`, 6995-7025). Entries are raw u64 bits
   (`primitive_dict.rs` 296-304, `fixed_width_data_block.data.borrow_to_typed_slice::<u64>()`), so NaN and -0.0 are
   distinct entries. The sample rule (`sample_unique_ratio`, 6191-) and the budget (`dictionary_encode` raw sum, W1p2
   review) are type-free. Dictionary values take the int64 path: `InlineBitpacking(64)` when the bit width is below 64,
   `General{LZ4,Flat(64)}` for 10 values, `Bitpacked(64,Flat(62))` for 5,000 or more.
2. **RLE over values, float64 and float32.** `rle_is_applicable` (compression.rs 174-197) accepts any 8/16/32/64-bit
   FixedWidth block; `rle_beats_raw_and_bitpacking` (200-218) decides. Float32 is RLE'd (`Rle{Flat(32),Flat(8)}`), not
   dictionary-encoded.
3. **RLE over dictionary indices.** As int64 (W1p2): `Rle{Flat(32),Flat(8)}` over u32 indices when the dictionary's
   indices have runs, with the W1p2 budget.
4. **Bitpacking of values (the rule that makes random floats bitpacked).** `try_bitpack_for_mini_block` (372-385) returns
   `InlineBitpacking(bits)` when `estimate_inline_bitpacking_bytes` (388-420) is below the raw size. That estimate uses the
   bit width of the raw bits: uniform [0, 1) floats have a zero sign bit, so their width is 63 and bitpacking wins
   (`InlineBitpacking(64)`). Signed floats (sign bit used) need 64 bits and stay `Flat(64)` (measured below).
5. **Byte-stream-split.** `try_bss_for_mini_block` (154-171) only when a compression is set (`lance-encoding:compression`
   or the writer's compression flag). Not part of the plain layout.

Measured, pylance 12 (nanolance = before W1s, all flat):

| case | rows | pylance layout (float64 unless marked) |
|---|---|---|
| random | 50,000 / 1,000,000 | `InlineBitpacking(64)` |
| random, 5% nulls | 50,000 / 1,000,000 | `InlineBitpacking(64)`, `repdef=Bitpacked(16,Flat(1)),nullable` |
| signed normal (sign bit used) | 50,000 / 1,000,000 | `Flat(64)` |
| 10 distinct | 50,000 / 1,000,000 | `InlineBitpacking(32)` + `dict=General{LZ4,Flat(64)}x10` |
| 10 distinct, 5% nulls | 50,000 / 1,000,000 | same + `repdef`, `nullable` |
| 1,000 distinct | 50,000 | `InlineBitpacking(32)` + `dict=InlineBitpacking(64)x1000` |
| 1,000 distinct | 1,000,000 | `InlineBitpacking(64)`, no dict (sample projects above the cap, as for int64) |
| 1,000 distinct, 5% nulls | 50,000 / 1,000,000 | `InlineBitpacking(32)` + dict x1000 / `InlineBitpacking(64)`, no dict; `repdef` |
| 5,000 distinct | 50,000 / 1,000,000 | `InlineBitpacking(64)`, no dict |
| 5,000 distinct, 5% nulls | 50,000 / 1,000,000 | same + `repdef`, `nullable` |
| runs of 2 | 50,000 / 1,000,000 | `Rle{Flat(64),Flat(8)}`, no dict (also with nulls) |
| runs of 5 | 50,000 | `Rle{Flat(32),Flat(8)}` + `dict=Bitpacked(64,Flat(62))x10000` |
| runs of 5 | 1,000,000 | `Rle{Flat(64),Flat(8)}`, no dict |
| runs of 50 | 50,000 / 1,000,000 | `Rle{Flat(32),Flat(8)}` + `dict=InlineBitpacking(64)x1000` / `Bitpacked(64,Flat(62))x20000` |
| runs of 50, 5% nulls | 50,000 / 1,000,000 | same + `repdef`, `nullable` |
| sorted 1,000 distinct | 50,000 / 1,000,000 | `Rle{Flat(32),Flat(8)}` + `dict=InlineBitpacking(64)x1000` |
| sorted 1,000 distinct, 5% nulls | 50,000 / 1,000,000 | same + `repdef`, `nullable` |
| sorted 5,000 distinct | 50,000 / 1,000,000 | `Rle{Flat(32),Flat(8)}` + `dict=Bitpacked(64,Flat(62))x4999 / x5000` |
| sorted 5,000 distinct, 5% nulls | 50,000 / 1,000,000 | same + `repdef`, `nullable` |
| float32 random | 50,000 / 1,000,000 | `InlineBitpacking(32)` (no dictionary: 32-bit) |
| float32 runs of 2 | 50,000 / 1,000,000 | `Rle{Flat(32),Flat(8)}` |
| float32 runs of 50 | 50,000 / 1,000,000 | `Rle{Flat(32),Flat(8)}` |

Nanolance before W1s: every float64 and float32 case above is `Flat(64)` / `Flat(32)`.

Derived rule for the implementation: a float64 column takes the int64 decisions, in order (dictionary, then RLE over
values, then bitpacking, then flat), on its raw 8-byte values; a float32 column takes the RLE and bitpacking decisions
on its raw 4-byte values. Under `lance-encoding:compression` the float keeps its own layout (as the int path does).

## W1s review (2026-10-30)

Decided: pylance's layout (InlineBitpacking for floats with a clear sign bit), as for the integer shapes. Profiled
the random-float write (1M rows): the time went to the file writes (the default 8 KiB `filebuf` sent every 8 KiB
mini-block chunk to the file as its own call), `bitpack::bit_width` (a runtime-width memcpy per value, not
vectorized) and the dictionary sample. The local sink now has a 256 KiB buffer and `bit_width` a typed loop per
width (every bitpacked column, integers too). Best of 7, 1M rows, nanolance [pylance 12]:

| column | before W1s | W1s as delivered | after review | size |
|---|---|---|---|---|
| float64 random [0, 1) | 3.9 ms | 10.0 ms | 9.7 ms [15.9] | 7.78 MB (8.0 flat) |
| float64 1,000 distinct | 3.9 ms | 12.1 ms | 10.1 ms [14.0] | 7.78 MB |
| float64 runs of 50 | 4.0 ms | 11.6 ms | 9.9 ms [20.4] | 0.27 MB (8.0 flat) |
| float64 smooth | 3.5 ms | 9.4 ms | 8.6 ms [15.2] | 7.91 MB |
| float64 signed (stays flat) | -- | -- | 5.6 ms | 8.0 MB |

A random float column costs about 2.4x its flat write for a 1.5% smaller file; that is the price of pylance's
layout, and nanolance stays faster than pylance. Dropping float bitpacking is one branch (`else if (is_float)` in
`write_session.cpp`) if that trade is ever wrong for a user.

**W1q2 (done, see "Result for W1q2"):** nullable plain RLE (no dictionary): pylance writes `Rle{Flat(64),Flat(8)}` + repdef
for a column with nulls and short runs; nanolance now writes the same (strict xfails removed from `test_float_dict_rle.py`).

## Result for W1s (2026-10-30, float64 and float32 layouts; performance conflict open)

Written: float columns take the integer decisions on their raw bits. `write_session.cpp`: the dictionary, RLE and bitpack
branches run for `double` (8 bytes) and `float` (4 bytes); the dictionary only at 8 bytes (as Lance's 64/128-bit rule).
A float with its own `lance-encoding:compression`, or with the writer-wide `bss-zstd` tag, keeps its layout. Bitpacking
for floats is `bitpack_beats_flat` on the raw bits (as Lance's estimate). `data_file_writer.cpp`: the `bitpack` flag
accepts floats. `schema_mapper.hpp`: `lance_logical_type_is_float`. No `encoding_choice` decision changed (they already
take bytes), so no C++ decision test was added; the new behavior is covered by the Python tests below.

Layouts against pylance 12 (2.2 files, `measure.py` in scratch, after the change): float64 dense 18 of 18 SAME; float64
with 5% nulls 15 of 18 SAME; float32 random, runs of 2 and 50 at 50,000 and 1,000,000 rows, 6 of 6 SAME. The three nulls
misses are plain RLE over nullable values without a dictionary (float64 runs of 2 at 50,000 and 1,000,000 rows, runs of 5
at 1,000,000): pylance writes `Rle{Flat(64),Flat(8)}` with `repdef`, nanolance writes flat. The same gap exists for int64
(checked: int64 runs of 2 with nulls, not implemented; `write_rle_column` is gated on `!column_has_nulls`). It needs the
RLE writer to take definition levels, as the dictionary writer does (W1p3). Strict xfail in
`bindings/python/tests/test_float_dict_rle.py`.

Reads: `test_float_dict_rle.py` reads every case back with nanolance and pylance, bit for bit (NaN payloads, signed zeros
via an int64 view of the raw bits), `take` of 200 rows, and an append to a pylance-written dataset (four shapes). A
float64 column with `lance-encoding:compression=none` stays flat. Float32 runs read back bit-exact (int32 view).
Test result: `test_float_dict_rle.py` 79 passed, 3 xfailed. Full Python suite (`-m "not bench" -n 4`): 4623 passed,
173 skipped, 8 xfailed. C++ `ctest`: 156 tests, all passed in `build/Testing/Temporary/LastTest.log`.

Byte-identical elsewhere: `nlbench_gendata 50000` data file md5 `84b4dce7912cd80d047e779f01a143fb`, unchanged (no float
column in that dataset changed).

Write time, 1,000,000 rows float64, best of 5, nanolance before (HEAD, built from `git archive`), nanolance now, pylance 12:

| column | nanolance before | nanolance now | pylance 12 | data before / now / pylance (B) |
|---|---|---|---|---|
| random in [0, 1) | 3.9 ms | 10.0 ms | 15.9 ms | 8,007,496 / 7,775,880 / 7,773,346 |
| 1,000 distinct, random order | 3.9 ms | 12.1 ms | 14.0 ms | 8,007,496 / 7,775,880 / 7,773,346 |
| runs of 50 | 4.0 ms | 11.6 ms | 20.4 ms | 8,007,496 / 267,336 / 263,681 |
| smooth (cumulative sum of noise) | 3.5 ms | 9.4 ms | 15.2 ms | 8,007,496 / 7,891,080 / 7,888,418 |

**Performance conflict (open, not resolved here).** The brief's rule says a random or smooth float column must not get
slower by more than 10%. It does: 2.5x on random, 2.7x on smooth. The cause is that pylance writes those columns
`InlineBitpacking(64)` (the sign bit is clear, so the estimate beats raw), and the layout has to match pylance. The
extra cost is the bitpack pass and its encode, plus the RLE scan that runs first; the split between them was not
profiled within this session's budget. Nanolance's new write is still faster than pylance on these columns. The
owner decides: keep the pylance layout (and the cost), or drop the float bitpacking (the `else if (is_float)` branch in
`write_session.cpp`: floats then stay flat on random data, which matches the old layout and the old time, but not
pylance's). The dictionary sample cost on random floats is the same early-out as int64 (unique ratio >= 0.98 at the
first sample), not measured separately.

Left: nullable plain RLE (the three strict xfails above, shared with int64); the C++ decision tests (none needed, see
above); the performance decision.

## Measured for W1q2 (2026-10-30, pylance 12.0.0 at data storage 2.2, `build/nlance-pagelayout`)

Harness: `scratchpad/w1q2/measure.py`. Values are random per run; a null slot is either `0` (underlying value 0, which
breaks the run it sits in) or keeps its run's value (`hidden`, the run goes on through it). The run-1 rows are the probe:
at run 1 the raw values decide the RLE choice alone.

Before (nanolance at 2b745d4, int64 only; "=" means the layouts are equal, DIFF means nanolance flat and pylance RLE):

| rows | nulls | underlying | run 1 | run 2 | run 5 | run 50 |
|---|---|---|---|---|---|---|
| 50,000 | 5% | 0 | = | DIFF | = | = |
| 50,000 | 5% | run value | = | DIFF | = | = |
| 50,000 | 50% | 0 | DIFF | DIFF | = | = |
| 50,000 | 50% | run value | = | DIFF | = | = |
| 1,000,000 | 5% | 0 | not run | DIFF | DIFF | = |
| 1,000,000 | 5% | run value | not run | DIFF | DIFF | = |
| 1,000,000 | 50% | 0 | not run | DIFF | DIFF | = |
| 1,000,000 | 50% | run value | not run | DIFF | DIFF | = |

The run-1, 50% row is the discriminator: pylance writes RLE there only when the nulls hold 0. So the null slots' raw
bytes count, as in Lance's `rle_is_applicable` (`compression.rs`).

After (the same matrix, this change): all 128 cases equal pylance, for int64, float64, int32 and uint8, at 50,000 and
1,000,000 rows, runs 1, 2, 5 and 50, 5% and 50% nulls, both underlying values. Full output:
`scratchpad/w1q2/matrix_after.txt`.

Layouts (pylance = nanolance, after), runs of 2 with 5% nulls at 1,000,000 rows, int64:
`MiniBlock{values=Rle{values=Flat(64),lengths=Flat(8)},repdef=Bitpacked(16,Flat(1)),nullable,buffers=2}`.

Chunks: pylance holds 4,096 rows per chunk with levels, nanolance 1,024 (W1p4, accepted). Only the layout text is compared.

## Result for W1q2 (2026-10-30)

Rule: a nullable fixed-width column with the RLE tag (the decision in `encoding_choice::fixed_column_rle_plan` already ran
over the raw values of every slot, so it was unchanged) writes `write_rle_column` with the validity: `encode_rle_chunks`
carries each chunk's definition levels, and the page descriptor is `Rle{Flat(bpv*8),Flat(8)}` + `repdef_encoding_bytes()`
with `mini_block_tail(m, rows, 2, true)`, as `write_dict_fixed_column` does for nullable indices. The gate
`!column_has_nulls` at the RLE branch of `encode_column` is gone. The non-null path is byte-identical (same
`page_layout_bytes_rle` call).

Commit: the W1q2 commit on `claude/nanolance-optimization-review-h3rxi7` (local, not pushed). Files: `src/file/data_file_writer.cpp`
(write_rle_column, encode_column), `bindings/python/tests/test_float_dict_rle.py` (xfails removed),
`bindings/python/tests/test_rle_nulls.py` (new, 80 cases), `docs/tasks/SPEC_GAPS.md`.

No decision change, so no C++ test was added to `tests/encoding/`.

Checks:

- `nlbench_gendata <dir> 50000` data-file md5: `84b4dce7912cd80d047e779f01a143fb` (unchanged).
- ctest: 156 of 156 pass.
- Python, subset: `test_rle_nulls.py`, `test_float_dict_rle.py`, `test_dict_64bit*.py`, `test_rle_multichunk.py`,
  `test_writer_tail.py`, `test_fsl_all_null_items.py`, `test_column_codec.py`, `test_lance_read_matrix.py`: 628 passed, 4
  xfailed (the remaining FSL xfails, unrelated). The full Python suite was NOT run: the volume filled up (1.7 GB free
  after the other sessions' scratch folders), so it could not be run without deleting other work. To run it, free space.

Timing, 1,000,000-row int64 with 5% nulls, best of 5 (`scratchpad/w1q2/timing.py`), seconds:

| column | nanolance before | nanolance after | pylance 12 |
|---|---|---|---|
| random, 5% nulls | 0.0092 | 0.0095 and 0.0096 | 0.0325 |
| runs of 2, 5% nulls | 0.0172 (flat) | 0.0307 and 0.0333 (RLE + levels) | 0.0498 |

The random column is within the 10% rule (+3% to +4%, noise-level). The runs-of-2 column is written as RLE now, so it
costs about 1.8x the flat write. That is the cost of matching pylance's layout; the owner should decide if it is
acceptable (the same trade as W1s's float write time).

Left: the 1,024-row chunks against pylance's 4,096 (W1p4); the write time above; nullable RLE of float32 and of
variable-width values was not measured.

Review (2026-10-30): accepted pylance's layout for nullable runs of 2 (31-33 ms against 17 ms flat for 1M rows at
5% nulls; pylance takes 50 ms), as for the non-null shapes. Full suite after review: 4,774 passed, 173 skipped, 5
xfailed; ctest 156/156; `nlbench_gendata` md5 unchanged.
