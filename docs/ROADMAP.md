# Roadmap: lists, maps and every other open gap

Written 2026-09-24, after the read-gap work in `docs/PROGRESS.md`. This supersedes the ordering of the
open-items list there. Each task is written to be picked up by a session that has not seen this one:
it names the evidence, the files, the oracle that proves it done, and which model should do it.

Everything below was **measured against pylance 2.2 files written for this plan**, not taken from the
backlog. That changed the picture substantially. Two gaps nobody had listed are bigger than lists, and
two items the backlog called large are small.

---

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
also fixed null structs outside any list, read until then as structs of nulls. Open: FullZip list
pages (C8), and a list of `fixed_size_list`. **C6 done** since: `map` reads as Arrow's
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
per-page string dictionaries -- within ~0.2% of pylance's size except high-cardinality strings, which
wait on an FSST encoder (F2).

### Phase E — small type gaps (any time; good first tasks)

| # | Task | Size | Model |
|---|---|---|---|
| E1 | `float16` read + write (`halffloat`, Arrow `e`). | XS | Sonnet |
| E2 | `duration` read + write (`duration:<unit>`, Arrow `tDs/tDm/tDu/tDn`). | XS | Sonnet |
| E3 | Arrow `null` type on write → `ConstantLayout` all-null (the reader already understands it). Delete the stale refusal message "cannot store nulls yet", which stopped being true at 1.1. | S | Sonnet |
| E4 | `large_utf8` / `large_binary` on write: decouple the chunk's u32 offsets from the declared 64-bit Arrow width across the variable-width paths. The exact fix is recorded at the refusal site. | M | Sonnet; Opus review |

### Phase F — performance (measure before building)

| # | Task | Size | Model |
|---|---|---|---|
| F1 | **Page size.** nanolance writes ~1024 rows per bitpacked page; pylance put 200,000 in one. Measure read time vs page size on the bench datasets *first* — this project's instruction profiles misled four times; only wall clock is trusted. | S (measure) + M | **Opus** |
| F2 | **FSST on write** — the answer to the one bench shape nanolance still loses (`high_card`, 7.67 ms vs 5.12 ms). Encoder only; the reader exists. Correctness oracle: pylance reads it; speed oracle: the bench. | M–L | **Opus** |

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

---

## Appendix A — decoding a descriptor without protoc

`nlance-pagelayout --dump-corpus DIR DATASET` writes each page's raw `PageLayout` descriptor, and
`python3 tools/pb_raw.py DIR/descriptor_1.bin` prints its wire structure with no schema — that is how
every descriptor in this file was read. Field numbers are in `protos/encodings_v2_1.proto` in the
Lance repository (`MiniBlockLayout`, `FullZipLayout`, `ConstantLayout`, `FixedSizeList`, `RepDefLayer`).
Task A1 makes the tool print all of this itself.
