# Roadmap: lists, maps and every other open gap

Written 2026-09-24, after the read-gap work in `docs/PROGRESS.md`. This supersedes the ordering of the
open-items list there. Each task is written to be picked up by a session that has not seen this one:
it names the evidence, the files, the oracle that proves it done, and which model should do it.

Everything below was **measured against pylance 2.2 files written for this plan**, not taken from the
backlog. That changed the picture substantially. Two gaps nobody had listed are bigger than lists, and
two items the backlog called large are small.

---

## Priority: competing with pylance directly (2026-10-08)

This order supersedes the "Suggested order" lines further down. The evidence is pylance 12's own
suite, run against nanolance.lance with every failure grouped by its cause: nanolance passes 259;
869 tests fail or error, of which pylance itself passes all but about 14. The counts are a proxy
for how often a user hits a gap, so the order also weighs how ordinary the workflow is: a gap in
plain append / update / scan comes before a whole subsystem.

| tier | gap | failing tests |
|---|---|---|
| 1 | ~~Writes with part of the schema~~ -- **done (2026-10-08)**: appends of some columns (files hold only those, as Lance's do), `merge_insert` with part of the columns, `when_matched_delete` / `fail`, the primary key as default `on`; 12 more of pylance's tests pass. Left: `write_mode("rewrite_columns")` writes whole rows (same results, other layout) | ~10 |
| 1 | ~~Commit retries~~ -- **done (2026-10-08)**: appends and overwrites rebuilt on the newer version, other changes rebased when the other writers only added fragments, delete / update / merge / compaction / index optimization re-run after a real conflict; found and fixed on the way: concurrent writers could overwrite each other's data files (names from a counter). `test_compact_with_write` passes 16 runs in 16 (15 before) | ~3 |
| 1 | ~~System columns~~ -- **done (2026-10-08)**: `_rowid` / `_rowaddr` in filters, `_rowoffset` and the row version columns in takes and scans, `_distance` / `_score` placement and `disable_scoring_autoprojection`, `default_scan_options` in the schema, `batch_size_bytes`, `LANCE_DEFAULT_BATCH_SIZE`; 12 more of pylance's tests. Left: a `_rowid` filter with vector or full-text search, or on a delete / update | ~10 |
| 1 | ~~Nested field paths~~ -- **done (2026-10-08)**: `s.x` and back-quoted projections, scalar indexes on paths resolved as Lance resolves them (exact, else the one case-insensitive match). Left: an INVERTED index on a nested field (build and search), SQL expressions in a projection (`list_struct[1]['x']`) | ~7 |
| 1 | ~~`order_by` in scans~~ -- **done (2026-10-08)**: `ColumnOrdering`, scans, batches and fragments; differential test against pylance over mixed types, NaN, nulls, filters, offset/limit. Left: order_by with vector / full-text search, on nested fields; a sort that spills to disk | ~5 |
| 1 | ~~Version housekeeping~~ -- **done (2026-10-08)**: tags, `cleanup_old_versions` / explain, auto cleanup on commit, `drop`, `version_refs`; 19 more of pylance's tests, and nanolance's cleanup picks the same files as pylance's on the same dataset. Left: branches (`create_branch`, tags on branches) | ~20 |
| 1 | ~~Write `_transactions/*.txn` files~~ -- **done (2026-10-08)**: every commit writes one (Append, Delete, Update, Rewrite, Overwrite, Restore, CreateIndex, Merge, Project, UpdateConfig, as pylance records the same change). Found on the way: a pylance commit from a read version older than a nanolance commit failed outright ("does not have a transaction file"); it now rebases or conflicts as between two pylance writers. Left: compaction remaps indexes in a second version (CreateIndex) where Lance's Rewrite carries the remap; no key-existence filter for merge_insert on primary-key datasets | ~3 |
| 1 | ~~Compaction parity~~ -- **mostly done (2026-10-08)**: Lance's planner exactly (candidacy by physical rows, runs broken by exclusions and by index coverage, split_for_size, source budgets, zero refused), `max_bytes_per_file` / `batch_size`, and Lance's two commits (ReserveFragments, then Rewrite); a differential test plans 16 random datasets as pylance does. Left: remapping an index's row addresses in the Rewrite (nanolance re-covers the rewritten fragments with a delta in a third version, so an indexed compaction is one version more and indexes an appended fragment Lance leaves unindexed), `defer_index_remap`, `Compaction.plan` / distributed compaction | ~10 |
| 1 | ~~Filter functions~~ -- **done (2026-10-08)**, with JSON columns themselves: Lance's JSONB on disk (nanolance could not read pylance's JSON columns, and pylance could not read nanolance's), all nine `json_*` functions, `::` casts, `arrow_cast`, `regexp_match` / `regexp_like`, `current_date()` / `now()`, `ARRAY[...]`, `0x..` binary literals, pyarrow's arithmetic, pylance's filter error wording; 20 more of pylance's tests. Left: `LanceDataset.sql`, JSON inside structs or lists, JSON text through lance-c (it returns the JSONB), `arrow_cast` to nested types | ~40 |
| 1 | ~~Small APIs~~ -- **done (2026-10-08)**: `stats` (dataset, index and data statistics, the same numbers as pylance for every index type nanolance reads, vector models included), `lance_schema` / `lance.schema.LanceSchema`, `update_field_metadata`, `read_transaction` / `get_transactions` (and `transaction_properties` / `commit_message` recorded), `merge`, `validate`, `lance.indices.IndexFileVersion` (which kept `test_vector_index.py` from loading: 23 of its tests now pass); 47 more of pylance's tests. Found on the way: a restore's transaction was recorded as an Overwrite, struct fields carried a "plain" encoding pylance does not write, Merge / Project transactions lacked `preserves_nullability`. Left: `merge` into a dataset with deleted rows, the deletion-vector check of `validate`, `LanceSchema.to_pyarrow` of a schema rebuilt from protos | ~25 |
| 1 | ~~Writer tail~~ -- **done (2026-10-09)**: Arrow dictionary columns (Lance's `dict:<value>:<index>:<ordered>` pages, every index width, string / binary / fixed-width values; read back with the dictionary they were written with, unused entries included, by either library), empty structs (one constant page, as Lance writes them; refused with Lance's error where Lance refuses), null elements inside fixed-size lists (`FixedSizeList.has_validity`), Lance's error for a zero-dimension fixed-size list, pydantic enums, `truncate_table`; 10 more of pylance's tests. Found on the way: a null element in a fixed-size list inside a list would have been written as 0.0 once top-level element nulls were accepted (now refused by name, as before). Left: dictionary columns inside structs or lists, element nulls under a list or struct | ~10 |
| 1 | ~~Blobs, every kind~~ -- **done (2026-10-09)**: Blob v2 written as Lance writes it (inline / packed / dedicated by the field's thresholds, external references; the same descriptors, blob ids, sidecar files and Lance 12 logical schema), nulls, several blob columns, `blob_field` / `blob_array` / `Blob`, `external_blob_mode="ingest"`, `allow_external_blob_outside_bases`, `blob_pack_file_size_threshold`, pylance's write checks and errors, `read_blob_ranges`, `to_pandas(blob_mode=)`; whole-object external blobs (size 0) read; legacy v1 blob columns of formats 2.0 / 2.1 read in every shape, and their 2.2 write refused as Lance refuses it (nanolance used to write a dataset pylance could not read); 86 more of pylance's tests. Then (2026-10-09): blobs beside vectors / lists / structs (that read was refused outright), and inside structs, lists and lists of structs, read and written as Lance pages them; found on the way: nanolance's blob pages over 64 KiB of descriptors had a row index pylance could scan but not take rows from (a panic) -- now Lance's byte-packed offsets, older files still read. Left: prepared-layout writers (`PackedBlobWriter`, ...), blobs through `add_columns` / `write_fragments`, registered external bases | ~40 |
| 1 | ~~DuckDB / Polars / LanceDB~~ -- **done (2026-10-09)**: datasets, scanners and fragments are `pyarrow.dataset` objects as pylance's are, so DuckDB replacement scans and `from_arrow`, Polars `scan_pyarrow_dataset` and `to_polars()` read them with pushdown; DuckDB's `lance` extension and LanceDB read nanolance's files and indexes, and nanolance reads and appends to LanceDB's (`tests/test_interop.py`, `tools/interop_suite.py`) | -- |
| 2 | ~~Distributed index builds from Python~~ -- **done (2026-10-09)**: `create_index_uncommitted` (BTree, Bitmap, LabelList, Inverted, IVF_FLAT, IVF_PQ, IVF_HNSW_SQ, with shared IVF / PQ models), `merge_existing_index_segments` (Lance's checks and errors: one keyed field, disjoint coverage, one model within 1e-5), `commit_existing_index_segments`, the legacy INVERTED flow (`create_scalar_index(fragment_ids=, index_uuid=)`, `merge_index_metadata` with its progress events, `LanceDataset.commit(CreateIndex)`), `lance.indices.IndicesBuilder` (`train_ivf` / `train_pq` / `prepare_global_ivf_pq` on float16 / 32 / 64 vectors, `transform_vectors`, `shuffle_transformed_vectors`, `load_shuffled_vectors`), `IvfModel` / `PqModel` files, `lance.bitmap.Bitmap`, `centroids()` / `get_ivf_model`; 120 more of pylance's tests. Found on the way: training a model on a dataset that already had an index of the default name failed ("already exists"), and data files dropped the schema metadata Lance writes into each. Left: merging is a rebuild over the union of fragments (the same index, not a physical merge of the segments' files), indexes over float16 / float64 columns, the index types of tier 3 | -- |
| 2 | ~~Transactions and fragment-level writes~~ -- **done (2026-10-09)**: `write_fragments`, `LanceFragment.create` / `create_from_file`, `DataFile.create`, fragment `delete` / `delete_rows` / `merge` / `merge_columns` / `update_columns`, `LanceDataset.commit` of every operation but `DataOverlay` (Lance's checks, `build_manifest`, and conflict rules: `CommitConflictError`, retryable or not), `commit_batch`, detached commits, `execute_uncommitted`, `add_columns` with UDFs and `batch_udf` checkpoints, `include_deleted_rows`; 95 more of pylance's tests. Found on the way: dropped columns' data files stayed in their fragments, a `LanceFileWriter` file (fields numbered from 0) could not be read in a dataset, JSON columns in `LanceSchema.from_pyarrow`, SQL column types (`"1"` not null, `int32 + 1` int32), Lance's data file names. Left: `DataOverlay`, `commit_lock`, `update_columns` of blob columns, conflict checks for index builds made from an older version, V1 manifest names | -- |
| 2 | ~~Namespaces~~ -- **done (2026-10-09)**: `DirectoryNamespace` (Lance's `__manifest` catalog, read and written by either library, concurrent writers retrying as Lance's do; directory listing, declared / registered / deregistered tables), `RestNamespace` and `RestAdapter` (each works against Lance's), `lance.dataset` / `write_dataset` / `commit` with `namespace_client`, managed versioning through `create_table_version`; 130 more of pylance's tests (130 of the 134 that pylance passes here; the other 4 need branches). Left: table branches, credential vending, materialized views, structured full-text queries in `query_table` | -- |
| 2 | ~~Stable row ids~~ -- **done (2026-10-10)** | -- |
| 3 | Search breadth: fuzzy full-text, ZONEMAP / NGRAM / BLOOMFILTER, more tokenizers (icu, jieba, lindera), full-text on list columns, Hamming and binary vectors, IVF_SQ / IVF_HNSW_PQ / IVF_HNSW_FLAT, Lance's tie order under a limit | ~60 |
| 4 | Niche: `mem_wal`, writing data storage versions other than 2.2, multiple base paths, samplers, debug / logging / otel hooks, bfloat16 and image extension arrays, `lance.util.KMeans` | ~120 |

Order of work: tier 1 top to bottom (each item is days, not weeks, and removes an error from an
everyday workflow); then distributed index builds (done); then transactions and fragment-level writes (done); then namespaces (done) and stable row
ids. Tier 3 items move up only when a user asks for one. Every item keeps the standing rules: same
answers as pylance (checked against it), no speed regression, pylance's errors where nanolance
still refuses.

---

## Found on published datasets (2026-09-28)

`tools/real_lance_check.py` read 24 tables from Lance datasets on the Hugging Face Hub
(`docs/REAL_DATASETS.md`). None mismatched. Two gaps matter most for what people actually store:

- **Done (2026-10-01): Lance file format 2.0** (footer `0.3`), all 6 refusals. LanceDB wrote 2.0 by
  default, so most LanceDB tables on the Hub are 2.0 (281 of 621 tables). `src/lance_v20_decoder.cpp`
  reads the `ArrayEncoding` tree of `encodings_v2_0.proto` (Flat, Nullable, Binary, Dictionary,
  FixedSizeList, FixedSizeBinary, List, Struct, PackedStruct, Fsst, both bit-packings, blob
  columns) into the same `ColumnValues` as 2.1, decoding only the rows a range or take needs. 57
  2.0 Hub tables read equal to pylance, and the Rust suite's 2.0 round trips pass but for types
  nanolance refuses in every format (`docs/RUST_SUITE.md`). Still open: Constant pages (Lance
  cannot read them either).
- **Done (2026-10-01): `take` on `list<string>` in FullZip pages** decoded each touched page whole:
  MS MARCO's `passage_text`, 256 rows, read 193 MB. A FullZip list page has a repetition index
  (buffer 1, one entry per row) giving each row's byte range; take and row ranges now read just
  those ranges and unravel their levels (`take_windows`, `decode_nested_rows`), large pages are
  read in windows, and the parallel reader splits them. MS MARCO: 3 ms (pylance 7). The work guard
  is `data_bytes_read`.

## What the survey found

### Two gaps nobody had listed

**1. FullZip layout: any string over 255 bytes, and every real embedding.** Lance picks its page layout
from the page's *longest* value (`MINIBLOCK_MAX_BYTE_LENGTH_PER_VALUE = 256` in
`rust/lance-encoding/src/encodings/logical/primitive.rs`). At or above it, the page is `FullZipLayout`
(`PageLayout` field 3), which nanolance cannot read at all:

| column written by pylance | layout | nanolance |
|---|---|---|
| strings, every value < 256 B | MiniBlock | reads |
| strings, **one** value of 300 B among 2000 | FullZip | `unsupported page layout` |
| `fixed_size_list<float32, 16/32>` | MiniBlock | fails (see 2) |
| `fixed_size_list<float32, 64/128/768/1536>` | FullZip | `unsupported page layout` |

One long value flips the whole column. Text columns and embedding columns are the two most common
things people put in a Lance dataset, so this is the most user-visible gap in the project. It fails
loudly and projecting other columns still works, which is why it hurt nobody's tests.

What FullZip actually is, from the descriptors (decoded raw; no `protoc` needed, see Appendix A):

```
FullZipLayout { bits_rep=0, bits_def=0|1, bits_per_offset=32 | bits_per_value=N,
                num_items, num_visible_items, value_compression, layers }
```

Per row: a control word holding the rep/def bits, then either the fixed-width value or a `u32`
length plus bytes. The per-value compression pylance chose was **FSST** for strings — which
nanolance already decodes (`fsst::decompress_value`) — and `FixedSizeList{dim, Flat(32)}` for
vectors. Buffer 0 is the zipped data; buffer 1 is a repetition index used for random access, which a
full scan does not need. For the non-list case this is a few hundred lines, not a subsystem.

**2. `fixed_size_list` is a compressive encoding, not a list.** Lance does not model FSL with
repetition levels. The schema has **no child field** (logical type `fixed_size_list:float:3`) and the
descriptor wraps the values: `CompressiveEncoding` field 11 = `FixedSizeList{ items_per_value,
values, has_validity }`. Reading it is a reshape of a flat buffer into an Arrow `+w:N` array. It
does not depend on the list work at all — it was only ever grouped with lists because Arrow calls it
a list.

### Two things smaller than the backlog said

- **`float16`** is `halffloat` on disk, `InlineBitpacking(16)`. **`duration`** is `duration:us`,
  `InlineBitpacking(64)`. Both are existing integer paths plus a schema mapping.
- **Rep levels reuse code that already exists.** A `list<int64>` page carries its repetition levels as
  `Bitpacked{16, Flat(1)}` — exactly the out-of-line encoding `unpack_out_of_line_bitpacked` was
  generalised for in the dictionary work. Decoding the levels is solved. What is missing is
  *interpreting* them.

### Lists, measured

| shape | rep | def | layers | notes |
|---|---|---|---|---|
| `list<int64>`, no nulls | `Bitpacked(16,Flat(1))` | — | all-valid | `num_items` = leaf values (10000 for 5000 rows of 2) |
| `list<int64>` with null lists, empty lists, null items | 1-bit | `Bitpacked(16,Flat(2))` | `[NULLABLE_ITEM, NULL_AND_EMPTY_LIST]` | 2 def bits: valid / null item / empty / null list |
| `list<list<int64>>` | `Flat(2)` | — | — | 2 rep bits |
| `list<string>` | 1-bit | — | — | values go through the existing FSST path |
| `list<struct<a>>` | | | | logical type `list.struct` |
| `map<string,int64>` | | | | `map` → `entries` struct → `key`, `value`: two physical columns |

All MiniBlock at these sizes, with `has_large_chunk = 1` (already handled by `MiniBlockChunkShape`).
`SparseLayout` (field 5) needs file version 2.3; pylance writes 2.2 by default, so it is a watch item,
not a gap.

The hard part is not decoding. It is (a) Lance's **rep/def unraveler** — turning level streams back
into per-layer offsets and validity, including the rule for which definition levels are visible at
which repetition depth — and (b) **nanolance's own data model**, which is flat all the way down:
`ColumnValues`, the Arrow batch builder, and above all `column_slice.cpp` (row ranges, deletion
vectors) are byte-addressed and would need offset rebasing per layer. (b) is the larger of the two.

---

## The plan

Sizes: **XS** < 1 h, **S** half a day, **M** 1–2 days, **L** most of a week — for one focused session.
**Model**: who should do it and why (see the section after the plan).

### Phase A — pin and instrument (do first; unblocks every diagnosis below)

| # | Task | Size | Model |
|---|---|---|---|
| A1 | **Fix `nlance-pagelayout` labelling.** It names physical columns after the first N schema fields, skipping structs but not lists or maps, so a list column is labelled with the list's name and a map's `value` column is labelled `key`. Map physical columns to *leaf* fields. Also print `rep=` (only a flag is stored today), `layers=`, FullZip details, and decode CompressiveEncoding field 11 instead of `Unknown(field 11)`. | S | Sonnet |
| A2 | **Pin every gap as a failing test by its message.** Add to `test_lance_read_matrix.py`: a *value-size* axis (strings of 64/255/256/1024 B, one-long-value-among-short), FSL at dims 8/64/768, the list shapes above, `halffloat`, `duration`. Use `KNOWN_GAPS` with the specific message, exactly as the four previous gaps were pinned, so each fails loudly when fixed. | S | Sonnet |
| A3 | **proto3 defaulting hardening.** `Field.type` (ours 2, proto3's 0) and `Field.encoding` (ours 1, proto3's 0) are the same shape as the `parent_id` bug. Neither is load-bearing on the read path today; make the decoder honour proto3 and add hand-built wire-byte tests like the `parent_id` ones in `tests/test_lance_minimal_pb.cpp`. | XS | Sonnet |
| A4 | **`macos-14` → `macos-15`** in `wheels.yml` and `ci-platforms.yml` (deprecated; a retired label queues forever — see PROGRESS item 12). | XS | Sonnet |

### Phase B — FullZip and fixed-size lists (highest user impact)

| # | Task | Size | Model |
|---|---|---|---|
| B1 | **Spike: settle FullZip's byte layout empirically.** Open questions, each answered by writing a file and reading bytes, not by inference: control-word width and bit order for `bits_def` 1 and 2; whether a null row carries a value slot (fixed-width) or a zero length (variable); when `num_items ≠ num_visible_items` without lists; what buffer 1 holds. Output: a short spec in this file plus hand-checked fixtures. | S | **Opus** |
| B2 | **FullZip reader, non-list** (`bits_rep = 0`): variable-width with FSST/plain per-value compression, fixed-width, nullable. New `ColumnEncodingKind`, parsing in `page_layout.cpp`, decode in `lance_column_decoder.cpp`, fuzz seeds. | M | Sonnet, against B1's spec and A2's tests; Opus review |
| B3 | **FSL reader** (field 11, `has_validity`), MiniBlock and FullZip; schema `fixed_size_list:<type>:<n>` → Arrow `+w:n`; nested child array in the batch builder. Row slicing is trivial (fixed stride). | M | Sonnet; Opus review |
| B4 | **FSL writer.** Emit field 11 around Flat. Verify stock Lance reads nanolance's MiniBlock FSL at dim 768 — legal, but unproven; if it does not, emit FullZip for wide rows. | M | Sonnet, then Opus if B4's verification fails |
| B5 | **FullZip writer for long strings** — only if measurement shows MiniBlock with 256 B+ values costs something real on read. Not needed for correctness: MiniBlock with long values is a legal page. | S–M | Opus decides; Sonnet builds |

**B1 result — the FullZip byte layout (non-list), confirmed against page buffer sizes.**

- Page buffer 0 is the zipped data. A variable-width page has a second buffer, the repetition index
  (`(rows + 1)` offsets), used for random access and ignored by a full scan.
- Every row starts with a control word of `0 / 1 / 2 / 4` bytes as `bits_rep + bits_def` is
  `0 / ≤8 / ≤16 / more` (`ControlWordParser::new` in `repdef.rs`). With no repetition the whole
  little-endian word is the definition level; with both, `rep = word >> bits_def` and
  `def = word & mask(bits_def)`. Level 0 is a valid item.
- **Fixed width:** every row is `control word + bits_per_value/8` bytes, the value slot present even
  for a null row. Checked: FSL-768 nullable, 2000 rows = `2000 × (1 + 3072)` = 6,146,000 bytes.
- **Variable width:** a valid row is `control word + length (bits_per_offset/8 bytes) + bytes`; a null
  row is the control word alone. Checked: one 300-byte string among 2000, `2000 × 4 + 19,180`
  = 27,180 bytes. Per-value compression is what `value_compression` says: `Variable` = raw bytes,
  `Fsst{…}` = each value FSST-compressed on its own.
- **fixed_size_list element validity** (`FixedSizeList.has_validity`): on FullZip, each slot starts
  with `ceil(N/8)` bytes of element bits (LSB-first, 1 = valid), then the N items, and
  `bits_per_value` counts both — 768 × float32 is 25,344 bits. On MiniBlock the chunk carries two
  buffers: element bits for all the chunk's rows back to back, then the values.

**Status:** B1–B4 done — see PROGRESS, "Roadmap phases A, B and E". B5 not started.

Done when: A2's value-size and FSL cells pass with their `KNOWN_GAPS` entries deleted, the
write-matrix gains FSL and long-string shapes read by both readers, and a 768-dim embedding column
round-trips pylance → nanolance → pylance.

### Phase C — lists, read side

Order matters: each step's tests are the next step's regression suite.

| # | Task | Size | Model |
|---|---|---|---|
| C0 | **Design note: nested `ColumnValues` and slicing.** How per-layer offsets and validity are carried from decoder to Arrow, and how `column_slice.cpp` slices and compacts them (row ranges and deletion vectors both have to rebase offsets per layer). This is the piece most likely to be got subtly wrong, and wrong here is silent. | S | **Opus** |
| C1 | **Port the rep/def unraveler** (`RepDefUnraveler` in `rust/lance-encoding/src/repdef.rs`: `levels_to_rep`, `unravel_offsets`, `unravel_validity`) as a standalone, fuzzable unit with table-driven tests built from pylance-written level streams. | M | **Opus** |
| C2 | `list<primitive>`, no nulls, full scan. Schema `list`/`large_list` → `+l`/`+L`. | M | Opus (establishes the pattern) |
| C3 | Null lists, empty lists, null items — all four `DefinitionInterpretation`s. | S | Sonnet, against C1's tests |
| C4 | `list<string>` / `list<binary>` — composes C2 with the existing variable-width path. | S | Sonnet |
| C5 | `list<list<…>>`, `list<struct>`, `struct<list>`. Multi-leaf structs under a list: every leaf carries the full rep/def and they must agree — check it, refuse on mismatch. | M | Opus |
| C6 | `map` = `list<struct<key, value>>` with logical type `map`, Arrow `+m`, keys non-null. | S | Sonnet, once C5 lands |
| C7 | Row ranges and deletion vectors on nested columns, per C0. | M | **Opus** |
| C8 | FullZip with `bits_rep > 0` (lists of long strings / wide FSLs). | M | Sonnet, after B2 and C3 |

**Status:** C0 done — [NESTED_COLUMNS.md](NESTED_COLUMNS.md). C1 done — `src/repdef.cpp`, tested
against Lance's own `repdef.rs` vectors and fuzzed (`fuzz_repdef`). **C2, C3, C4 and C7 done**, and
the `list<list<…>>` part of C5: every flat leaf type, null/empty lists and null items, dictionary and
constant pages inside lists, row ranges and deletions (`tests/test_lance_lists.py`, 66 cases against
pylance). **C5 done** too: lists of structs and structs of lists, with nulls at every level — which
also fixed null structs outside any list, read until then as structs of nulls. **C8 done**: FullZip
list pages -- lists of long strings or binaries, at any depth, under structs and as map values. Open:
a list of `fixed_size_list`. **C6 done** since: `map` reads as Arrow's
map type — it is stored as a list of (key, value) structs, so it needed only the schema mapping.

Done when the list shapes in A2 pass, `test_type_support_matrix.py` moves `list`, `large_list`,
`map` from `UNREADABLE_FROM_PYLANCE` to read-only, and the fuzz harness covers the unraveler.

### Phase D — lists, write side

| # | Task | Size | Model |
|---|---|---|---|
| D1 | Rep/def **serializer** (`RepDefBuilder`/`SerializerContext` in `repdef.rs`, the inverse of C1). Property test: serialize → C1's unravel = identity, over random nested Arrow arrays. | M | **Opus** |
| D2 | List, nested and map writers; `schema_mapper` accepts `+l`/`+L`/`+m`. Every shape joins the write matrix, read back by both readers. | M | Sonnet, against D1 and the matrix |

The read side has to come first: it is the oracle for the write side, alongside pylance.

**Status:** D1 done — `repdef::serialize` in `src/repdef.cpp`. Property test: 20,000 random nested
columns (lists and structs at depths 1–4, nulls and empties at every layer, garbage children under
null lists) serialized over a random row range and unravelled back, compared row by row; two
deliberate mutations fail it thousands of times. **D2 done:** lists, large lists, maps, lists of
structs, structs of lists and null structs are written and read by both readers
(`tests/test_lance_list_writes.py`, plus the type and parity matrices), including sliced batches,
several fragments, long rows split across pages, and `nanolance convert` from parquet. **Pages
compressed:** bit-packed levels, 1,024-value chunks (rows may span them), bit-packed integer items and
per-page string dictionaries -- within ~0.2% of pylance's size; high-cardinality string items are
FSST-compressed since F2.

### Phase E — small type gaps (any time; good first tasks)

| # | Task | Size | Model |
|---|---|---|---|
| E1 | `float16` read + write (`halffloat`, Arrow `e`). | XS | Sonnet |
| E2 | `duration` read + write (`duration:<unit>`, Arrow `tDs/tDm/tDu/tDn`). | XS | Sonnet |
| E3 | Arrow `null` type on write → `ConstantLayout` all-null (the reader already understands it). Delete the stale refusal message "cannot store nulls yet", which stopped being true at 1.1. | S | Sonnet |
| E4 | `large_utf8` / `large_binary` on write: decouple the chunk's u32 offsets from the declared 64-bit Arrow width across the variable-width paths. The exact fix is recorded at the refusal site. | M | Sonnet; Opus review |

**Status:** E1–E4 done. E4 turned out to need the opposite of what the refusal site said: Lance
wants **u64** offsets inside a large type's chunk, dictionary block and list-item chunk, with
`Variable{Flat(64)}` — it decodes each page straight into the Arrow type and refuses 32-bit offsets
for a large one. See PROGRESS, "Roadmap E4".

### Phase F — performance (measure before building)

| # | Task | Size | Model |
|---|---|---|---|
| F1 | **Page size.** nanolance writes ~1024 rows per bitpacked page; pylance put 200,000 in one. Measure read time vs page size on the bench datasets *first* — this project's instruction profiles misled four times; only wall clock is trusted. | S (measure) + M | **Opus** |
| F1 | *(status)* **Reopened and done.** The first measurement (200,000 rows) said no change was needed and claimed bitpacked columns were already multi-chunk pages. That was wrong: the flat writer put one chunk per page, so a 2M-row int64 column became 1,954 pages of 2.5 KB, and the benchmark matrix (`tools/bench_matrix.py`) showed Rust reading it 5.7x slower than its own file (104 vs 18 ms). Every fixed-width column is now written as ~512 KiB pages of many chunks (measured from 64 KiB to 8 MiB: within noise of the best for Rust, within ~10% of the best for nanolance); Rust reads nanolance's numeric files at its own speed. See PROGRESS.md, "Performance from the benchmark matrix". | | |
| F3 | **Memory budget for writing** — `max_pending_bytes`: write_batch flushes a fragment when the buffered data reaches it (edge devices). | S | **Done** — see PROGRESS |
| F2 | **FSST on write** — the answer to the one bench shape nanolance still loses (`high_card`, 7.67 ms vs 5.12 ms). Encoder only; the reader exists. Correctness oracle: pylance reads it; speed oracle: the bench. | M–L | **Opus** |

**Status:** F2 done — see PROGRESS, "Roadmap F2". String files are now as small as pylance's or
smaller. On `high_card`, rust lance reads nanolance's file in ~7 ms (10.8–18.7 ms before, when it was
zstd); nanolance's own read is about where it was (7.6–8.0 ms against 7.7–10.6 ms) and still trails
rust lance reading its own file (6.1–6.7 ms on the same runs). F1 is still open.

### Phase G — packaging and infrastructure (independent; run in parallel)

| # | Task | Size | Model |
|---|---|---|---|
| G1 | **CMake `install()` + `export()`** so `find_package(nanolance)` works (plan item 2.2, the oldest open item). Test by consuming the installed package from a separate CMake project in CI. | S–M | Sonnet |
| G2 | **Nightly fuzz** with a persisted corpus (`actions/cache`), all six targets, longer runs. CI's 120 s is a regression guard; the OOB read took ~9 min locally. | S | Sonnet |
| G3 | `nanolance import --rows-per-fragment`. | S | Sonnet |
| G4 | **Windows** bring-up in `ci-platforms.yml`, then add `windows-2022` to the wheels matrix. Mostly waiting on CI and fixing what it reports. | M | Sonnet |

---

## Which work suits Sonnet

**The price gap is 2×, not 5×.** Opus 5.5 is $4 / $20 per million input / output tokens; Sonnet 5 is
$2 / $10. So the best case for moving a task to Sonnet is half its cost — and that saving is gone
the moment the cheaper session needs a second round trip to reach the same result. Cost per
*finished task* is what counts, not cost per request.

What actually cost time in this project was **wrong conclusions, not typing.** Of the seven bugs
fixed in the last session, the expensive moments were the struct gap diagnosed as a missing
decoder (it was protobuf defaulting), a consistency check shipped as a bound, a mid-run fuzz
reading reported as a result, and a retired runner label read as scarce capacity. Each was a
plausible reading of the evidence, and each was caught by an oracle or by re-examining the
evidence — not by writing code faster.

So the split is not "easy vs hard". A task goes to Sonnet when **both** hold:

1. **It has a mechanical oracle.** A pylance differential test that fails today and must pass. A
   wrong implementation cannot survive it, whichever model wrote it.
2. **It needs no format archaeology.** The behaviour is already specified — in this file, in a
   spike's output, or in an existing nanolance path it composes with. Reading Lance's Rust source to
   infer undocumented behaviour stays with Opus.

That gives three patterns, used in the tables above:

- **Sonnet end to end** — A1–A4, E1–E3, G1–G4, C3, C4, C6. Specified, oracle-checked, mostly
  composition of existing paths.
- **Opus specifies, Sonnet builds, Opus reviews** — B2–B4, C8, D2, E4. Opus does the short spike or
  design note (B1, C0, D1) and writes the failing tests; Sonnet implements against them; Opus reviews
  the diff. Reviewing is much cheaper than authoring, and it is where a subtle defect gets caught.
- **Opus** — B1, C0, C1, C2, C5, C7, D1, F1, F2. Format archaeology, a new algorithm, a data-model
  change whose failure would be silent, or performance work where the measurement discipline matters
  more than the code.

**Rough effect.** By task count, about two-thirds of this plan can run on Sonnet. By effort it is
closer to half, because the large items (the unraveler, nested slicing, FSST) stay with Opus. At half
the price, that is on the order of **a quarter off the whole plan** — worth having, not transformative.
That figure is an estimate from task sizes, not a measurement.

Two cheaper levers are worth trying before a task moves models at all:

- **Opus 5.5 at lower effort.** Its default is already `medium`. For the XS/S Sonnet-bucket tasks,
  `low` may land within the same margin with no change of model, and keeps one cache namespace.
- **Give every session the oracle up front.** A task that starts from a failing test finishes in
  fewer turns on any model. Phase A exists partly for this.

### How to run it in Claude Code

- One task per session. Start it with `/model sonnet` (or `/model opus`), point it at the task's row
  here, and say which tests must pass.
- Or, from an Opus session driving the plan, delegate a Sonnet-bucket task to a sub-agent with
  `model: "sonnet"` and review what it returns before it is committed.
- Every task ends the way the last session's did: full pytest and ctest, the ASan/UBSan build with
  `-Werror`, a revert check that the new test actually fails without the fix, and for decoder changes
  a fuzz run that has printed its completion line.

---

## Recommended order

1. **Phase A** (a day, mostly Sonnet) — every later task starts from a pinned failing test and a
   dump tool that labels columns correctly.
2. **Phase E1–E3** alongside it — three small, visible wins.
3. **Phase B** — FullZip and FSL. The largest user-visible gain available: it makes text and
   embedding datasets readable, which are the two things most Lance users store.
4. **Phase C**, then **D** — lists and maps, read before write.
5. **Phase F** once correctness work has settled; **Phase G** in parallel throughout.

## Indexes: the open gaps (2026-10-07)

nanolance builds and searches Lance's BTree, Bitmap, LabelList, IVF_FLAT, IVF_PQ and INVERTED
indexes, and pylance and LanceDB use them as their own (`docs/PYLANCE_COMPAT.md`, "Indexes"). What
pylance users rely on that nanolance does not do yet, roughly in order of how often they hit it:

1. ~~**Keeping an index up to date**~~ -- **done (2026-10-08)**: `optimize_indices` (Lance's segment
   selection, `num_indices_to_merge`, `retrain`; a vector index keeps its model and is rebalanced as
   Lance 12 rebalances it), and compaction gives its rewritten rows back to the indexes
   (`include/nanolance/index_optimize.hpp`, `tests/test_index_optimize.py`).
2. **HNSW and quantized vector indexes**: ~~IVF_HNSW_SQ~~ -- **done (2026-10-08)**: searched with
   pylance's answers, built so pylance uses it, kept up by `optimize_indices`, in lance-c too
   (`docs/VECTOR_INDEX.md`, `tests/test_hnsw.py`). Left: IVF_HNSW_PQ, IVF_HNSW_FLAT, IVF_SQ, IVF_RQ
   -- on a dataset that has one nanolance answers correctly but by exact search, which does not
   scale. Related search gaps: batch query vectors, multivector (ColBERT-style) search, Hamming
   distance, building on float16 / float64 vectors (`stats.index_stats`, which most of pylance's
   own HNSW tests read, is done).
3. ~~**Stable row ids.**~~ -- **done (2026-10-10)**: format, reads, writes, mutations, compaction,
   indexes of every kind, lance-c.
4. **Full-text search beyond LanceDB's defaults:**
   - ~~positions (`with_position=True`) and phrase queries~~ -- **done (2026-10-08)**: read,
     searched (slop included) and built as Lance 12 does, in pylance and lance-c;
   - fuzzy matching (`fuzziness`);
   - other languages and their tokenizers: icu, jieba (Chinese), lindera (Japanese);
   - the ngram tokenizer and the NGRAM index, which serve `LIKE '%x%'` and `contains` filters;
   - full-text search over list columns;
   - multi-word top-k that skips postings (WAND / MaxScore). Already about 2x faster than pylance
     without it.
5. **Other scalar index types and column types:**
   - not built: ZONEMAP (cheap, increasingly the choice for range filters), BLOOMFILTER, NGRAM,
     JSON, RTREE;
   - BTree / Bitmap on decimal columns are built, but nanolance's filters cannot compare decimals;
     duration columns need `arrow_cast` in the filter engine; pylance's large-string index type
     test also fails (not yet diagnosed);
   - indexes on fields inside structs (`a.b`).
6. **Less common:** ~~distributed builds (`fragment_ids`, `index_uuid`, merging index metadata),
   `lance.indices.IndicesBuilder`~~ (done 2026-10-09, see the priority table), progress callbacks
   of single-machine builds, index file format v3 (256-document posting blocks).

Suggested order: see "Priority: competing with pylance directly" at the top (IVF_HNSW_SQ from (2)
and phrase queries from (4) are done).

## lance-c: what is left of the C API (2026-10-07)

`liblance_c` defines all 127 functions of lance-c's header (`compat/lance-c/UPSTREAM`). When this
section was written 39 of them only returned `LANCE_ERR_NOT_SUPPORTED`; none does now (2026-10-08):
the last 20, the index-segment API, are done, and lance-c's own suite passes 106 of 106. All 39 were
vector search, full-text search or index segments. Much of what they need now exists in nanolance's
core (Phases 2 and 3 above) and only has to be wired into `src/lance_c.cpp`. `docs/LANCE_C_COMPAT.md`
still calls vector and full-text indexes out of scope; it should be updated along with this work.

1. ~~**Wire what the core already does**~~ -- **done (2026-10-08)**: vector index creation and
   search with every setter, INVERTED in `create_scalar_index`, `full_text_search` and prepared Match
   contexts with both coverage modes (`tests/test_lance_c_search.cpp`).
2. **Wait on core features from the index gaps list:**
   - ~~`lance_scanner_set_ef`: HNSW indexes~~ -- done with IVF_HNSW_SQ (and
     `set_query_parallelism`, which Lance's HNSW search uses).
   - `lance_scanner_nearest_multivector`: multivector search.
   - ~~`lance_dataset_prepare_fts_phrase_query`: positions in INVERTED indexes~~ -- done.
3. ~~**Index segments**~~ -- **done (2026-10-08)**: builders, model training, metadata, commit with
   Lance's replacement rules, listing, and searches restricted to segments (vector, full-text with
   the whole index's scores, scoped scalar scans); pylance uses the indexes committed this way
   (`docs/LANCE_C_COMPAT.md`). What it was: build an index in pieces, possibly on other machines, and
   commit the pieces. This is the one large new subsystem; it shares its core with distributed
   builds (item 6 of the index gaps). The four lance-c tests nanolance fails today all need it
   (`index_segment_builder`, `index_segment_builder_progress`,
   `vector_models_and_reusable_segments`, `commit_index_segments`).
   - Builders: `lance_index_segment_builder_new_scalar` / `new_vector`, `lance_index_train_ivf_model`,
     `lance_index_train_pq_model`, `lance_index_segment_builder_execute_uncommitted` and
     `set_progress_callback`.
   - Segment metadata: `lance_index_segment_metadata_parse` and its 10 accessors (`uuid`, `name`,
     `dataset_version`, `index_version`, `index_type`, `index_details_type_url`, `field_count`,
     `field_ids`, `fragment_count`, `fragment_ids`).
   - Commit and list: `lance_dataset_commit_index_segments`, `lance_dataset_index_segment_count`,
     `lance_dataset_index_segments`.
   - Search with chosen segments: `lance_scanner_set_index_segments`,
     `lance_scanner_set_scalar_index_segment`, `lance_scanner_set_fts_index_segments`.
4. **Finish the partial ones:**
   - ~~`UPDATE_IF` and `include_deleted_rows`~~ -- done (2026-10-08).
   - ~~`lance_dataset_take_rows` on a dataset with stable row ids~~ -- done (2026-10-10).
   - `lance_scanner_set_substrait_filter`: Substrait filters.
   - Object-store URIs (`s3://` and others) in `lance_dataset_open` and the writes; `storage_opts`
     is accepted and ignored today.
   - ~~Writes in Lance's inline, packed and dedicated blob layouts~~ -- done: `lance_dataset_write`
     goes through the same writer as the Python module (checked 2026-10-10 with a Blob v2 column of
     inline, packed, dedicated, external, empty and null values, read back by pylance and nanolance).

What is left: the features item 2 waits on, the rest of item 4, and the options and index kinds
`docs/LANCE_C_COMPAT.md` lists as not implemented (BTREE / BITMAP / LABEL_LIST parameters, nearest
or full-text search with `set_fragment_ids`, IVF_SQ / IVF_HNSW_PQ / IVF_HNSW_FLAT, Hamming).

Found along the way (not lance-c's): full-text results with exactly equal scores come back in
another order than pylance's, and when a limit cuts through such a tie the two keep different rows
(short documents tie often; the scores of the rows kept agree). nanolance breaks ties by row id;
Lance 12's block-max WAND (`lance-index/src/scalar/inverted/wand.rs`, ~10,500 lines) keeps
whichever tied rows its traversal admits first -- it visits posting blocks in impact order and only
replaces a top-k entry on a strictly higher score -- so matching it means emulating that traversal
per query shape. Not done yet.

---

## Appendix A — decoding a descriptor without protoc

`nlance-pagelayout --dump-corpus DIR DATASET` writes each page's raw `PageLayout` descriptor, and
`python3 tools/pb_raw.py DIR/descriptor_1.bin` prints its wire structure with no schema — that is how
every descriptor in this file was read. Field numbers are in `protos/encodings_v2_1.proto` in the
Lance repository (`MiniBlockLayout`, `FullZipLayout`, `ConstantLayout`, `FixedSizeList`, `RepDefLayer`).
Task A1 makes the tool print all of this itself.
