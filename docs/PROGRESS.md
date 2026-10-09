# Implementation progress against the optimization plan

_Companion to [OPTIMIZATION_PLAN.md](OPTIMIZATION_PLAN.md). Every claim here was verified on this
branch; commands to reproduce are in the plan or the commit messages. Test counts are from
`ctest --test-dir build` and `pytest` in `bindings/python`._

## Status

| Plan item | State |
|---|---|
| **Phase 0 — stop the bleeding** | **complete** |
| 0.1 Nulls loud instead of wrong | done, plus the schema gate removed (0.6 below) |
| 0.2 Reject or fix write/read asymmetries | done — struct fixed, `large_*`/dictionary refused |
| 0.3 Fix the quick start | done, guarded by a new CI workflow |
| 0.4 Correct the docs | done, plus the C snippet is now a compiled test |
| 0.5 Repo slim-down | done for the unambiguous 5.5 MB; the 15.4 MB capture left in place by decision |
| **Phase 1.3 — read stock-Lance files** | **all three steps done** (parse, oracle, dispatch, FSST) |
| Fuzz coverage for the descriptor parser | done — found one real bug in 25 executions |
| 1.1 Real nullability | **done**, read and write, fixed- and variable-width; a null struct is still refused |
| 1.2 timestamp / date / time / decimal | **done** — plus a pre-existing width-declaration bug it exposed |
| Phase 2 — wheels, CMake install | **2.1 wheels and 2.3 CI done**; 2.2 install/export deliberately not |
| Phase 3 — the Python API a parquet user expects | **complete** — streaming, projection, row ranges, `count_rows`/`read_schema`, `nanolance convert` |
| Phase 4 — read-path optimization | **4.1 done** — 2.01x -> 1.01x peak, ~20% faster reads |
| [Roadmap](ROADMAP.md) A — pin and instrument | **done** |
| Roadmap B — FullZip and fixed-size lists | **B1–B4 done**; B5 (FullZip *writer*) not started, not needed for correctness |
| Roadmap F — performance | **F2 done** (FSST on write); F1 (page size) open |
| Roadmap E — small type gaps | **E1–E4 done** (float16, duration, Arrow null type, `large_utf8`/`large_binary` write) |
| Roadmap C — lists, read side | **C0–C8 done**; only a list of `fixed_size_list` is still refused |
| Lance metadata codec on nanom | **done** — one model in nanom, canonical proto3, deletion files now survive appends |
| Roadmap D — lists, write side | **D1–D2 done**: lists, maps, lists of structs, null structs round-trip; pages compressed to within ~0.2% of pylance (FSST aside) |
| Blobs, every kind (2026-10-09) | **done** — Blob v2 written inline / packed / dedicated / external as Lance writes them, legacy 2.0 / 2.1 blob columns read, blobs inside structs and lists and beside nested columns; `tests/test_blob_kinds.py` against pylance both ways |
| DuckDB / Polars / LanceDB interop (2026-10-09) | **done** — pyarrow-dataset subclasses, `to_polars()`, DuckDB `lance` extension and LanceDB read nanolance's files and indexes; `tests/test_interop.py`, `tools/interop_suite.py` |

Test suite: **54 ctest** (was 42) and **1395 pytest** (was 22), all passing -- and nothing skipped: the one
ctest that used to report a green SKIP for a real interop failure now passes for real.

Fuzzers: six targets (`decode`, `page_layout`, `fsst`, `lz4`, `deletion_vector`, `column_decode`),
all clean at their last run; the longest campaign run here was 95,896,936 executions. The newest,
`column_decode`, found two memory-safety bugs and one quadratic read — see the roadmap section below.

---

## What changed, and why it mattered

### The headline bug is gone

`nanolance.write_table` turned `[10, None, 30]` into `[10, 0, 30]` and `["x", None]` into `["x", ""]`
with no error and no warning, and stock Lance read those wrong values back without complaint. Ingest
now refuses a batch containing a null, naming the column, the row and the remedy.

The detection handles the cases a naive check gets wrong, each covered by
`tests/test_null_rejection.cpp`: Arrow's `null_count` is authoritative when non-negative but `-1`
means "not computed", so the validity bitmap is scanned; `ArrowArray::offset` shifts that window, so
a null outside it is not this array's row; an absent validity buffer is all-valid whatever a
malformed positive `null_count` claims; and a null on a parent struct is caught, since it makes every
child row null without any child-level bit being clear.

It costs nothing measurable: 1M rows × 4 columns, best of 5, **52.08 ms vs 52.41 ms** before.

Two existing tests had been shaped around the bug and now assert the opposite.
`smoke_arrowipc2lance_ignore_nullability.sh` asserted the corruption directly
(`[10, 0, 30, 40]`, commented "null slots keep prior bytes"). The Python
`test_lance_nullable_roundtrip` wrote a table full of nulls and then asserted only on a null-free
control column, so it passed throughout.

### A nullable-flagged schema no longer needs a flag

With null *values* refused unconditionally, the old `--ignore-nullability` gate tested the schema's
nullable bit rather than the data, and protected nothing. pyarrow marks essentially every field
nullable, so the first command a new user ran failed:

```
$ arrowipc2lance -c -o out.lance < in.arrow
nullable fields are not supported without --ignore-nullability: pos
```

...and the documented remedy was the flag that then silently corrupted actual nulls. The gate is
gone; the C setter, CLI flag and Python option remain as accepted no-ops so existing callers keep
working. On-disk output is unchanged.

_This differs from the plan, which proposed flipping the Python default to `False` plus a separate
opt-out. With the value check in place both are unnecessary, and flipping the default would have made
every pyarrow table error._

### No type writes a file nanolance cannot read back

Three types wrote successfully and then could not be read back — strictly worse than refusing them.
They turned out to be three different problems:

- **struct — fixed.** The writer always produced a correct, stock-Lance-readable file; only the
  reader refused, with "nested struct children are not supported in this reader build", for a feature
  `README.md` advertises. Plan resolution is now recursive, and nested lookup is scoped to the parent
  field id so a struct child named `id` no longer resolves to a top-level `id`.
- **`large_utf8` / `large_binary` — refused.** nanolance wrote 64-bit Arrow offsets into a miniblock
  chunk, but Lance v2.2 requires the u32 chunk grammar; **stock Lance called the result corrupt**.
  Lance keeps u32 offsets inside the chunk for large types too and signals the 64-bit Arrow width
  only in the page layout's `Variable{offsets = Flat{bits}}` node — pylance's `string` and
  `large_string` descriptors are byte-identical apart from that one token (`0x20` vs `0x40`).
  Supporting them properly means decoupling the chunk offset width from the declared Arrow width
  across every variable-width page path, so they are refused until that lands; the finding is
  recorded in the rejection comment.
- **dictionary — refused.** `pa.array(["a","b","a"]).dictionary_encode()` became an int32 column
  reading back `[0, 1, 0]` with the dictionary values stored **nowhere**. Nothing downstream could
  detect it: the file is a valid int32 column to every reader. `LanceField`'s `is_dictionary_index`
  and `dictionary_value_logical_type` are read only by the schema-equality comparison; no writer path
  ever stored the values. Casting to the value type costs nothing on disk, since nanolance
  dictionary-encodes low-cardinality string columns itself.

Arrow's `null` type also now says what is wrong instead of failing later with an internal message.

### The documented quick start works on a plain clone

Two independent breakages, both on the first commands a new user runs.
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` — verbatim from `README.md`, which states the
library needs no submodule — hit a `FATAL_ERROR` from the pcapng2lance example. And the shared
`./.deps` FetchContent cache bakes in its generator, so `pip install -e bindings/python`
(scikit-build-core, Ninja) poisoned it for the documented cmake call (Makefiles): *"Does not match
the generator used previously: Ninja"*. The example now skips with a `message(STATUS)`, and the cache
is keyed by generator. `.github/workflows/quickstart.yml` guards both.

### The docs no longer misdescribe the library

Corrected: "nullable columns" under type coverage; the `ignore_nullability` line in all four quick
starts; interop claims pinned to `lance` 7.0.0 when the verification ran against pylance 12.0.0; and
`pyproject.toml` saying 0.1.0 while the library said 0.2.0.

One correction ran the other way. **"`bool` row fields are not supported — use `uint8` for flags"
was simply wrong.** `bool` round-trips as a plain column and inside a struct, stock Lance agrees, and
`bool_flags` is a dataset nanolance writes *faster* than rust-lance (0.78 ms vs 1.26). The README was
steering users away from one of its best types.

Added, because the review found no honest statement of either: a **Type coverage** table (every Arrow
type either round-trips or is refused, listed explicitly) and a **What nanolance can read** table
(nanolance reads back everything it writes but is not a general Lance reader).

`tests/test_readme_quickstart.c` compiles and runs the documented C snippet verbatim so it cannot rot
silently. It is built as C rather than C++ on purpose: `nano_lance_writer.h` is an `extern "C"` header
whose only other consumers in the tree are C++, so nothing would have noticed it drifting into
C++-only syntax.

---

## Phase 1.3: the reader generalization

**Step 1 (done): parse the descriptor, and prove it agrees with the current dispatch.**

`ColumnPage` field 4 — the `/lance.encodings21.PageLayout` descriptor — was written by every page and
skipped on read. `decode_lance_physical_column` dispatched instead on nanolance-private field
metadata (`nanolance:packing`), which is exactly why stock-Lance files do not decode: they carry the
descriptor but not the metadata.

`nanolance/page_layout.hpp` now parses it into a typed tree. Unmodeled variants parse but record
their wire field number, so a caller refuses **by name** rather than misreading a page's buffers —
today's failure mode is the opposite, and worse: a stock-Lance int64 page is misparsed under
nanolance's layout assumptions and caught downstream by a size check, which is why a 3-row table
decodes and a 5000-row one does not.

`tests/test_page_layout.cpp` is the differential oracle: for every encoding the writer can emit, with
zstd off and on, it asserts the parsed descriptor **agrees** with the legacy metadata — two
independent signals in the same file, across the writer's whole output space. That is the safety net
the plan calls for, and it is what makes re-rooting decode a provable change rather than a rewrite.

**What the descriptors actually say.** `tests/smoke_pagelayout_pylance_interop.sh` runs
`nlance-pagelayout` over ten pylance-written datasets. Every descriptor parses:

| stock-Lance column | descriptor says | nanolance already decodes it? |
|---|---|---|
| `bool` | `Flat(1)` | yes |
| `double` | `Flat(64)` | yes |
| `int64` | `InlineBitpacking(64)` | **yes** |
| nullable `int64` | `InlineBitpacking(64)` | yes (needs the validity layer) |
| `timestamp[ms]` | `InlineBitpacking(64)` | yes (needs the type mapping) |
| `list<int32>` | `InlineBitpacking(32)` | yes (needs repetition levels) |
| `struct` | `InlineBitpacking(64)` + `Constant` | yes |
| low-card `string` | `InlineBitpacking(32)` + dict `General{scheme 1, Variable{Flat(32)}}` | yes |
| high-card `string` | `Unknown(field 6)` | **no — the one genuinely new variant** |
| `large_string` | `Unknown(field 6)` | no |

This sharpens the plan considerably. The review listed eight stock-Lance failures and framed step 3
as an open-ended "work down the list". The descriptors say **seven of the eight use encodings
nanolance already implements and already writes itself**. The read gap is one unmodeled
`CompressiveEncoding` variant plus the dispatch change — not a pile of missing decoders.

**Step 2 (done): select the decode path from the descriptor.**

`decode_lance_physical_column` now walks the parsed tree instead of reading `nanolance:packing`.
Every decode block is byte-identical; only the selector changed. The metadata remains the fallback
for pages with no descriptor, so older files decode unchanged.

The test that makes this real: every column of every generated dataset is decoded twice — once
normally, once with **every `nanolance:*` key stripped**, which is what a stock-Lance file looks like
— and the two must agree byte for byte. That caught the one branch genuinely still depending on the
private channel (constant columns read their value from `nanolance:const-value`); the value is now
taken from the descriptor's inline value, or from the page's own Lance scalar value buffer, in
preference to the metadata.

Unknown encodings are refused **by name** before any buffer byte is interpreted. The old failure mode
was worse than it looked: a stock-Lance int64 page was misparsed under nanolance's layout
assumptions and only caught later by a length check — hence 3 rows decoding and 5000 not.

**What stock-Lance files do now** (after 1.1's read half and 1.2 — see below):

| stock-Lance column | result |
|---|---|
| fixed-width integers, `float64`, `bool` | decodes |
| `timestamp`, `date`, `time`, `decimal` | decodes |
| nullable columns, including all-null | decodes |
| nullable with nulls in runs | refused — RLE'd definition levels |
| `utf8` | refused by name: `CompressiveEncoding` field 6 |
| `list`, `dictionary`, `struct` | refused at manifest mapping |

**Step 3 remains**, and is now a specific list rather than an open question: `CompressiveEncoding`
field 6, the `InlineBitpacking` and miniblock chunk framing differences, `AllNull`, validity levels
(shared with 1.1), and the logical-type mappings from 1.2. `FullZip` (PageLayout field 3) is what
blob-v2 packed pages use and is already handled by its own path.

### Phase 1.2: temporal and decimal types

`timestamp`, `date32/64`, `time32/64`, `decimal128/256` were rejected at write, which made nanolance
unusable for most real parquet data. They are fixed-width integers on the wire, so no encoder work
was involved — what was missing was the Arrow format strings and the Lance logical-type names. Those
names were read back out of manifests written by pylance 12.0.0 rather than invented, so a column
nanolance writes is described exactly as stock Lance describes its own.

Verified in all three directions, 14 type/unit/timezone combinations each: nanolance round-trip,
nanolance → stock Lance, and **stock Lance → nanolance**. The last is new — these columns previously
failed at manifest mapping.

A UTC-offset timezone (`+05:30`) is refused, because Lance supports IANA zone names only and *panics*
on offsets — pylance cannot write one either.

**It also exposed a pre-existing corruption.** A flat page's declared bits-per-value snapped anything
unrecognised to 32, so a 16-bit column and `fixed_size_binary(N != 4)` were mis-declared; stock Lance
panicked on the first and errored on the second — including the 6-byte MAC address README.md
recommends the type for. Integers escaped through `InlineBitpacking` (which declares its own width)
so `int16` happened to survive with default options; `fixed_size_binary` is not bitpackable and did
not. The root cause was a second copy of the width table inside the writer plus MiniBlockLayout byte
strings with hardcoded submessage lengths; both are now derived.

### Phase 1.1: nullability, read and write

The on-disk validity format is not documented anywhere nanolance could consult, so it was established
by reading real pylance 12.0.0 output:

- `MiniBlockLayout.f6 layers` is `[1]` without nulls, `[3]` with. `ConstantLayout.f5` says the same,
  and `[3]` with no inline value is how an **all-null** column is spelled.
- `MiniBlockLayout.f2` carries the levels' encoding — `CompressiveEncoding` field 4 (a bit-width
  wrapper) around `Flat(1)` when the only levels are 0 and 1.
- A chunk's 8-byte header is **four** little-endian `u16` slots, not the single size the reader
  assumed: the repdef value count followed by three buffer sizes, `0xFEFE` marking an unused slot.
- The levels are FastLanes-bit-packed — the same kernel integer columns already use — and
  **level 1 means NULL**, the opposite polarity to Arrow's validity bit.
- Values are dense over every row: a null still occupies a value slot.

Confirmed rather than assumed: unpacking reproduces a 1024-row alternating pattern bit for bit and a
scattered 10%-null pattern both bit for bit and in count (103 of 1024); the chunk arithmetic
reproduces every boundary of a 5000-row column.

**This also fixed plain `int64` from stock Lance, which had nothing to do with nulls**: a page is not
a chunk. nanolance's writer emits one chunk per page, so treating a page as a single chunk worked on
its own files; stock Lance packs five 1024-value chunks into one 5000-row page.

**The write side followed**, using the format above in reverse. Three things had to change beyond the
encoding itself: the manifest's `nullable` flag (hardcoded false, which makes stock Lance reject a
file whose column actually has nulls); the per-chunk control word (derived from the value bytes
alone, which agreed with the real formula only by coincidence); and the chunk size cap, since the
flat path's 4095-value chunks overran a 1024-entry level array — a stack smash found by running a
realistic nullable dataframe, not by any test.

**A severe pre-existing bug surfaced on the way.** Any read that failed — a truncated file, a missing
manifest, or an encoding nanolance does not implement — **segfaulted the process** through the C API
and the Python bindings. `lance_table_read_dataset` released `out_schema` on one failure path and the
C shim released it again; `ArrowSchemaRelease` nulls `release` and then dereferences it. The C++ tests
never caught it because they call the C++ entry point directly, and the Python tests only ever read
files nanolance had just written. The ownership contract is now uniform and documented.

### Phase 1.1b: nulls in string and binary columns

The definition-level layer built in 1.1 was fixed-width only, which left the most common thing a
pandas or pyarrow user does — a text column with missing values — refused. It now works, both
directions, plain and zstd:

```python
>>> nanolance.write_table(pa.table({"s": ["x", None, "zz"]}), "out.lance")
>>> lance.dataset("out.lance").to_table().column(0).to_pylist()
['x', None, 'zz']
```

On the write side this is the same layer applied to a different value encoding: the variable-width
`MiniBlockLayout` gains the `f2` repdef encoding and `layers = [3]`, and each chunk gains its level
buffer ahead of the offsets. The one structural difference is chunking. A variable-width chunk is
sized by the offsets math rather than by a value count, so a nullable one is additionally capped at
one FastLanes block (1024 values), the same cap the flat path needed.

Assembling the variable-width `PageLayout` was also **hand-written protobuf with a hardcoded
submessage length**, in a function that skipped a computed prefix of the fixed-width layout to reuse
its tail. It now builds the message the same way every other page path does, which is what let the
nullable variant exist at all.

**The read side got more general than the feature needed.** The variable-width branch concatenated
every chunk in a page and handed the result to one offset table — correct only because nanolance's
own writer emits one chunk per page. It now decodes chunk by chunk, and where a fixed-width chunk's
value count has to be inferred from its byte size, a variable-width chunk's is *derivable*: the chunk
opens with an `(n+1)`-entry offset table whose first entry is the byte position where the data
begins, so `n = offsets[0] / offset_width - 1`. That removes an assumption about the writer rather
than adding a special case for nulls.

Verified on 3000-row string and binary columns across five null patterns (scattered, first row only,
straddling the 1023/1024/1025 chunk boundary, all-null, none) with compression on and off, plus the
one-row all-null page, against nanolance's own reader **and** stock Lance in every combination.

### Phase 1.3 step 3: FSST, and stock Lance's string columns

`utf8` was the last common type a pylance file could carry that nanolance could not read *at all*.
The reason was one protobuf field: stock Lance wraps every variable-width column in
`CompressiveEncoding` field 6, which is **FSST** (Fast Static Symbol Table), with the real value
encoding nested inside it.

nanolance now decompresses FSST (`src/fsst.cpp`, ~90 lines). Only the decoder: the symbol-table
*construction* is the whole algorithm, the decode is a table lookup, and nanolance has no reason to
produce FSST when the uncompressed `Variable` pages it already writes are what stock Lance reads.

Two things about the format were worth the trouble to get exactly right:

- **The lengths follow the symbols, not the 256 slots.** The table is a fixed 2312 bytes
  (`[u64 header][256 u64 symbols][256 u8 lengths]`), but the encoder writes `n_symbols` symbols and
  then `n_symbols` lengths *immediately after them*. A 120-symbol table's lengths sit at byte
  `8 + 120*8`, not `8 + 256*8`.
- **`encoder_switch = 0` means the bytes are not compressed at all.** Lance skips FSST below 32 KiB
  of input but still writes the wrapper, so the passthrough case is most small files rather than an
  edge case.

**A short chunk's definition levels have two legal spellings, and the length tells them apart.**
Lance packs a level buffer into full 1024-value FastLanes blocks and then either pads the tail up to
a block or appends it raw as plain `u16` words -- whichever costs fewer bits. At width 1 that means a
tail of 64 or fewer values is always raw. Its decoder infers which from the buffer's length, so this
is not a hint: a 20 000-row nullable binary column from pylance ends in a 32-value chunk whose
64-byte level buffer nanolance rejected outright as "expected 128". The writer now follows the same
rule, which in turn required the padding below.

**Every buffer inside a miniblock chunk is padded to 8 bytes, and the header records the unpadded
length.** That was invisible while a level buffer was always a 128-byte block. It is not optional:
with a raw 62-byte tail and no padding, stock Lance read the values from the wrong offset and died
with "Inline bitpacking width 67108864 exceeds 64-bit values".

**Also generalized on the way**, because the same code had to be touched:

- Multi-chunk variable-width pages (see 1.1b).
- The dictionary block's own encoding is now checked. A unicode-heavy string column from Lance is a
  dictionary page whose dictionary is `General{LZ4, Variable}`; nanolance read it raw and failed with
  "dict block header invalid", naming the symptom rather than the missing decoder. It now refuses by
  encoding. (LZ4 itself is still not implemented.)

The stock-Lance read matrix, re-measured column by column, is in the README. What is left: run-length
definition levels, LZ4 buffers, and `list`/`struct` at the manifest level.

New coverage: `tests/test_fsst.cpp` (refusals, escapes, passthrough, accumulation),
`tests/fuzz/fuzz_fsst.cpp` (contract-asserting: a refusal carries a reason, a decode expands by at
most 8x, passthrough copies exactly, decoding appends and is deterministic), and the safety
workflow's PageLayout corpus is now seeded from **stock-Lance-written** datasets too -- FSST strings,
LZ4 dictionaries and run-length levels are the grammar nanolance's own writer never produces.

### Run-length-encoded definition levels

Lance picks between two encodings for a chunk's definition levels on its own, and the choice is not
about the column: it is about the *shape* of the nulls. A null every 13th row bit-packs; nulls that
come in runs, or that are very sparse, are run-length encoded instead. That makes **"one null in 5000
rows"** -- probably the most ordinary nullable column there is -- a different on-disk encoding from
"a null every 13th row", and only the second one decoded.

The block is `[u64 LE values_size][run values][run lengths]`, with the two widths named by the
descriptor's `Rle{ Flat(16), Flat(8) }`. A run longer than the length type can hold is split into
several entries carrying the same value, so decoding is a plain expansion with no special case.

Verified against pylance across six null shapes -- a single null, two runs, one-in-997, a leading
run, a trailing run, and alternating -- on `int64` and on an FSST-compressed string column. The
alternating case is deliberately kept in the same test: Lance chooses the encoding itself, so a test
fed only run-shaped nulls would quietly stop covering the bit-packed path the day that heuristic
changed.

### LZ4 dictionaries, and nullable categorical columns

The last non-nested stock-Lance shape that would not read was the most ordinary one a dataframe has:
a string column with few distinct values. Lance writes it as a dictionary page whose dictionary block
is `General{LZ4, Variable}`, and nanolance read that block raw and died on its header with "dict
block header invalid" -- naming the symptom rather than the missing decoder.

**LZ4 is now decompressed in-tree** (`src/lz4_block.cpp`, ~90 lines), with no new dependency. This is
the LZ4 *block* format, not the framed one: a token byte, a run of literals, and a back-reference,
with no entropy coder and no checksums. Vendoring a library to read that would have cost more than it
saved -- nanolance carries zstd only because zstd genuinely needs it. Lance's wrapper around the
block is `[u32 LE uncompressed size][block]`, from the `lz4` crate's prepend_size mode.

The unit test is **differential against the reference encoder**: its vectors are liblz4's own output
(via python-lz4), so the decoder is checked against the real thing rather than against my reading of
the spec. Malformed cases are hand-built, because no encoder produces one.

**The fuzzer found a real bug in 38 executions.** `decompress_block` reserved the declared
uncompressed size before touching the block, so an 8-byte buffer declaring 3.7 GB allocated 3.7 GB --
under the 8 GiB per-buffer read limit, so the budget never fired. The declared size is now checked
against what the block could possibly produce first: literals are copied 1:1, and a match costs at
least one extension byte per 255 output bytes, so no LZ4 block expands by more than 255x.

**Two more generalizations came with it**, both of the same kind -- a decode branch was deciding
something the descriptor already says:

- *How the dictionary block is compressed* was hardcoded per branch: `dict` read it raw, `dict-rle`
  un-zstd'd it. That happened to match what nanolance's own writer does. It now comes from the
  descriptor, so raw, zstd and LZ4 all decode from one unwrap.
- *A nullable dictionary column* -- a categorical column with missing values -- failed with
  "unexpected miniblock payload prefix", because the dictionary path had its own chunk parser that
  predated definition levels. It now uses the same splitter as every other miniblock path, so the
  levels decode on the way and that parser is gone.

Relatedly, an unrecognized `General{scheme}` no longer silently falls through as "not zstd, therefore
raw". It is refused by number, which is what the descriptor-dispatch work was for in the first place.

### Fuzzing the descriptor parser

`page_layout.cpp` parses untrusted input that will drive decoder selection, so it got its own
libFuzzer target asserting contracts rather than just absence of crashes: a refusal always carries a
reason, a refused parse leaves no output behind, `describe()` is total, and parsing is deterministic
across calls.

**It found a real bug in 25 executions.** `decode_page_layout` picked the layout kind before parsing
its body, so a malformed body returned `false` with `kind` already set to `kMiniBlock` — and step 2's
dispatch reads exactly that field. The parse now builds into a scratch value and publishes only on
success.

It also surfaced a bug in this branch's own earlier FetchContent change: keying the shared `.deps`
directory by generator was not enough, because nanoarrow still compiled into it, so a
sanitizer/fuzzer tree and a plain tree shared object files. nanoarrow now builds into the current
build tree, matching what zstd already did.

## Phase 2: distribution

### 2.1 Wheels

`pip install nanolance` is the entire on-ramp for the audience this library is trying to reach, and
nothing else in the plan reaches them without it -- the alternative is CMake, a C++20 toolchain and
network access for FetchContent.

`.github/workflows/wheels.yml` builds manylinux_2_28 x86_64 and macOS arm64/x86_64 wheels for CPython
3.9-3.13 with `cibuildwheel`, tests each one in a fresh interpreter, and publishes to PyPI on a
version tag through trusted publishing (no API token in repository secrets). Non-tag runs build and
test without publishing, so the release path is exercised long before a tag exists.

**Building one locally turned up a real packaging bug.** The wheel was **1.92 MB, and about half of
it was not nanolance**: 2 MB of nanoarrow and zstd *headers* installed into `site-packages/include/`,
`lib/cmake/nanoarrowConfig.cmake`, `lib/pkgconfig/libzstd.pc`, and two `libnanoarrow*_shared.so` that
nothing loads (the extension links the static libraries). `FetchContent_MakeAvailable` runs a
dependency's own `install()` rules as part of ours, and scikit-build-core packages whatever
`cmake --install` writes. The Python build now uses `add_subdirectory(... EXCLUDE_FROM_ALL)` like the
top-level tree already did -- which also stops the unused shared libraries being built at all -- and
the install is scoped to a named component as a second guard. **1.92 MB -> 912 KB**, containing
exactly the extension and the package.

The wheel's test is `bindings/python/tests/wheel_smoke.py`, deliberately not the pytest suite: that
needs pylance, polars and pandas, which would be testing *their* wheels. What matters is that the
extension loads with no zstd or nanoarrow on the system and that a round trip -- nulls included --
gives back what went in.

Also: `nanolance.__version__` now exists, read from the installed distribution's metadata rather than
being a third copy of the version number.

### 2.3 Wider CI

`.github/workflows/ci-platforms.yml` builds and runs the full ctest suite on **macOS** on every push;
that claim has been in the docs untested since the branch opened, because every other workflow here
is Linux. **Windows is `workflow_dispatch` only**, on purpose: the tree has never been built there,
and a job that is red on every push teaches people to ignore red. Turning it green is what unlocks
Windows wheels, and the comment in each file says so.

### Not done: 2.2 CMake install/export

`find_package(nanolance)` from an install tree is still missing. It is not a matter of adding
`install(TARGETS ... EXPORT)`: nanolance's public headers include nanoarrow's, and both nanoarrow and
zstd arrive as FetchContent'd static libraries whose own interface include directories point into the
build tree, so exporting nanolance means deciding how to ship *them* -- and a vendored copy of
nanoarrow's headers on a consumer's include path is a real decision, not a mechanical one. The
audience for it also already has a working path (`FetchContent_Declare(nanolance)`), unlike the
wheel audience, which had none. Left for a deliberate pass rather than done halfway.

## Phase 3: the Python API a parquet user expects

### 3.2 Column projection

`nanolance.read_table(path, columns=["ts", "level"])` now works, shaped like
`pyarrow.parquet.read_table` so the migration is a one-line diff.

This is a real projection, not a post-filter: `lance_table_read_dataset_projected` already existed in
C++ and skips the unasked-for columns **during decode**. That is the whole point — column
materialization is where a read spends its time, so on a wide table read for a few columns the
skipped work IS the cost. It reached Python through a new
`nano_lance_table_read_dataset_projected` C entry point that shares `read_dataset_impl` with the
full read rather than duplicating it; the duplicated version of that ownership contract is what used
to segfault every failed read.

Three edges are refused by name rather than guessed at:

- an unknown column name (an error, not a silently empty result);
- an empty list (`columns=None` reads everything; `columns=[]` reaching the schema mapper came back
  as "schema mapping has no root fields", which tells a caller nothing);
- a bare string -- the trap worth a test, since `str` *is* a `Sequence[str]`, of its own characters.

**One documented difference from pyarrow**, pinned by a test so it cannot drift silently: the result
is in the dataset's column order, not the order you listed. `columns=["c", "a"]` gives back
`["a", "c"]`.

The Python README's read section was rewritten at the same time. It still said interop was "verified
vs lance 7.0.0" and that pylance -> nanolance was "best-effort only for simple schemas", which
stopped being true several commits ago.

### A plan claim, measured — and it changed the sequencing

Before building 3.1 on top of it, I measured the "peak memory ≈ 2× the dataset" claim in the plan's
§2.7. Reading the code suggested it was wrong (`ExportedTable::from_read_result` is `ArrowArrayMove`,
not a copy). **The measurement said otherwise, and the measurement won:**

| 61 MiB, 4M-row table | peak RSS / dataset |
|---|---|
| written as **1 fragment** | **2.01×** |
| the same data as **16 fragments** | **1.10×** |

So the 2× is real, but not for the reason the plan gave. It is not batches accumulating — it is
**inside one fragment's decode**: the decoder fills `ColumnValues` buffers and then copies them into
the ArrowArray, so both exist at once. Across 16 fragments that transient is 1/16 of the data and the
peak collapses.

**This changes what to do next.** Plan item 3.1 says streaming "removes the 2× peak". It does not:
streaming removes the *accumulation*, which only bites on many-fragment datasets. An ordinary
single-fragment dataset would still peak at 2× after 3.1. The thing that removes it is **4.1,
decoding straight into `ArrowBuffer`** — sequenced last in the plan, on the reasoning that the decode
paths were about to be rewritten for 1.1 and 1.3. Those rewrites are done, so that reason is spent.

Both §2.7 and §3.1 of the plan now say this, so the next person to read it is not misled the way I
nearly was.

### 3.1 Streaming read

`nanolance.open_stream(path, columns=None)` returns an Arrow C stream that decodes **one fragment per
pull** instead of every fragment up front.

Measured on a 61 MiB / 16-fragment dataset:

| | eager `read_table` | `open_stream` |
|---|---|---|
| peak RSS | 61.4 MiB (1.01x) | **5.6 MiB (0.09x)** |
| time to first batch | 66.5 ms | **3.2 ms** |

Which is exactly what the corrected §2.7 predicted: streaming buys **larger-than-memory datasets and
latency**, and 4.1 is what bought the 1.01x. Neither subsumes the other, and together a read now
costs about one fragment.

**The design decision worth recording: this is a new entry point, not a change to `read_table`.**

My first attempt made `read_table` itself stream, which looked like a free win — same signature, same
result, less memory. Two existing tests failed, and they were right to:

- `test_failed_read_raises_instead_of_crashing` — a corrupt dataset used to raise from
  `read_table(...)`. Streaming defers the decode, so the open succeeds and the error surfaces later,
  from whoever pulls a batch. Anyone with `try: read_table(...) except:` around the call gets an
  uncaught exception from somewhere else entirely.
- the exception *type* changes too: `RuntimeError` from our own error path becomes `OSError`, raised
  by pyarrow when the stream's `get_next` reports a failure.

Both are silent API breaks — the kind that passes review because the diff looks like a pure
improvement. So `read_table` keeps its eager contract and its up-front errors, `open_stream` is a
separate function, and its docstring states the two differences outright. A caller who wants the
memory profile opts into the different error behaviour explicitly.

**How it is built.** The eager and streaming paths share one `ReadPlan` (dataset path, schema
mapping, data files, projected field ids) produced by `open_read_plan`; `read_dataset_eager` loops it
to completion, `LanceTableStream` holds it and advances a cursor per `next()`. So projection,
validation and the read limits are defined once. Two details that bit:

- **`ScopedReadLimits` is thread-local and scoped**, so the stream re-establishes it inside every
  `next()` rather than once at open — otherwise the limits silently stopped applying to every batch
  after the first.
- **The stream owns a deep copy of the schema** (`ArrowSchemaDeepCopy`), and `get_schema` hands out a
  fresh copy per call, because the C data interface lets a consumer call it more than once and
  release each result.

On the Python side `ExportedStream` is single-shot: exporting the capsule twice raises "already been
consumed" instead of handing out a second owner of the same `ArrowArrayStream`.

This work is also what found the `ArrowArray::offset` writer bug fixed in the preceding commit — the
streaming tests were the first in the repo to feed the writer sliced batches.

### The writer bug the streaming tests found

Worth its own entry, because it was silent data corruption of the same class this branch opened with.

`test_stream_yields_the_same_rows_as_a_full_read` failed. The stream and the eager read agreed with
each other and both disagreed with the source table, from exactly one row index on. Reading it
through pylance gave the **same wrong value** — so the bytes on disk were wrong and this was never a
reader bug. A type probe narrowed it to `utf8` and `binary`; `int64`, `float64`, `bool` and
`fixed_size_binary` were all correct.

A sliced Arrow array shares its parent's buffers and addresses its rows through `ArrowArray::offset`.
`append_fixed_width` applied that offset. `append_variable_width` did not — it read the offsets
buffer from index 0 and the data buffer from byte 0. `Table.to_batches()` returns slices of one
contiguous array, so **every batch after the first re-ingested the first batch's strings**, and a
mixed table came back looking plausible: the integer columns lined up, only the strings were shifted.

The existing chunked-writer tests missed it because their batch helper allocates a fresh array per
batch, so every batch had `offset == 0`. The new guards deliberately do not: the C++ one hands the
ingest a borrowed shallow slice, as a first batch and as an accumulating one; the Python ones round
trip `to_batches()` output for `utf8`, `binary` and nullable `utf8`, plus a sliced *first* batch and
empty batches in between; and one of them cross-checks through stock Lance, since reader agreement is
what proved where the bug lived. Each was verified to fail against the unfixed code.

### 3.3 / 3.4: the parquet on-ramp, and the bug it found

`nanolance convert in.parquet out.lance` (also `python -m nanolance convert ...`), plus
`nanolance inspect`. Conversion streams: `ParquetFile.iter_batches` into `LanceWriter`, so a file
larger than memory converts in bounded memory, and it prints the comparison that is the whole point:

```console
$ nanolance convert events.parquet events.lance --compress
200,000 rows  events.parquet -> events.lance
  parquet :    2.0 MiB
  lance   :  530.3 KiB   (3.92x smaller)
```

`arrowipc2lance` grew `-i/--input FILE`; stdin stays the default. Reading only from stdin meant
`arrowipc2lance -i in.arrow` did not work and a shell-less subprocess call could not feed it at all.

For 3.3 the API was already `pyarrow.parquet`-shaped; what was missing was saying so. The Python
README now has a side-by-side migration table and the four differences worth knowing first (the read
returns a handle rather than a `pa.Table`; a projection comes back in dataset order;
`compression=True` is a boolean, not a codec name; unsupported types are refused at write time).

### count_rows and read_schema: the cheap questions

`nanolance.count_rows(path)` and `nanolance.read_schema(path)`, the remaining half of 3.2. Both
answer from the manifest and never open a data file:

| on a 200k-row dataset | |
|---|---|
| `count_rows` | 0.18 ms |
| `read_schema` | 0.12 ms |
| full `read_table` | 16 ms |

The regression guard is not a timing assertion -- "cheap" is not testable that way. It **deletes the
data files** and checks both still return the right answers, while a real read of the same dataset
now raises. That tests the actual claim.

Two details:

- `count_rows` sums the fragments rather than reading a total field, because the number has to be
  the one a read would give and a read walks those same fragments. A test asserts the two agree.
- The schema handle is **re-exportable**, unlike the single-shot stream handle: a schema is
  copyable, so each `__arrow_c_schema__` hands out a fresh deep copy. Getting that wrong would be a
  use-after-free rather than an error -- the first export would give away the handle's only
  `ArrowSchema` and the second would read released memory -- so it has its own test.

`nanolance inspect` was rewritten onto them; it used to stream every batch just to count rows.

### The dictionary encoding that made whole FILES unreadable

Writing the 3.4 demo and reading it back with pylance is what found this, and it is the strongest
argument for building the demo at all: no existing test wrote a table of this shape.

A scattered low-cardinality string column (`"INFO"/"WARN"/"ERROR"`) picks the structural-dictionary
encoding. Its page declared `has_large_chunk = false` -- the u16 chunk-meta grammar. Lance v2.2's
`validate_page_layout` refuses **any** miniblock page that does:

    Invalid user input: Lance v2.2 miniblock pages require the u32 chunk grammar

and it validates the file's whole page table before decoding anything. So one dictionary-encoded
column made **every other column in the same dataset** unreadable by stock Lance -- reading just the
`id` column of a three-column table failed. The README's claim that everything nanolance writes is
Lance-readable was wrong for any table containing such a column, which is an ordinary shape.

The word layout is identical in both grammars -- `(word >> 4) + 1` eight-byte units of chunk size,
`word & 0x0F` as log2 of the value count -- so only the *width* changed: u16 words to u32, and
`has_large_chunk = 1` in the layout. The chunk headers needed nothing: `append_miniblock_chunk`
writes `[u16 0][u16 size][u16 0][u16 0xFEFE]`, and Lance's u32 reader takes bytes 2..6 as the buffer
size, which is `size | (0 << 16)`. That is why every other miniblock column already passed.

**How it stayed quiet for months.** `tests/smoke_dict_pylance_interop.sh` wrapped its pylance read in
`except Exception: sys.exit(77)`, and ctest reports 77 as a SKIP. The suite said
`49/49 tests passed`, with one line of "did not run" underneath. That is the same failure mode as
the `smoke_pagelayout_pylance_interop.sh` exit-0 found earlier on this branch: a test that reports
success for a genuine failure is worse than no test. The `try`/`except` is gone, so the script fails
if this regresses, and `test_stock_lance_reads_a_dictionary_encoded_column` asserts it from pytest
too -- including reading a neighbouring int column on its own, since that is what the whole-file
validation broke.

### Row ranges: `read_table(..., offset=, length=)`

The last piece of 3.2. Measured on a 61 MiB dataset in 16 fragments of 250k rows:

| | time | vs full read |
|---|---|---|
| full read (4M rows) | 72.1 ms | — |
| 1,000 rows mid-dataset | 2.5 ms | **29x faster** |
| 500,000 rows (2 fragments) | 7.3 ms | 9.9x |
| half the dataset (8 fragments) | 35.1 ms | 2.1x |

Peak memory for the 1,000-row range: 1.8 MiB, **0.03x** the dataset.

**The claim is deliberately narrow.** The rows returned are exactly the rows asked for, but the work
saved is proportional to *fragments skipped*, not rows dropped -- a range inside one fragment still
decodes that fragment, and a single-fragment dataset saves nothing. The docs say this rather than
implying a row-granular win, because a `table.slice()` dressed up as a range would be worse than not
having one.

**Where the slicing happens, and why there.** On `ColumnValues` -- the decoder's output, *before* the
Arrow arrays are built -- not on a decoded `ArrowArray`.

That was the whole safety argument. Setting `ArrowArray::offset` on a decoded batch would mean
producing offsets from a reader that has never produced them, with `bool` (bits, not bytes), validity
bitmaps (also bits) and struct children (offset applied at exactly one level, or it double-counts)
each needing separate care. That is the same class of mistake as the writer's `array.offset` bug, two
of which this branch already found. Slicing `ColumnValues` instead makes every value buffer
byte-addressed -- `bool` is a *byte* per value until the Arrow buffer is assembled -- leaving exactly
one thing that needs bit arithmetic: the validity bitmap.

So `slice_column_values` is one function in one file, and `tests/test_column_slice.cpp` brute-forces
it: every `(total, first, count)` combination up to 40 rows, across fixed-width, 32- and 64-bit
variable-width, nullable and not, compared bit by bit against a reference built from plain per-row
vectors. 40 rows spans five bitmap bytes, so every `first % 8` and `count % 8` pair appears many
times. Validity is compared bit by bit rather than byte by byte, so the last byte's padding cannot
hide a wrong answer or fail a right one.

**Skipping is validated where it can be.** `skip`/`take` come from the manifest's per-fragment row
counts, but the rows are in the data file. When a file is partially covered the two are compared, and
a disagreement is an error -- a silently misaligned range is precisely the failure this must not
have. A fragment spanning several data files has no per-file row count in the manifest at all, so a
range over one is refused by name rather than guessed at.

**The test that proves the feature does anything.** `test_fragments_outside_the_range_are_never_opened`
**deletes** the data files the range does not need and checks the read still succeeds. That is the
only way to tell a real range from `read_table(...).slice(...)`, and it also fails if the plan is
merely sloppy -- opening one extra fragment breaks it.

Everything else is checked against `full_read.slice(offset, length)` as the oracle: a 32-point sweep
of offsets and lengths (on, inside and either side of every fragment boundary, most not multiples of
8) over a dataset holding *every* encoding at once, eager and streamed, plus each encoding on its own
so a failure names the encoding rather than the dataset.

### The struct bug the range tests found

Writing the range tests needed a struct column fed through `to_batches()`, and that had never been
done:

    RuntimeError: column value count does not match row count for a

Arrow does not slice a struct's children when the struct is sliced -- the parent carries the offset
and the children keep their full extent. `resolve_field_array` returned the child pointer directly,
so a sliced struct column reported the wrong rows *and* the wrong count, and the writer refused the
batch. Loud rather than silent, unlike the flat-column version of this bug fixed earlier on this
branch, but it meant `to_batches()` -- the obvious way to feed the streaming writer -- could not
write a struct column at all.

Field resolution now returns a **rebased, borrowed view** whose `offset`/`length` compose every
enclosing array's slice, so the ordinary `array.offset + row` arithmetic keeps working everywhere
downstream. Both conventions compose correctly: pyarrow puts the offset on each column, while a
producer that slices the record batch itself puts it on the root.

Two details the change turned up:

- **ASan earned its keep.** The first version stored pointers to per-iteration view locals in a
  vector that outlived the loop. The plain build passed 50/50 with a live stack-use-after-scope;
  the sanitizer build caught it immediately. The views are stored by value now.
- A narrowed view's `null_count` no longer describes it, so it becomes -1 ("not computed") and
  callers scan the window. Zero stays zero -- a null-free column has null-free windows, which keeps
  the fast path.

The matrix no longer excludes `struct` from its sliced case, which is what let this through.

## Reading Lance datasets that pylance has MODIFIED

An audit of nanolance against *mutated* pylance datasets -- not just freshly written ones -- found
five silent wrong answers. Every stock-Lance test in the suite wrote its dataset exactly once, and
that turned out to be load-bearing.

| pylance operation | pylance sees | nanolance returned |
|---|---|---|
| `write` + `append` | 4 rows | 2 rows |
| `write` + `overwrite` | 2 rows | 3 rows (the pre-overwrite data) |
| `delete("id < 10")` | 90 rows | 100 rows -- deleted rows returned |
| `update(...)` | new values | stale values |
| `add_columns({"b": "a * 2"})` | 10 rows / 2 cols | 10 rows / 1 col -- column dropped |

No error in any case. Three independent causes.

### 1. Manifest naming, which sorts in opposite directions

Lance has two schemes (`rust/lance-table/src/io/commit.rs`, `ManifestNamingScheme`):

    V1: _versions/{version}.manifest              <- what nanolance writes
    V2: _versions/{u64::MAX - version}.manifest   <- what pylance writes, 20-digit zero-padded,
                                                     so the NEWEST version sorts FIRST

Picking the numerically largest filename is right under V1 and **exactly backwards** under V2, so
every multi-version pylance dataset read back as version 1. Confirmed by renaming a dataset's
manifests into V1 form, after which the same reads came back correct.

The **writer** mattered as much as the reader, in two ways I did not anticipate:

- `next_version` carried its own private copy of the naive scan, so appending to a pylance dataset
  numbered the new manifest `u64::MAX` -- which reads back as version 0, behind everything.
- Stock Lance **refuses** a directory holding both schemes ("Found multiple manifest naming schemes
  in the same directory"). Writing nanolance's V1 names into a pylance dataset made it unreadable by
  the tool that created it. `publish_manifest` now adopts whichever scheme is already present.

### 2. Deletion files were not read at all

Lance records deleted rows in `_deletions/{fragment}-{read_version}-{id}.{suffix}` and picks the
shape by density: an **Arrow IPC file** of u32 offsets when sparse, a **roaring bitmap** above 5000.

Both are parsed directly, in `src/deletion_vector.cpp`, rather than through a library. The Arrow
route would otherwise mean enabling nanoarrow's `NANOARROW_IPC_WITH_ZSTD`, which pulls a
`find_package(zstd REQUIRED)` into a build that deliberately vendors zstd -- a portability
regression on exactly the platforms CI had only just started covering. So the file carries a minimal
flatbuffer table reader (Message and RecordBatch, a handful of fields, vtable walked directly) and a
roaring reader covering array, bitmap and run containers.

Two details worth keeping:

- The Arrow IPC **file** pads its magic to the writer's alignment -- pyarrow and arrow-rs pad to 64,
  not the 8 the format requires -- so the first message is not at a fixed offset. The parser skips
  forward over **zeros only**, never arbitrary bytes.
- Arrow prefixes each compressed buffer with its uncompressed length, and writes **-1** when the
  buffer did not compress. Both spellings occur in practice for these small files.

If the vector's length disagrees with the manifest's `num_deleted_rows`, the read is **refused**:
filtering by a vector that does not match would silently return the wrong number of rows, which is
the failure class this whole effort exists to close.

Applying them is `compact_column_values`, beside `slice_column_values` and for the same reason --
everything at that stage is byte-addressed except the validity bitmap, which is the one buffer that
has to be re-packed. Deletions run **before** any row range, because a range counts logical rows and
those only exist once the deleted rows are gone. `count_rows` subtracts them too, or it would
disagree with `len(read_table(...))`.

### 3. A fragment can span several data files

`add_columns` writes the computed column to its own file *beside* the original. Those files are not
more rows -- they are more **columns of the same rows**. The reader emitted one batch per file,
which dropped everything after the first file's columns; and the schema mapping compounded it by
deriving "is this field materialized" from `files[0]` alone, so the added column looked absent
entirely. A fragment is now one batch however many files it spans, and the files must agree on their
row count or the read is refused.

That also retired the row-range restriction added with ranges: multi-file fragments no longer need
refusing, because each fragment maps to exactly one batch with a known row count.

### Found on the way, NOT fixed

Reading a pylance **nullable string** column fails at some sizes with
`unsupported definition-level encoding: InlineBitpacking(16)` -- reproducible at 400 rows, fine at
100 and at 2000. It is a loud refusal rather than a wrong answer, and it is an encoding gap rather
than a lifecycle one, so it is recorded here rather than fixed alongside the above.

## Closing the gaps that let those bugs through

Two silent-corruption bugs landed on this branch within a day of each other. Both were found by
accident -- one by a streaming test, one by writing the conversion demo -- and neither had to be.
This section is about why, and what now prevents a third.

### A write -> pylance matrix, because "no test wrote that shape" is the root cause

The dictionary bug survived for months because nothing in the suite wrote a scattered low-cardinality
string column. Individual encodings had tests; the *matrix* did not exist.

`test_write_encoding_matrix.py` is now that matrix: 21 shapes x 3 write modes (`structural`,
`--compress`, `--no-structural`), each written and read back by **both** nanolance and stock Lance.
21 encodings covered -- integer bitpacking, RLE, constant, string constant, scattered dictionary,
dictionary+RLE, high-cardinality string and binary, float32/64 byte-stream-split, 1-bit bool,
fixed_size_binary, struct -- plus a nullable variant of each path that has one, since the
definition-level layer sits in front of the value encoding and is a different code path, not the
same one with a bitmap.

Three things it asserts that a narrower test would not:

- **Every column read on its own**, not just the whole table. The dictionary bug's signature was an
  *unrelated* column failing to read, because Lance validates the entire page table before decoding.
  A whole-table assertion alone reports that as a mystery.
- **One dataset holding every shape at once.** Writing each shape to its own file cannot catch a
  column taking its neighbours down with it -- which is precisely what happened.
- **The same shapes fed as sliced, multi-fragment writes.** Encoding choice and batch slicing are
  independent axes and the `ArrowArray::offset` bug lived in their product.

Verified in both directions: with the dictionary fix reverted, 7 of the 67 cases fail -- including
`struct` (its string child picks the dictionary) and both combined-dataset cases. Today all 67 pass.

### The warning ratchet was never switched on

`NANOLANCE_WERROR` defaults `OFF` and **no CI workflow set it**, so every job compiled nanolance's
own targets with warnings entirely disabled. That is the family the macOS `std::min` break came from.

Turning it on cost 8 findings, all of them dead code or vestigial:

| | |
|---|---|
| `control_buffer_for(vector<MiniblockChunk>&)` | dead overload; `control_buffer_for({chunk})` resolves to the scalar one |
| `page_layout_bytes(const LanceField&, uint64_t)` | dead |
| `build_array_from_field` + `append_string_values` + `append_fixed_values` | 200 lines left behind by the 4.1/3.1 refactors |
| `find_mapping_field_by_name` | dead, and containing unreachable `return true;` after `return nullptr;` |
| `append_varint` in blob_v2_external.cpp | dead |
| `have_value` in the generated protobuf reader | set, never read |
| `ArrowBufferView view{value.data(), size}` | clang `-Wmissing-braces`: `data` is a **union**, so the flat form picks its first member by accident rather than intent |

No latent correctness bugs -- which is the point: the ratchet is cheap to close *now* and gets more
expensive with every month it stays open. It is enabled in `linux-bench.yml` (clang-19) and
`memory-safety.yml` (gcc-13), so both compilers are covered, and verified clean locally under gcc,
clang and the ASan/UBSan configuration.

The option stays `OFF` by default on purpose: a downstream build on an unknown compiler must not
fail because of a warning nanolance's CI has never seen. The scope is nanolance's own three library
targets -- not FetchContent'd dependencies, the tools or the tests. (`tools/nlance_pagelayout.cpp`
trips a GCC 13 false positive from inside libstdc++'s `stl_algobase.h`, which is exactly why the
policy does not extend there.)

### Two more tests that reported success for failure

`smoke_bool_pylance_interop.sh` and `smoke_bss_zstd_pylance_interop.sh` still carried the
`except Exception: sys.exit(77)` removed from the dict script -- ctest reports 77 as a SKIP. Both
pass today, so the handler was dormant, but it was a live tripwire that would turn a future
regression into a green build. Gone. That is the third and fourth instance of this pattern found on
this branch (after the dict script and `smoke_pagelayout_pylance_interop.sh`'s exit-0), so it is
worth stating as a rule: **in this repo a test may skip for a missing dependency and for nothing
else.**

## Phase 4.1: decode into the Arrow buffer instead of copying into it

Taken next, out of plan order, because the measurement above showed this -- not streaming -- is what
removes the 2x read peak.

The decoder produces each column into a `std::vector<uint8_t>`, and every `fill_*` helper then
`ArrowBufferAppend`ed it into the Arrow buffer. Both were live at once. They now **adopt** the
vector's storage instead: the vector moves to the heap and the `ArrowBuffer` takes ownership of it
through `ArrowBufferDeallocator`, which nanoarrow documents for exactly this ("avoid copying an
existing buffer that was not allocated using the infrastructure provided here"). The copy becomes a
move.

Measured on a 61 MiB, 4M-row table, and on a 103 MiB table for the timings:

| | before | after |
|---|---|---|
| peak RSS, 1 fragment | 2.01x | **1.01x** |
| peak RSS, 16 fragments | 1.10x | **0.94x** |
| read, median of 7 | 201 ms | **160 ms** |

So ~20% off the read as well, which is the one benchmark where nanolance still trailed `lance`.

Two details worth keeping:

- **An empty vector is deliberately not adopted.** `ArrowBufferReset` only calls the deallocator when
  `data != NULL`, so adopting a zero-length vector would leak the heap object it was moved into. An
  empty Arrow buffer is already the right representation.
- **`bool` still copies once, unavoidably.** It is a byte per value on disk and a *bit* per value in
  Arrow, so the bits must be packed somewhere; they are packed into a fresh vector which is then
  adopted. One copy instead of two.

The regression guard is `test_read_peak_is_about_one_copy_of_the_dataset`, and it was checked in both
directions: it fails at 2.00x against the old build and passes at 1.01x against the new one. It uses
a 4M-row table on purpose -- at ~30 MiB the old build measured only 1.50x, close enough to the 1.5x
threshold to pass by luck on another machine.

Verified under ASan/UBSan/LSan (46/46) as well as the plain build, since this hands raw ownership
between C++ and Arrow by hand; `fuzz_decode` clean over 1,106,008 runs.

---

## Phase 4.2 — two per-row loops, and one measurement that said "stop"

`append_repeated_value` (constant-encoded columns) wrote one value at a time. It now writes the
value once and doubles it forward, so a page of N identical values costs `log2(N)` memcpys:

| 4M rows | before | after |
|---|---|---|
| constant int64 | 12.3 ms | **4.5 ms** |
| constant string (26 bytes) | 69 ms | **66 ms** |

The string row is the interesting one, and it is reported rather than buried: 4M copies of a 26-byte
value is ~100 MiB of stores, and the fill already ran at ~1.7 GB/s (3.8 MiB / 4.0 ms, 91.6 MiB /
50.8 ms, 381.5 MiB / 224.8 ms — linear). That path is memory-bandwidth bound. Only an encoding that
never materializes the bytes (Arrow REE, or a dictionary array) would move it, and that changes the
column's Arrow type, so it is a separate decision rather than something to slip in here.

Profiling the result found a larger target that was not on the plan. `decode_variable_width_page`
appended each row's offset through its own `vector::resize()`; callgrind put that one function at
**32% of a whole read's instruction count** — not the four bytes it copies, but the size/capacity
round trip per call. A page's offsets are now grown in one resize and written through a typed
pointer. The two paths that genuinely cannot batch — FSST, and dictionary indices, where a row's
length is only known once the value is resolved — keep the per-row append but reserve once per page.

Reading a 1M-row `int64 + double + string` dataset three times:

| | before | after |
|---|---|---|
| instructions | 373.0M | **235.6M** (−37%) |
| wall clock | 87 ms | **77 ms** (best of 20, interleaved) |

`memcpy` is now 53% of the profile, which is the honest floor for a reader that materializes Arrow
buffers.

### 4.3 was closed by measuring it, not by implementing it

The plan's next item was a default-initializing allocator for the byte buffers, sized at ~6.5% by a
measurement taken *before* 4.1 and 4.2. It does not survive them.

After 4.2 the remaining zero-fill is the offset resize above, and callgrind still bills it at 5.1%
of instructions. That number is an artifact: **callgrind counts `rep stosb` once per byte**, so it
reports ~12 MB of zeroing as ~12M instructions. The same function was rewritten to `insert()` the
page's offsets (a copy, with no zero-init) and shift them in place — which gets the whole
default-init saving with no allocator and no change to a public type. Interleaved, best of 20:

```
resize + write   80 ms
insert + shift   81 ms
```

Indistinguishable. So the tree keeps the simpler version, and `ColumnValues` does not grow a custom
allocator parameter for nothing. This is the second time in this work that an instruction profile
promised a win wall clock refused to pay — the plan's own §2.4 warned about this exact fix, and the
warning was too mild.

---

## Phase 4.4 — the profiler was hiding the biggest win

4.4 was "mmap the data file", on the theory that the cost of `ifstream` page reads is the zero-fill
and the kernel→heap copy. `strace -c` disagreed. Three reads of the 1M-row dataset:

```
 48.17%   19,065   newfstatat
 26.68%    9,544   read
 23.00%    9,546   lseek
```

Nearly half the syscall time was `stat`, and none of it was doing anything. The reader keeps a small
LRU of open data files and re-validates size + mtime on lookup, because a path can legitimately name
a *different* file later — fragment names come from the lowest unused suffix, so rewriting a dataset
directory reproduces `fragment-0.lance` with other bytes, which nanolance's own test suite does. That
check is right. Running it on **every page-buffer read** is not: the answer cannot change partway
through a single read.

`DataFileReadScope` marks a batch read as one operation. A file is validated on its first lookup
inside the scope and reused for the rest of it. Outside any scope the epoch is 0, which never matches
a stamped entry, so every lookup validates exactly as before — the fast path is opt-in, not something
a caller loses by forgetting to opt out. The reader also remembers where the stream is, so a page
that starts where the last one stopped skips its seek.

```
newfstatat   19,065 -> 15
lseek         9,546 -> 8,793
wall clock    78 ms -> 68 ms      (best of 15, interleaved)
```

`tests/test_data_file_reader.cpp` guards it, and the guard took two attempts. The first version
replaced the file with `ofstream` truncation and passed even with validation ripped out entirely —
truncating in place keeps the inode, and a cached handle follows the inode, so nothing was being
tested. It now unlinks first, and checks both directions of the stale-size bound: a read past a
*shrunken* replacement's end must be refused rather than served from the old inode, and a read inside
a *grown* replacement must be allowed rather than refused by the old length. Against a
never-revalidate build it fails on the first of those; against a never-seek build it fails on the
forward jump.

### And then mmap was closed, by pricing what was left

With the stats gone, the whole remaining file-read path — seek, read, copy into the page buffer —
prices at **~5% of a read** (every page read run twice: 74 ms → 78 ms over three reads). That is
mmap's entire ceiling, against a `docs/SAFETY.md` rewrite, its own fuzz pass, and SIGBUS on a
truncated file. Closed.

Same verdict, same method, for a refactor that was not on the plan at all: `MiniBlockChunkView` is
named "View" but copies each chunk out of the page payload, and callgrind blamed those copies for
**25.6%** of a read's instructions. Running the split twice priced them at 3.8% (78 ms → 81 ms). The
refactor would thread a span type through six decoder helper signatures and give up the
`std::swap(chunk.values, raw)` the zstd and LZ4 paths rely on. Not at that price.

### 4.5 — the ratio, stated honestly

CI already publishes the callgrind breakdown beside the bench, so what 4.5 owed was the number.
`bench/run-local-bench.sh` on this branch, best of 7 reads, each engine reading its own file:

| 200k rows | parquet (zstd) | rust lance | nanolance | vs rust lance |
|---|---|---|---|---|
| `pcap_ref` | 6.34 ms | 5.64 ms | **1.51 ms** | 3.7x faster |
| `wide_int` | 3.48 ms | 3.25 ms | **1.55 ms** | 2.1x faster |
| `high_card` | 7.47 ms | **5.53 ms** | 8.04 ms | **1.45x slower** |
| `float_smooth` | 3.50 ms | 9.29 ms | **1.73 ms** | 5.4x faster |
| `bool_flags` | 2.00 ms | 1.79 ms | **1.10 ms** | 1.6x faster |

Parity was the goal and four of five shapes are past it, so the caveats matter more than the wins:

- This ran in a **shared container, not the CI runner**. Only comparisons within one run mean
  anything — the 11.01 ms this plan quoted as the gap was measured on a different machine, and
  subtracting the two would be arithmetic on noise. `bench/linux-ci-results.md` stays authoritative.
- **`high_card` is the shape that is still slower**, and it is the one every profile in this phase
  pointed at: high-cardinality strings, the variable-width path, memory-bandwidth bound once the
  per-row bookkeeping is gone (4.2). nanolance writes it more slowly too (28.42 ms vs 17.96 ms).
- **rust-lance reading a nanolance file takes 10.90 ms on `pcap_ref`, against 5.64 ms for its own.**
  Interop works; it is not free for the other reader.

### What Phase 4 actually taught

Three reads of a 1M-row `int64 + double + string` dataset: **87 ms → 68 ms (−22%)**, on top of 4.1's
201 ms → 160 ms and 2.01× → 1.01× peak RSS.

Every item in this phase was decided by wall clock, and **the instruction profile was wrong about all
four of them**:

| | callgrind said | wall clock said |
|---|---|---|
| per-row offset append | 32% | 11% — worth doing |
| chunk-view refactor | 25.6% | 3.8% — not worth doing |
| default-init buffers (4.3) | 5.1% | 0% — not worth doing |
| file-read path (4.4) | 3.9% | ~24% — the largest win left |

`rep stosb` and `rep movsb` are counted once per byte, so memset and memcpy look enormous; syscalls
are one instruction to count and microseconds to make, so they vanish. Use `callgrind_annotate` to
find candidates. Never to size them.

---

## Phase 5 — the writer's front door, and one CLI instead of four

**One options struct (5.1).** The writer's options were eight `set_*` calls that all had to precede
the first `write_batch` — a rule enforced at runtime with `INVALID_STATE` and nowhere else.
`NanoLanceWriteOptions` + `nano_lance_writer_open` take them together, so there is no "after" for
them to be in. Both `init` forms and every setter stay and now delegate, so nothing that compiled
before stops compiling.

One decision is worth recording because it looks like a wart and is not: **a zeroed struct means the
defaults**, which forced the one option that is ON by default to be spelled as a negation,
`disable_structural_encoding`. A positive `structural_encoding` field would make
`NanoLanceWriteOptions options = {0}` silently turn structural encodings off — precisely the class of
trap this item exists to remove, reintroduced in the fix for it. The C++ `WriteOptions` has member
initializers and spells it positively.

**An RAII writer (5.2).** `nano_lance::Writer` (`nanolance/writer.hpp`) owns the handle, so an early
return closes it. Errors stay in the house style (`bool` + `error()`, not exceptions); only the
lifetime changes. `commit()` defaults `is_append` to whatever `open()` was given and flips to `true`
after the first successful commit, so the argument that was easiest to get wrong has a right answer
by default.

`tests/test_writer_api.cpp` is about **equivalence**, not coverage: the options struct against the
equivalent setters, a zeroed struct and a `NULL` pointer against plain `init`,
`disable_structural_encoding` against `set_structural_encoding(false)`, and the RAII writer against
the hand-rolled C sequence — each compared as **byte-identical data files**. A second way to write
files would be worse than none.

**One CLI (5.3).** `nanolance import` / `info` / `cat` / `stitch`, replacing `arrowipc2lance`,
`nlance_info`, `nlance2table` and `nlance_stitch` as the surface a user is meant to find. The old
names stay, and deliberately: each tool's source is compiled **twice** — once into its standalone
binary, once into `nanolance` with `NANOLANCE_CLI_SUBCOMMAND` defined, which drops its `main()` and
leaves the subcommand entry point. One implementation, two front doors. Deleting the four names would
have broken every script that calls them while buying nothing visible; duplicating the argument
parsing to keep them would have created exactly the drift this was meant to remove.

`tests/smoke_nanolance_cli.sh` pins the join: the same Arrow IPC input through both front doors must
produce byte-identical data files, `cat` and `info` must produce identical text, and a value-taking
flag (`--compress -l 9`) must survive the forwarding **and** be shown to have changed the output —
otherwise the comparison proves nothing. It fails against a dispatcher that drops one forwarded
argument, which is how it was checked. Subcommands print `argv[0]`, so `nanolance info` says
`Usage: nanolance info`.

Two stale claims surfaced while wiring this up, and were corrected rather than carried across:

- `arrowipc2lance --help` still said **"nanolance cannot store nulls yet"**. It has since 1.1.
- `nlance_info`'s hint suggested re-ingesting with `--rows-per-fragment`, **a flag no tool has ever
  had**. One `import` run commits exactly one fragment however many IPC batches it reads, so the hint
  now says to split the input and re-run with `--append`. The real gap — no way to split fragments
  within a single run — is listed below rather than papered over.

**5.4** turned out to be already done: `append_fixed_values` went with 4.1's rewrite.

---

## Reading pylance's nullable columns: three gaps, all closed

The stock-Lance suite had been testing nulls at one or two sizes. Nulls are the wrong axis to spot
check: Lance does not have *a* definition-level encoding, it has several, and which one a column gets
depends on the row count and the null pattern rather than on anything the writer was asked for.
Walking the cross-product of five types x five null patterns x ten sizes, each cell compared against
**pylance's own read**, found three distinct failures — one of which made ordinary datasets
unreadable. All three are fixed, and all 250 cells now match.

### Closed: `InlineBitpacking(16)` definition levels

A nullable string column of a few hundred rows was refused outright:

```
unsupported definition-level encoding: InlineBitpacking(16)
```

The note in PROGRESS said this happened "at 400 rows, fine at 100 and 2000", which undersold it
badly. Measured: **200, 300, 400, 500, 700 and 1000 rows all fail**; 100 and 2000 happen not to. That
is not an exotic corner, it is a normal dataset.

`InlineBitpacking(16)` is the same FastLanes block a bitpacked *value* page carries, with its bit
width stored as the buffer's first `u16` instead of in the page descriptor — which is what "inline"
means. Confirmed on disk before writing any code: a 200-row page's level buffer is 130 bytes, and
`[u16 width=1][64 u16 packed words]` is exactly 130. `unpack_bitpacked_page<uint16_t>` already
decodes that shape for value pages, so the fix is to route the levels through it. The 16 is the
uncompressed element width and Lance's levels are `u16`, so it is the only width that can occur;
anything else is still refused by name rather than guessed at.

All 13 sizes from 100 to 20000 now match pylance exactly.

### Closed: the chunk header is not a fixed shape

The other two failures both came back to the chunk header, and reading `decode_miniblock_chunk` in
`rust/lance-encoding/src/encodings/logical/primitive.rs` settled what the real grammar is:

```
[u16 num_levels]
[u16 rep_size]                    only when rep_compression is present
[u16 def_size]                    only when def_compression is present
[num_buffers x (u32 if has_large_chunk else u16)]
pad to 8
[rep] pad8  [def] pad8  [buffer_0] pad8  [buffer_1] pad8 ...
```

nanolance read a **fixed** `[u16 num_levels][u16][u16][u16]`. That is the right 8 bytes for exactly
one shape — no repetition layer, two value buffers, `has_large_chunk` false — which covers everything
it had been tested on, and silently misreads the rest. The header now comes from the page's
descriptor: `has_repetition` (a new flag, since f1 `rep_compression` was being skipped and its `u16`
slot shifts every later field), `def_compression`'s presence, `num_buffers` and `has_large_chunk`.

1. **A `float64` column with a run-shaped null pattern** (`rle chunk buffer sizes invalid`). Lance
   RLEs both values and levels, so the page sets `has_large_chunk` and its buffer sizes are `u32`:
   the real header is `[u16 200][u16 def_size=14][u32 1208][u32 151]` plus `fe fe fe fe` padding to
   16 bytes. Read as four `u16`s, the definition block was located 8 bytes early.

   The RLE branch also had a **second, hand-rolled copy** of the chunk parse, assuming
   `[u16 num_levels][u32 size0][u32 size1]` — correct only for a *non-nullable* RLE column. It now
   uses the shared split like every other branch, which is the point: the duplicate parser is how the
   two drifted apart in the first place.

2. **A `bool` column over 1024 rows** (`definition-level chunk covers more than one FastLanes
   block`). Bools pack a bit per value, so a byte-sized chunk holds far more of them than of anything
   else — 1025 at the first size that overflows. The level buffer is then a whole packed block
   followed by a raw `u16` tail (128 + 2 = 130 bytes, exactly what that page contains). The decoder
   now walks blocks, using Lance's own rule from `unpack_out_of_line` for whether the tail is packed
   or raw: `raw ⟺ words == whole_blocks * packed_words + tail`, rather than approximating it.

**All 250 cells of the sweep now match pylance** — 5 types x 5 null patterns x 10 sizes from 100 to
20000. `bindings/python/tests/test_lance_nullable_matrix.py` is that cross-product, plus a named test
per failure so a future break says which shape regressed instead of only which parameter tuple.

Both fixes were checked in the other direction too: forcing `large_buffer_sizes` false fails the
float64 tests, and restoring the one-block cap fails the bool tests. Being decode changes on the
untrusted read path, `fuzz_decode` was re-run over them: 3,346,817 runs, clean.

---

## Choosing an encoding by whether it pays, not by type

Asked where high-cardinality reads still lag rust-lance, I wrote the same dataset with both and
compared the page descriptors. The answer was not in the decoder:

| column | rust-lance picks | nanolance picked |
|---|---|---|
| `id` — random `uint64` | `Flat(64)` | `InlineBitpacking(64)` |
| `label` — random 16-char string | `Fsst{...}` | `General(ZSTD)+Variable` |

nanolance bitpacked **every** integer column, unconditionally, because integers are bitpackable.
That is a claim about the type, not the data, and it is wrong in two ways:

- **Values that need the full width** pack to exactly their own size, plus a width word per chunk. A
  column of ids, hashes or uuids therefore comes out *larger* bitpacked, and pays a FastLanes
  transpose on every read to get back what it started with. Measured on 200k random `uint64`:
  **0.85 ms → 0.50 ms per read (−41%)**, file 2.4% smaller.
- **A FastLanes chunk always covers 1024 values, padded.** Any integer column shorter than that was
  inflated to a whole block. An 8-row `int64` column's page payload was **528 bytes for 64 bytes of
  data**; the dataset as a whole went 1041 → 499 bytes. For the "simple parquet user" this project
  is courting, whose first file is small, that was a 2× penalty on the very first thing they write.

The rule is now Lance's own (`rust/lance-encoding/src/compression.rs`): cost the packed form per
1024-value chunk — one width word plus `1024 * width / bits` words — and bitpack only when that is
strictly smaller than raw. Mirroring the reference implementation rather than inventing a threshold
is deliberate; "how much saving is enough" is exactly the kind of number that gets picked once and
never re-examined.

It was the writer's own test that caught the second case: `test_writer_api.cpp` asserted that
disabling structural encoding *changes* the bytes, and after this change its six-row sample encodes
identically either way — because at six rows bitpacking correctly loses. That test now uses a
constant column, where structural encoding has something unambiguous to do.

`bindings/python/tests/test_write_encoding_choice.py` pins both directions, including the one that
matters most: **a narrow column must still be bitpacked.** Without it, "only bitpack when it pays"
could quietly become "never bitpack" and every other assertion would still hold. Verified by
reverting the check: the small-column and incompressible-column tests fail, the narrow-column one
does not.

### What is left on high_card, and it is not the decoder

After this, `high_card` reads 7.67 ms against rust-lance's 5.12 ms. Callgrind says where the rest
goes: **zstd decompression is ~48% of the read** (`ZSTD_decompressSequences` alone is 37%). That is
not waste — zstd earns its size on that column (~2.6× on the string data; nanolance's file is
*smaller* than lance's, 18.0 vs 18.9 B/row). It is a deliberate trade that rust-lance declines by
using **FSST**, which decompresses at roughly memcpy speed because it is a symbol-table
substitution rather than an entropy coder.

nanolance already *reads* FSST. Writing it is the real answer to this shape, and it is a genuine
piece of work (an FSST encoder), not a tweak — so it is listed below rather than guessed at here.

---

## The one parser with no fuzzer had an unbounded expansion in it

Asked whether nanolance should be using `nanom` (the sister parser-combinator library) for its binary
parsing, I went looking at what the hand-rolled parsers actually are. The audit's most useful output
was not an answer about nanom: it was noticing that `src/deletion_vector.cpp` — 547 lines of
untrusted-input parsing added in this same session — was **the only parser in the tree without a
libFuzzer target**. `fuzz_decode`, `fuzz_page_layout`, `fuzz_fsst` and `fuzz_lz4` cover everything
else, and none of them reach a deletion file: `fuzz_decode` stops at the data file and never opens
one.

`fuzz_deletion_vector` now covers both entry points. It found something within minutes.

### A 4-byte run container can emit 65,536 values

The roaring format amplifies violently, and nothing bounded it:

| container | bytes on disk | values out | amplification |
|---|---|---|---|
| run | **4** | up to 65,536 | **~65,000x** |
| bitmap | 8,192 | up to 65,536 | 32x |
| array | 2 per value | 1 per value | 2x |

With up to 65,536 containers, roughly **640 KiB of crafted input buys 16 GiB of allocation**. The
fuzzer reached 614 MiB of RSS on inputs under 8 KiB. Every other decode path in the reader budgets
its output against `default_read_limits()`; this one did not.

The `num_deleted_rows` cross-check at the end of `read_deletion_vector` does not help: it catches the
bad file only *after* the allocation it was supposed to prevent. A budget is only a budget if it is
checked before each expansion, so the check now sits inside the container loop — and the real caller
passes the manifest's `num_deleted_rows` as the cap, which is not a heuristic but the exact number of
values an honest file will produce.

Getting that right took three passes, and the middle one is worth recording because it looked fine.

The first version budgeted a bitmap container at its 65,536 maximum rather than its real count, which
refused a perfectly ordinary pylance dataset of 9,000 deleted rows. Fixing that was free and
stricter: the descriptive header already states each container's cardinality, and for a bitmap
container that *is* its popcount — so the parser counts the bits, **refuses a container whose bit
count disagrees with its own header**, and budgets on the true value.

Then a re-fuzz said peak RSS had gone from 614 MiB to **1047 MiB** — worse, not better. (A mid-run
reading of 403 MiB had looked like an improvement; it was not the peak, and reporting it was a
mistake.) The reason was the *default* budget: the standalone entry point falls back to the reader's
generic per-buffer limit, `max_uncompressed_bytes / 4` — 2.1 billion values. Consistent with the rest
of the reader, and meaningless as a bound on a deletion vector.

So the check moved to where the answer already was. The descriptive header states every container's
cardinality, so the total is summed and checked **once, before any value is materialized**, and two
per-container invariants keep that header honest — a bitmap container's popcount and a run
container's expansion must each equal the cardinality it declared. Without the second, a run
container could claim a cardinality of 1, sail through the up-front total, and then expand 65,536
values per run.

That version also reserved the output vector to the declared total, which the fuzzer refused to let
stand: **it found a 4 GiB `malloc` from an input of a few kilobytes on the next run.** The declared
total is a number the *file* chose; passing the budget proves it is permitted, not that it is real,
so reserving it hands a small file a huge allocation directly. The reserve is now capped at 1M
offsets (4 MiB, past any real deletion file) and everything beyond that is paid for one container at
a time, each measured against the budget before it expands.

### ...and then it found a second one, in the other parser

With the roaring path bounded, the next run produced two reproducers that both began `ARROW1`. The
Arrow IPC reader trusts each buffer's int64 uncompressed-length prefix, checked only against the
reader's generic 8 GiB ceiling — so **a 328-byte file declaring 4 GiB got a 4 GiB zeroed
allocation**.

My first fix for it was wrong, and **CI's fuzzer rejected it in twelve seconds.**

I cross-checked the Arrow prefix against `ZSTD_getFrameContentSize`, reasoning that the frame header
is "the authority". It is not: a zstd frame header's content-size field is written by whoever wrote
the frame, so it comes out of the same untrusted bytes as the Arrow prefix. Making the two agree
proves only that the file is self-consistent about its lie — set both to 4 GiB and the check passes.
That is a consistency check, not a bound, and I shipped it as a bound.

The real bound is the one number the file does not choose: **the manifest's `num_deleted_rows`**,
times four bytes per `uint32` offset. `parse_arrow_ipc_uint32_column` now takes the same `max_values`
cap `parse_roaring_bitmap` does, and `read_deletion_vector` passes it. The frame cross-check stays,
correctly labelled as a consistency check.

Both reproducers are checked in at `tests/fuzz/corpus/deletion_vector/` and CI passes that directory
to the fuzzer, so they are replayed on every push rather than waiting to be rediscovered.

### ...and with the allocations no longer hiding it, a crash

The next run reached 528,552 executions and produced a `deadly signal`: `std::length_error` escaping
`parse_arrow_ipc_uint32_column`. **An uncaught exception from a malformed file is a crash, not a
refusal** — the function's whole contract is to return `false` with a message.

The record batch's int64 row count was validated as `values.size() < rows * 4`. `rows` is already
known non-negative, but past 2^62 that product **overflows uint64 and wraps small**, so the check
passes and the `resize` that follows throws. It now uses `checked_mul`, the helper the rest of the
reader has used all along.

This is the overflow-naive spelling the nanom audit noticed a few paragraphs above and judged "safe
given their input ranges, but by argument rather than by construction". That judgement was right
about the roaring parser and wrong here — which is the whole reason the distinction matters.

### ...and, once that was gone too, an out-of-bounds read

The next run's tail carried an **AddressSanitizer heap-buffer-overflow: a one-byte READ past the end
of the input buffer**, in the same function. This is the most serious of the set — the others were
denial of service, this one reads memory the file does not own.

`flatbuffer_field` ended with `return out_absolute <= b.size()`, so an offset *equal to* the buffer
size counted as a **present** field, and the caller indexed it. Every flatbuffer scalar is at least
one byte, so "present" has to mean strictly inside. Two fixes, deliberately overlapping: the contract
is now `<`, and the two single-byte reads go through a bounds-checked `flatbuffer_byte` instead of
indexing — because a caller that trusts a proof made somewhere else inherits every future weakening
of it.

### What the exercise says

Six rounds, and the fuzzer found something in each: a missing budget, a budget that rejected honest
files, a 4 GiB reserve the *fix itself* introduced, the same allocation bug in the other parser, a
crash the allocation bugs had been masking, and finally — once the crash was gone — an out-of-bounds
read that had been sitting behind all of them. None would have been found by a test someone thought
to write.

The order matters and is worth stating: **each bug was only reachable once the one in front of it was
fixed.** A single fuzz run would have found the first and stopped. The memory-safety bug was six
deep.

Final state, one completed 10-minute run against the checked-in corpus:

| | first run | final run |
|---|---|---|
| executions | 25,023 in 241 s | **8,784,055 in 601 s** |
| throughput | ~104/s | **~14,600/s** (140x) |
| peak RSS | 614 MiB | **383 MiB** |
| coverage | 520 edges | **1,016 edges** |
| result | unbounded expansion | **clean** |

The throughput and coverage numbers are the interesting ones: the parser now refuses malformed input
immediately instead of allocating into it, so the fuzzer reaches twice as much code per unit of time.
A harness that spends its budget in the allocator is not fuzzing, it is benchmarking `malloc`.

**A caveat that belongs with these numbers:** CI runs each target for 120 seconds. The out-of-bounds
read surfaced roughly nine minutes into a local run, so CI would not have found it. What CI gives is
replay of the checked-in reproducers at startup — a regression guard, not a discovery engine. Real
discovery wants a nightly job with a persisted corpus, which does not exist yet.

That is the argument for the harness, more than any individual finding — and it reframes the question
that started this. "Should the core use nanom?" matters much less than "does every parser have a
fuzzer?", which until today was **no**.

### And the nanom question itself

Not today, and mostly for reasons that are not about nanom's quality:

- **The biggest piece is protobuf**, and nanom has no varint/LEB128 combinator. `lance_minimal.pb.cpp`
  (784 lines) and `page_layout.cpp` (661) are protobuf wire-format parsers — 1,445 of the ~2,300
  hand-rolled lines are in the one shape nanom does not cover.
- **nanom is C++23; nanolance is C++20**, and `ci-platforms` (macOS 14 + Windows 2022) has only just
  gone green. Raising the standard for the core library is a real portability cost.
- **README line 7 is a promise**: "nanolance itself depends only on nanoarrow + zstd". Parsing stacks
  are deliberately framed as an *example's* dependency, not the library's — which is exactly how
  nanom is already wired in (`examples/pcapng2lance_nanom`, opt-in submodule, OFF by default).
- Most honestly: **the bugs this session found were grammar bugs, not bounds bugs.** A combinator
  parser with the wrong idea of the chunk header is just as wrong as a hand-rolled one. nanom would
  have helped with the *duplicated* RLE chunk parser, and would make the bounds-check style uniform —
  the tree currently mixes overflow-safe (`size - min(pos, size)`) and overflow-naive (`pos + n >
  size`) spellings, all of which happen to be safe given their input ranges, but by argument rather
  than by construction.

If that changes, `deletion_vector.cpp` is the candidate, not the protobuf paths: Arrow IPC framing and
roaring containers map cleanly onto `length_data` / `length_count` / `take` / `verify`.

---

## The read matrix, and the gaps it found

Asked where coverage was thin, the honest way to answer was to look at where the bugs had actually
been, not at which files lacked a test named after them. Every stock-Lance defect in this project has
been on the **read** side, and the reason is structural: when nanolance writes, it chooses the
encoding; when it reads a pylance file, Lance chose, **and Lance chooses by row count**. The same
column is flat at 100 rows, dictionary-encoded at 1024 and run-length encoded at 5000.

The write direction was well covered — `test_write_encoding_matrix.py`, 21 shapes x 3 modes, both
readers agreeing. The read direction had spot tests and the nullable matrix, nothing systematic.

`test_lance_read_matrix.py` is the counterpart: 20 column shapes x 6 row counts, each written by
pylance and compared against pylance's own read. **It failed 14 of 114 cells the day it was
written**, in four classes, none of which any existing test touched:

| shape | descriptor Lance produced | symptom |
|---|---|---|
| `struct`, every size | leaf columns only, no physical column for the parent | `struct field has no children in mapping` |
| `time64` ≥1024 | `dict=General{LZ4,Flat(64)}` | `dict block header invalid` |
| `decimal128` ≥20000 | `dict=General{LZ4,Flat(128)}` | `dict block header invalid` |
| `str_runs` ≥5000 | `Rle{Flat(32),Flat(8)}` over `General{LZ4,Variable}` | buffer sized 16388, needed 20004 |

Three distinct causes behind them:

1. **Structs written by pylance** — **fixed**, and not for the reason the descriptor suggested. See
   below: it was a protobuf defaulting bug, not a missing decoder.
2. **Dictionaries whose values are fixed-width** — **fixed**. The dictionary block comes in two
   shapes and the descriptor says which. `Variable` is a block with an offset header, which is what a
   low-cardinality string column gets. `Flat(N)` is N-bit values end to end with **no header at
   all**, which is what Lance builds for a temporal or decimal column once its cardinality justifies
   a dictionary. One branch read both, so a flat block's first value was read as an offset header and
   refused with `dict block header invalid` — a message naming the symptom, not the cause.

   The descriptor already carried everything needed: `Flat`'s `bits_per_value` for the entry width
   and `num_dictionary_items` for the count, since a headerless block states neither itself. The
   decode then sizes its output once and writes through a pointer rather than maintaining offsets,
   because every row is the same width. Two checks guard it: the descriptor's entry width must equal
   the width the column's logical type implies (they always agree in a file Lance wrote, and
   trusting either one alone when they disagree would silently shift every value), and the block must
   be long enough for the entries it claims — Lance pads it, so longer is fine and shorter is not.
3. **Dictionary + RLE'd indices** — **fixed**. The branch read the page as ONE chunk in nanolance's
   own private framing (`[u16 num_levels][u32 size0][u32 size1]`). Lance writes the ordinary
   miniblock grammar, with as many chunks as the page needs, so a page past 1024 values decoded short
   and then failed downstream with a buffer-size mismatch that named nothing useful. It now goes
   through the same chunk splitter every other miniblock path uses, which also means a nullable
   column's definition levels are read rather than mistaken for run data.

   Chasing it turned up **two more dictionary shapes the matrix had never reached**, and neither was
   exotic: an ordinary `int64` column with runs. See below.

All four were pinned in the matrix by their **specific** message, so each fails loudly the moment it
is fixed — which is exactly what happened to the struct entry. `test_the_gap_list_is_not_stale` additionally asserts every entry still matches a cell the
matrix generates — otherwise trimming a size would orphan an entry and leave the suite advertising a
gap nobody reproduces.

The general point, which is the answer to "what needs more tests": **a format reader's coverage axis
is the writer's choices, not the reader's types.** Enumerating types found none of this; enumerating
types x sizes found all of it in one run.

---

## A constant column can have nulls, and we returned the wrong data twice

Found while checking something else: an all-zero `int64` column with a null every eleventh row read
back with **no nulls at all**. No exception, no warning — just different data from what pylance
returns out of the same file. That is this project's worst failure class, and it had been there the
whole time.

Lance writes a constant column as `ConstantLayout`, and the layout is decided by the inline value's
presence and the page's buffer count **together**:

| inline value | buffers | meaning |
|---|---|---|
| yes | 0 | the value, no levels |
| yes | 2 | the value; buffer 0 = rep, buffer 1 = def |
| no | 1 | buffer 0 = the value, no levels |
| no | 3 | buffer 0 = the value; buffer 1 = rep, buffer 2 = def |

`ConstantPageScheduler::try_new` refuses every other combination. Reading only half of that table
produced two separate wrong answers, in opposite directions:

1. **A fixed-width nullable constant lost its nulls.** The definition buffer was never read —
   `def_compression` and `num_def_values` were not even parsed out of the descriptor. Every row came
   back as the value.
2. **A variable-width nullable constant lost its values.** "All null" was inferred from *no inline
   value AND a definition layer*. But a variable-width constant **never** has an inline value — it
   keeps its value in a buffer — so every nullable string or binary constant read back entirely
   null. The rule is the buffer count, not the inline value.

The second one is the more instructive. The original code was not a missing case; it was a correct
observation ("Lance spells an all-null column with a definition layer and no inline value") promoted
to a rule it could not support. It held for every column anyone had tested because fixed-width
constants do carry their value inline.

The levels are raw u16, one per row, always. `ConstantLayout.def_compression` exists in the proto but
applies only to the all-null path; a page carrying a value borrows the buffer as a `u16` slice and
nothing else. pylance also leaves `num_def_values` at 0 there, so the page's row count is the only
statement of how many levels exist — the descriptor genuinely cannot answer this on its own.

`test_lance_constant_nulls.py` pins it: 9 types x 5 null patterns x 3 row counts, plus a direct check
on `null_count`, since a bitmap that is right row by row but carries a stale count is a malformed
Arrow array that a `to_pydict()` comparison cannot see. Both halves were verified by reverting them
separately — 82 cells fail without the first, 18 without the second.

**What this says about the testing.** Every null test in the suite used a column with more than one
distinct value, so no test ever produced a page that was constant *and* nullable. The encoding
matrix had `int_constant` and `str_constant`; the nullable matrix had `int64` and `string`. The bug
lived in the product of two axes that were each covered alone. That is the same shape as the sliced-
struct bug, and it is worth stating as a rule: **when two axes each have their own matrix, the bugs
are in the cells neither one visits.**

The same cell was missing from the *write* matrix. It turns out to be correct there — 144 cases, 6
types x 4 null patterns x 2 sizes x 3 modes, all round-tripping through nanolance and through
pylance — so the fix is on the read side only. `null_int_constant`, `null_str_constant` and
`null_float_constant` are now in the write matrix anyway: "we checked and it was fine" is worth
keeping as an assertion rather than as a memory.

---

## A dictionary block has four shapes, not one

The three read gaps were supposed to be three fixes. The third one turned into a fifth and a sixth,
and the way that happened is the useful part.

The dictionary branch had one idea of what a dictionary block is: a variable-width block with an
offset header, `[u32][u32 bytes_start][u32 offsets...][bytes]`. That is what a low-cardinality
**string** column gets, and strings were the only dictionary anyone had tested. Lance picks a
dictionary for any column whose cardinality justifies one, and for a fixed-width column it writes
something else entirely. There are four shapes, and only the page descriptor distinguishes them:

| descriptor | block | who gets it |
|---|---|---|
| `Variable{offsets=Flat(32)}` | offset header, then bytes | a low-cardinality string column |
| `Flat(N)` | N-bit entries end to end, no header | `time64` ≥1024 rows, `decimal128` ≥20000 |
| `InlineBitpacking(N)` | FastLanes blocks, width word per block | `int64` with runs, ~164 entries |
| `Bitpacked{N, Flat(w)}` | FastLanes blocks, width in the descriptor | `int64` with runs, ~3000 entries |

Any of them may in turn sit inside `General{LZ4, ...}`.

The last two are the same encoding with the bit width written in two different places, and **Lance
switches between them as the dictionary grows**. That is why enumerating types would not have found
them: `int64` with runs is one column shape, and which spelling it produces depends on how many
distinct values it happens to have. 65,536 rows of `i // 400` gets the inline one; 300,000 rows of
`i // 100` gets the out-of-line one. Both were unreadable, and an integer column with runs is about
as ordinary as a column gets.

Two pieces of code already existed and neither was reachable from here. `unpack_bitpacked_page`
decodes a single inline block, which is right for a miniblock chunk (the chunk splitter has already
cut the buffer up) and wrong for a dictionary buffer, which holds however many blocks its entries
need and has to be walked. And the out-of-line tail arithmetic — Lance's `unpack_out_of_line` rule,
where a tail is packed-and-padded or raw depending on the buffer's total length — was sitting inside
`append_definition_levels`, specialised to `u16`. It is now a template both callers share, because
this is precisely the kind of subtle arithmetic that two copies get wrong separately.

Likewise the dictionary-block parse itself, which the plain-index and RLE'd-index branches held two
copies of. They had already drifted: the fixed-width fix landed in one and not the other, which is
why the RLE'd-index branch still could not read a `time64` column with runs after fixed-width
dictionaries were "done". One `DictionaryBlock` now serves both.

**How they were found.** Not by reading the format spec — by widening a test until it broke. A
row-count axis found the first gaps; a *cardinality* axis found these. Both are the writer's
decision variables, which is the same lesson as before in a new dimension: **enumerate what the
writer chooses between, not what the reader has branches for.** A sweep of 13 types x 3 run
densities x 3 row counts x nullable — 234 cases — now passes end to end against pylance.

---

## The struct gap was a protobuf defaulting bug

The first of the three read gaps looked like a missing decoder and was not. The symptom pointed
straight at the schema mapper -- `struct field has no children in mapping` -- and the page dump
agreed: a pylance struct has no physical column for the parent, only its flattened leaves. The
obvious reading was that nanolance expected a parent column that Lance does not write.

That reading was wrong, and one experiment showed it. Put **any** column ahead of the struct and the
same file reads perfectly:

```
lance.write_dataset(pa.table({"s": struct_col}), ...)               -> FAILS
lance.write_dataset(pa.table({"lead": ints, "s": struct_col}), ...) -> reads fine
```

A missing decoder does not care what position a column is in. Field **ids** do.

Lance's `Field.parent_id` is a proto3 `int32`, and **proto3 does not put a zero on the wire**. A
struct in the first column gets id 0, so its children carry `parent_id = 0` -- which is never
serialized. Our decoder initialized `parent_id` to `-1` and therefore read the absence as *"I am a
root"* rather than *"my parent is field 0"*. Both children detached, the struct was left childless,
and the whole dataset failed to open. Roots are unaffected because Lance writes `-1` for them, and
`-1` is non-zero, so it is always on the wire. The bug was reachable only through the single value
protobuf refuses to transmit.

The fix is three rules, each independently defensible:

* absent `parent_id` means **0**, per proto3;
* a field cannot be its own parent, so field 0 with no `parent_id` on the wire is a root;
* a `parent_id` naming a field that is not in the schema re-roots instead of orphaning -- `drop_columns`
  can remove field 0, and a column that cannot be placed in the tree should be visible at the top
  rather than vanish from the output.

**Why no test caught it.** `test_type_support_matrix.py` had `struct` in `ROUNDTRIPS` and it passed,
because both of its directions start from a file *nanolance wrote* -- and our encoder always emits
field 4 explicitly, zero or not. The matrix was missing a third direction: **pylance writes it,
nanolance reads it**. That test now exists over every round-trippable type, and it is the one that
fails when the fix is reverted. The C++ side pins the rule directly, on hand-built wire bytes, since
no round-trip through our own encoder can produce a message with field 4 absent.

The generalizable lesson is narrower than "test both directions" and worth stating exactly: **a
round-trip test through your own encoder cannot see a decoder's defaulting bugs**, because your
encoder never produces the omission that triggers them. Any field whose proto3 default differs from
the value your struct initializes it to is a live bug waiting for a foreign writer. `Field.type`
(our default 2, proto3's 0) and `Field.encoding` (our default 1, proto3's 0) are the same shape;
neither is currently load-bearing on the read path, but both are noted here rather than left to be
rediscovered.

---

## The type surface, swept in both directions

Asked whether encoders or decoders were still missing, and specifically about lists, I swept every
Arrow type through both directions rather than answer from the backlog.

**Round-trips through nanolance, and stock Lance reads the result** (24): every int and uint width,
`float32`/`float64`, `bool`, `string`, `binary`, `fixed_size_binary`, `date32`/`date64`,
`time32`/`time64`, `timestamp` with and without a timezone, `decimal128`, `decimal256`, `struct`, and
nested `struct`.

**Refused on write, cleanly and by name** (12): `float16`, `duration`, `large_string`,
`large_binary`, Arrow `dictionary`, Arrow `null`, and every list shape — `list`, `large_list`,
`fixed_size_list`, `list<struct>`, `struct<list>`, `map`. No crashes and no silent corruption
anywhere, which is the property that matters most: writing a type it cannot represent is the failure
this project has already had once.

**Three read/write asymmetries, all deliberate:** `large_string`, `large_binary` and Arrow's `null`
type **read back correctly from a pylance dataset** while being refused on write. For the large
types the reason is recorded at the refusal — Lance keeps u32 offsets *inside* the chunk and signals
the 64-bit Arrow width only in the descriptor, so writing them means decoupling two widths across
five page paths; reading them needs none of that. For `null`, the reader already understands Lance's
all-null `ConstantLayout` and the writer has no path to emit it.

### Lists: no, and they fail earlier than expected

Not supported in either direction, and the read side does not fail in a decoder — it fails at the
**manifest's schema**, before a page is touched: `unsupported on-disk logical type for manifest
recovery: list`. Lance's repetition layer is parsed only far enough to know it exists
(`has_repetition`, which the chunk header arithmetic needs); nothing consumes it.

That makes lists the largest single type gap, and now the largest read gap of any kind, the three
above it being fixed: repetition levels are a whole layer, not a missing branch. Every nested shape built on one
goes with it — `list<struct>`, `struct<list>` and `map` (which Lance represents as a list of
key/value structs).

`test_type_support_matrix.py` pins all of the above: the round-trips both ways, a clean named
refusal for each unsupported type, the three read-only asymmetries, and the unreadable set by
message. It is the project's capability statement, executable.

---

## Deviations from the plan, and open items

Kept honest: everything here was found and reproduced during this work. Ordered by what a new user
is most likely to hit.

**The forward plan is [`docs/ROADMAP.md`](ROADMAP.md)**, which supersedes the ordering below. Writing
it against freshly measured pylance files found two gaps missing from this list, and both outrank
lists: the **FullZip** page layout (any string column with one value of 256 bytes or more, and every
embedding of 64+ float32 dimensions, is unreadable) and **`fixed_size_list`**, which Lance encodes as a
compressive wrapper rather than with repetition levels.

### Correctness / reach — worth doing next

1. **No known read gaps for pylance-written columns.** All three are fixed, and `KNOWN_GAPS` in
   `test_lance_read_matrix.py` is now empty — deliberately kept, rather than deleted, so the next one
   has an obvious home and cannot be parked as an xfail. What replaced them at the top of this list
   is lists and `map` (below): a whole missing layer rather than a missing branch.

2. **No FSST on write.** The direct answer to the one bench shape nanolance still loses
   (`high_card`: 7.67 ms vs rust-lance 5.12 ms). zstd is ~48% of that read; rust-lance avoids it by
   writing FSST, a symbol-table substitution that decodes near memcpy speed. nanolance already
   *reads* FSST, so this is an encoder, not a format change — the largest single remaining read win,
   and a real piece of work.
3. **Pages are much smaller than Lance's.** nanolance emits ~1024 rows per page for bitpacked
   columns; pylance put 200,000 items in **one** page for the same data. 306 pages vs 2 on the
   `high_card` dataset means hundreds of extra page-buffer reads, each a seek plus a read. The
   `DataFileReadScope` win came from the same place, which suggests the remaining per-page overhead
   is worth measuring before anything more exotic.
4. **`large_utf8` / `large_binary` are refused on write.** Lance keeps u32 offsets *inside* the
   chunk for large types and signals the 64-bit Arrow width only in the page descriptor, so
   supporting these means decoupling the chunk offset width from the declared Arrow width across
   every variable-width page path. The exact fix is recorded where the rejection is raised.
5. **Arrow `null`-typed columns are refused, and the message is now wrong.** It says nanolance
   "cannot store nulls yet", which stopped being true at 1.1. The reader already understands Lance's
   all-null spelling (`ConstantLayout` + definition-level layers); the writer just has no path to
   emit it. Small, and the stale wording should go either way.
6. **Arrow dictionary columns are refused.** Deliberate and explained in the error (nanolance would
   keep the indices and drop the values), with a working remedy — cast to the value type and let
   nanolance's own on-disk dictionary do it. Worth revisiting only if a user hits it.
7. **`list` types are unsupported end to end, and so is everything built on one.** Lance's
   repetition layer is parsed only far enough to know it exists (`has_repetition`, which the chunk
   header needs); nothing consumes it. On write a list never reaches an encoder; on read it fails at
   the manifest's schema, before any page. `list`, `large_list`, `fixed_size_list`, `list<struct>`,
   `struct<list>` and `map` all go together — Lance represents a map as a list of key/value structs.
   This is the largest type gap and a bigger piece than items 1-6: repetition levels are a layer, not
   a branch.

   Also missing, and much smaller: `float16` and `duration`, neither of which has a decoder or an
   encoder, both refused cleanly.

### Ergonomics

8. **`nanom` in the core: done for the protobuf metadata** (see "Lance metadata on nanom's protobuf
   codec" below). The C++23 requirement stays inside `nanolance_proto`'s one .cpp file.
   `deletion_vector.cpp` and `page_layout.cpp` (also protobuf, hand-parsed) are the next
   candidates.
9. **No way to split fragments within one `nanolance import` run.** It commits exactly one fragment
   however many Arrow IPC batches it reads, so a large input becomes one large fragment — which
   `nanolance info` then warns about. Splitting means re-running per chunk with `--append`. Found
   while correcting a hint that advertised a `--rows-per-fragment` flag no tool has ever had.
10. **Plan item 2.2, CMake install/export, is still not done.** There is no `install()` rule in the
   tree at all, so `find_package(nanolance)` cannot work and the only way to consume the library is
   to vendor it or point at a build tree. Deferred during Phase 2 and never picked back up; it is the
   oldest genuinely-open item here.

### Verification gaps

11. **No nightly fuzz job.** CI runs each target for 120 s, which is a regression guard (it replays
    the checked-in reproducers) rather than a discovery engine — the out-of-bounds read above took
    ~9 minutes of local fuzzing to surface. A scheduled job with a persisted corpus would find the
    next one; nothing does today.

12. **The wheel build had never completed on this branch**, and when a run finally got far enough to
   finish, it showed why it would never have passed: `test-command` used the wrong cibuildwheel
   placeholder. `{project}` is the working directory cibuildwheel was called in — the repository
   root — while `{package}` is the package directory, `bindings/python`. The smoke test lives under
   the package, so every wheel job built and repaired its wheel successfully and then died at the
   last step:

       can't open file '/Users/runner/work/nanolance/nanolance/tests/wheel_smoke.py'

   Fixed to `{package}`. The lasting part is `test_packaging_config.py`, which resolves the
   placeholders exactly as cibuildwheel does and asserts the referenced file exists — it reproduces
   this failure locally in milliseconds instead of at the end of the longest CI job. A config path
   that only runs inside a wheel job is otherwise unverifiable until it is too late.

   **Now verified.** The first run after the fix produced **10 Linux
   wheels** (manylinux_2_28 + musllinux_1_2, x86_64, CPython 3.9–3.13) and the macOS 14 arm64 set,
   every one of them installed into a clean virtualenv and smoke-tested. The log shows the
   placeholder resolving and the test importing the *installed* package rather than the repo, which
   is the property the smoke test exists to prove:

       + sh -c 'python /project/bindings/python/tests/wheel_smoke.py'
       nanolance 0.2.0 from /tmp/.../venv/lib/python3.13/site-packages/nanolance/__init__.py
       wheel smoke passed

   `macos-13 x86_64` was the remaining job, and it was **not** slow runners, which is what it looked
   like and what I first wrote here. **GitHub retired the macOS 13 image.** The label no longer
   exists, so the job is never scheduled at all — and a retired label does not error:

       a job whose runner label no longer exists sits `queued` forever.

   That is why this hid so well. The wheels workflow had **34 runs and not one ever completed**.
   Every one of them showed as `cancelled`, because `cancel-in-progress` kills the run when the next
   push lands, and a job still queued at that moment is reported as cancelled rather than as stuck.
   The backlog note blamed the cancellations on pushing faster than cibuildwheel takes. That was
   true, and it was the wrong conclusion: the cancellations were *masking* a job that could never
   have started, not causing one.

   Fixed by moving Intel macOS to `macos-15-intel`, the current x86_64 label. `macos-14` (arm64) is
   marked deprecated but not retired, so it stays, with a comment naming `macos-15` as its
   replacement — the same silent-hang failure is waiting behind that label whenever it goes.

   The lesson is about the failure's *shape*: a stuck job and a busy queue look identical, and
   neither one is red. The workflow now says so where it will be read: if a job is queued far longer
   than its siblings take to finish, check the label still exists before assuming capacity.

   **The wheels workflow has now completed, for the first time in 35 runs.** `macos-15-intel`
   started one second after the run began — against `macos-13`, which never started in 47 minutes —
   and the full matrix is green:

   | job | wheels |
   |---|---|
   | `ubuntu-24.04 x86_64` | 10 (manylinux_2_28 + musllinux_1_2, CPython 3.9–3.13) |
   | `macos-14 arm64` | 5 |
   | `macos-15-intel x86_64` | 5 |
   | `sdist` | source distribution |

   20 wheels and an sdist, each installed into a clean virtualenv and smoke-tested against the
   *installed* package. Both bugs show in a single log line — the path resolving under
   `bindings/python`, on a runner that exists:

       + /bin/sh -c 'python .../bindings/python/tests/wheel_smoke.py'
       nanolance 0.2.0 from .../venv-test-x86_64/lib/python3.13/site-packages/nanolance/__init__.py
       wheel smoke passed

   `pip install nanolance` is therefore a verified on-ramp on Linux (glibc and musl), Apple silicon
   and Intel macOS. Windows is still out of the matrix; `ci-platforms.yml`'s `windows` job is the
   thing that has to go green first.

## Roadmap phases A, B and E: vectors, long strings, and small types

What a pylance user writes and nanolance could not read, until this round: **any string or binary
column with a value of 256 bytes or more** (Lance switches that page to the FullZip layout), and
**any `fixed_size_list` column** — which is how every embedding is stored. Both now read, and
fixed-size lists write.

- **FullZip reader** (non-list). Byte layout settled empirically and written up in the roadmap (B1):
  a 0/1/2/4-byte control word per row, then a fixed slot (present even for a null row) or a length
  prefix plus bytes (absent for a null row), per-value FSST or raw.
- **fixed_size_list, read and write.** Lance keeps no child field for it; the element type lives in
  the logical type `fixed_size_list:<elem>:<N>`, and the page wraps Flat in `FixedSizeList{N}`. Read
  from both layouts, including **element validity**: pyarrow marks every element of a null row null,
  so pylance writes element bits for an *ordinary* nullable vector column — refusing them refused
  every nullable embedding column. Element bits are carried to Arrow's child array and re-cut by
  row ranges and deletion vectors (tests fail if that slicing is reverted). On write, whole-row nulls
  work; a null element inside a valid row is refused by name, pinned in the type matrix.
- **float16, duration, Arrow's null type** round-trip in all three directions.

Bugs found on the way, each now pinned by a test:

1. **A constant fixed-width column of 128+ bytes wrote a corrupt file.** The value was inlined in
   the page descriptor with a one-byte length, so 128+ bytes produced a descriptor neither reader
   could parse. Lance caps inline constants at 32 bytes; nanolance now does too, and wider constants
   take the flat path (`fsb_constant_32/33/200` in the write matrix).
2. **proto3 defaults.** `Field.type` and `Field.encoding` had the `parent_id` bug's shape: an absent
   field decoded as nanolance's own default rather than proto3's 0. Neither was load-bearing yet.
3. **The page decoder was never fuzzed.** A new target, `fuzz_column_decode`, feeds a real
   descriptor and buffers — dumped from pylance datasets by `nlance-pagelayout --dump-fuzz-pages` —
   straight to the decoder. It found an **8.4 GB allocation from an 899-byte page** (a zstd size
   header was trusted; now bounded by zstd's 32,768:1 maximum expansion, in the deletion-file path
   too), and an **out-of-bounds read** on a variable-width chunk shorter than one offset.

**Correction.** An earlier commit message on this branch said `fuzz_decode`'s coverage rose from
1008 to 1193 features "so the new paths are reached". That was wrong: `fuzz_decode` stops at the
footer and never decodes a page, so the new decoders were not reached at all. The rise came from
other parsing. `fuzz_column_decode` exists because of that mistake, and README's "fuzzer over the
full decode chain" — true only of the chain up to the footer — now says what is actually covered.

4. **Reading a pylance string column was quadratic.** Found because CI's new page-decoding fuzz
   step ran at 15 inputs a second, two seeds taking 13–16 s each. The FSST decoder called
   `reserve(size() + worst_case)` once per value, and `reserve` grows to exactly what it is asked
   for, so nearly every value reallocated and copied the whole column so far. A 20,000-row page
   took **2,099 ms; now 3.9 ms**, same output. Four more places had the same exact-size reserve once
   per chunk, page or batch; all now use one geometric `reserve_more` helper. Fuzz throughput on
   CI's seeds went from 15 to 1,458 inputs a second. `test_a_large_fsst_page_reads_in_linear_time`
   pins it: 100,000 strings took 21 s before the fix and must now read in under 3.

Still open from these phases: B5 (writing FullZip — nanolance writes long strings as MiniBlock,
which stock Lance reads, pinned by `str_long` in the write matrix), element nulls on write, and
page size: nanolance writes ~10 rows per page for a 768-dim float32 vector (roadmap F1).

## Roadmap phase C: reading list columns

`list` and `large_list` columns written by pylance now read — the largest type gap this project had.
Design first ([NESTED_COLUMNS.md](NESTED_COLUMNS.md)), then three pieces:

- **The unraveler** (`src/repdef.cpp`), a port of Lance's `RepDefUnraveler`: repetition and
  definition levels in, per-layer offsets and validity out. Pure computation, tested case by case
  against the vectors in Lance's own `repdef.rs`, and fuzzed on its own with Arrow's list invariants
  asserted on every accepted input (6.8M inputs, clean).
- **Decoding, page by page, in two steps.** Levels are unravelled; then the page's values are read
  by the *existing* decoders as a flat page of items, with each chunk's value count taken from the
  page's metadata words (a list chunk has more levels than values, so the old inference does not
  apply). Bit-packing, FSST, dictionaries and RLE therefore work inside lists with no second copy.
  A constant page — every list empty or null, or one repeated item — carries its levels as buffers.
- **Slicing and compaction per layer.** A row range selects one contiguous range at each layer down
  to the items; a deletion turns kept rows into kept items through every level. Reverting either to
  the flat code fails 43 and 21 of the new tests respectively.

Verified: 66 cases in `tests/test_lance_lists.py` — every flat leaf type, `large_list`,
`list<list<int64>>` with nulls at every level, null/empty lists, null items, dictionary and constant
pages, 60,000 rows across fragments and pages, row ranges and deletions — each against pylance's own
read. Still refused by name: a struct inside a list, a list inside a struct, `map`, FullZip list
pages, a list of `fixed_size_list`.

The page-decoding fuzzer, now reaching list pages, found two `memcpy`-from-null bugs (undefined
behaviour even for zero bytes): one in the new raw-level path, and one older one it could now reach
— width-0 bit-packing, which is legal (an all-zero level buffer packs to nothing), over an empty
buffer. Both fixed; both reproducers are in `tests/fuzz/corpus/column_decode`.

Two more, older, in code the wider seed set now reaches:

- **A heap-buffer over-read in the dictionary-block parser.** Its bounds check added two `u32`s, so
  an entry offset near 2^32 wrapped the sum back under the buffer size, passed, and handed the copy a
  ~4 GiB length. Now checked in 64 bits.
- **A 2.8 GB allocation from an 85 KiB page.** The earlier zstd fix bounded a frame's declared size
  by zstd's true maximum expansion (32768:1) — but that maximum is large enough to still allow this.
  A frame declaring more than 64 MiB is now decompressed as a stream, its buffer growing only as real
  output arrives; `test_a_zstd_frame_over_64_mib_streams_back` covers the legitimate side (a 100 MB
  value in a ~10 KB frame).

### A null struct read back as a struct of nulls

Found while building C5, in code that predates it, and silent: a pylance struct column with null
rows read back with those rows present and every field null — `{"b": None}` where pylance returns
`None` — and no error. A struct field's definition levels give "null struct" a level of its own,
above "null field"; the flat decoder treated every non-zero level as "null field", and the batch
builder never gave a struct array a validity bitmap at all.

Any column with a nullable layer above its item now takes the nested path, which already unravels
struct layers, and the batch builder fills each struct array's validity from them — once, with
every other field of the same struct checked to agree. Long strings (FullZip) and constant fields
under a null struct are covered too. `test_null_structs_read_as_null` pins six shapes, with ranges
and deletions; routing those columns back to the flat path fails all six.

The same change delivered C5: a list of structs has one leaf per field, sharing one list array and
one struct array; a struct holding a list is a struct layer above the list layers.

The page-decoding fuzzer, seeded with these struct pages, found one more old bug: the
`fixed_size_binary:N` width was parsed with `std::stoul`, which throws on a malformed type string,
and the exception escaped the reader — a crash, not a refusal. It is a bounded digit loop now; the
reproducer is checked in. A 10-minute rerun after the fix: 1.23M inputs, clean.

**Maps (C6)** read as Arrow's `map<K, V>`: Lance stores one as a list of `entries` structs, so the
nested path already handled it and only the schema mapping (`+m`, non-nullable entries and keys, as
Arrow requires) was new.

### Declared sizes cannot force an allocation, and nothing escapes the C API

Seeded with map pages, the fuzzer found one more: the inline-bit-packed dictionary reserved
`count x width` bytes from the descriptor's count before reading a block, 4 GiB from a small page.
It now reserves no more than the buffer could produce. Looking for the same pattern by hand turned
up the bigger one the harness cannot reach (it caps rows at 32,768): several paths reserved
`declared_rows x width` up front, and a constant page expanded to `rows x width` with no limit, so a
small file declaring 2^33 int64 rows asked for 64 GiB — and nothing on the read path caught
`std::bad_alloc`, so it would have escaped through the C API and terminated the calling process.

Three changes: speculative reservations are capped at 256 MiB (growth past that is geometric);
constant expansion is bounded by the decoded-size limit; and every C entry point, plus the stream's
`get_next`, now turns an exception into an error status and message. `test_read_safety` decodes the
2^33-row page and requires a refusal, not an exception; with the limit removed it fails.

## Roadmap phase D: writing lists, maps and null structs

nanolance now writes every nested shape it reads: `list`, `large_list`, `map`, lists of structs,
structs of lists — and null structs, which were refused on write until now. A parquet file with
nested columns converts with `nanolance convert` and stock Lance reads the result.

- **D1, the serializer** (`repdef::serialize`), inverse of the unraveler, property-tested against it
  over 20,000 random nested columns.
- **D2, ingest and pages.** Each leaf's path is walked from its top-level Arrow column; pages are one
  mini-block chunk each, cut at row boundaries, with raw `u16` levels and the repetition index Lance
  expects; a page with no values is a `ConstantLayout`, as pylance writes it. Leaves under structs
  that are never null keep the existing flat encodings.

Verified: `tests/test_lance_list_writes.py` writes every read-matrix shape and every null-struct
shape, reads each back with nanolance **and pylance**, through sliced batches and several fragments
and a range across a fragment boundary; the type and parity matrices move lists, maps and null
structs from "refused on write" to "round-trips". Two ingest offset rules (a list's child offset, a
struct's physical index) each fail tests when broken — the first needed a new test, because
`to_batches()` never produces a list whose child has its own offset.

Open: a list of `fixed_size_list` (refused by name).

### List pages compressed

The first list pages were correct but 4-16x larger than pylance's: raw `u16` levels, plain items,
one chunk per page. They are now written the way pylance writes them:

- **Levels bit-packed**, `Bitpacked{16, Flat(w)}` at the page's widest level (1-3 bits, typically):
  whole 1,024-level blocks plus a raw tail when that is smaller than padding a last block — the
  choice Lance's own decoder tells apart by buffer length.
- **Pages cut into 1,024-value mini-block chunks**, levels split between chunks by Lance's slicer
  rule (a chunk takes levels until it has covered its values; trailing empty/null lists go to the
  next). A row can now span chunks, so the per-chunk repetition index counts it where it ends. The
  old 32,000-level page budget -- and the refusal of any row longer than that -- is gone: a
  40,000-item row writes and is read back in full and by range by both readers.
- **Integer items bit-packed** (`InlineBitpacking`, as flat columns already were).
- **String items dictionary-encoded** per page when the page repeats itself (distinct values at most
  half the items, and dictionary + indices under 80% of plain), else plain `Variable`.

Data bytes for 20,000-row columns (nanolance before -> after, pylance):

| Column | before | after | pylance |
|---|---:|---:|---:|
| `list<int64>` small ints, nulls | 287,368 | 53,512 | 53,395 |
| `list<string>` repeating tags | 228,232 | 13,896 | 13,865 |
| `list<list<int64>>` | 348,296 | 68,680 | 68,586 |
| `map<string, int64>` | — | 60,312 | 60,221 |
| `list<string>` all distinct | — | 303,112 | 173,744 |

The last row was the remaining gap: pylance FSST-compresses high-cardinality strings, and nanolance
had no FSST *encoder* -- closed by roadmap F2 (below).

Verified: both readers on every shape of the write matrix, plus a size-regression test holding three
list shapes within 10% of pylance (`test_list_pages_are_about_as_small_as_stock_lance`); ctest plain
and under ASan/UBSan; the page-decoding fuzzer seeded with nanolance-written list pages (chunk-
spanning rows, dictionary items, constant pages) ran 732,113 executions clean, and CI now seeds it
with the same dataset.

## Roadmap C8: FullZip list pages

A list whose items include one of 256 bytes or more -- a long string, a document body -- is stored by
pylance as a FullZip page, which nanolance refused. It now reads them: one control word per level
(`rep << bits_def | def`), a value only behind the levels that own one, and the levels handed to the
same unraveler MiniBlock list pages use. The layout was taken from `serialize_full_zip_*` and
`ControlWordIterator` in Lance's source, not guessed.

Verified against pylance on six FullZip shapes (each confirmed FullZip with `nlance-pagelayout`):
`list<utf8>` with null lists, empty lists and null items (FSST values); `large_list<large_binary>`
(64-bit lengths); `list<list<utf8>>` with nulls at both levels; a list of nullable structs with a
long-string field; a nullable struct holding a list; and a map with long-string values -- each read
whole, by row range and after deletions, and each also written by nanolance and read back by both
readers. A 25 MB column that Lance cuts into two FullZip pages reads across the boundary.
Two mutations -- a level's visibility off by one, the repetition level not shifted out of the word
-- each fail the tests. The page-decoding fuzzer, seeded with these pages, ran 692,338 executions
clean; CI seeds it with FullZip list pages too.

Still refused by name: a list of `fixed_size_list` (pylance stores wide ones as FullZip too, but the
refusal is in the schema, before any page is read).

## Roadmap E4: writing `large_utf8` and `large_binary`

These were refused on write, with a reason that turned out to be wrong: the refusal said Lance keeps
u32 offsets inside a large type's miniblock chunk. Measured against pylance 12, it does the
opposite -- a `large_string` chunk holds u64 offsets (20 short strings: 232 bytes against 144 for
`string`), its descriptor says `Variable{Flat(64)}`, and Lance's decoder refuses 32-bit offsets for
a large type ("expected 64-bit offsets but got 32-bit offsets").

Lifting the refusal and running every string page nanolance writes through both readers showed
which paths were already right and which were not. Plain, nullable, constant, all-null, long and
binary columns were fine: the flat chunk writer already used the column's own offset width. Three
paths were not, and pylance refused each: the flat dictionary (both the bit-packed and the RLE
variant), whose dictionary block and descriptor were hard-coded to 32 bits; list, struct and map
items, whose chunks were built with u32 offsets; and the per-page dictionary for list items. All
three now write u64 words and `Flat(64)` for a large column.

That also exposed a **read** gap: nanolance's dictionary-block parser only knew the 32-bit layout,
so a low-cardinality `large_string` column written by pylance -- a 64-bit block,
`[u64 64][u64 start][u64 offsets][data]` -- failed with "dict block header invalid". The parser now
reads the width from the block's first word.

Verified: 15 shapes (plain, nullable, three dictionary variants, constant, all-null, empty, long,
binary, list items, list dictionary items, `large_list<large_binary>`, a null struct's field, map
values), each written by nanolance with and without zstd and read back by both readers with the
large type intact, and each written by pylance and read by nanolance
(`test_lance_parity.py::test_large_offset_types_*`); the type matrix moves both types from
"refused" to "round-trips". Before the fix, the same shapes failed in exactly the three paths above,
so the tests are known to catch each one. The page-decoding fuzzer, seeded with 64-bit dictionary
pages (pylance's and nanolance's), ran 662,109 executions clean; CI seeds it with one too.

## Roadmap F2: FSST on write

pylance compresses high-cardinality strings -- ids, URLs, free text -- with FSST, and nanolance only
read FSST, so on exactly that shape its files were 1.5-4x larger. nanolance now writes FSST too.

- **The encoder** (`fsst::train`, `fsst::compress_value` in `src/fsst.cpp`) builds the symbol table
  the way Lance does (`build_symbol_table` in `rust/compression/fsst`): six rounds over a ~16 KiB
  sample, counting symbols and adjacent pairs, keeping the 255 with the highest count x length
  (single bytes x8), and returning the table from the round that compressed the sample best. Three
  deliberate differences, none visible to a decoder: the sample is drawn with a fixed seed (the same
  input always writes the same file); a candidate is keyed by its bytes alone, so the same string
  reached two ways is one candidate; and matching never reads past a value's end, where Lance loads
  whole words and relies on a sentinel.
- **When:** Lance's own rule -- `utf8`/`large_utf8`, not binary, at least 32 KiB, longest value at
  least 5 bytes -- plus one more: a page is written FSST only if the compressed values plus the
  2,312-byte table are at least 10% smaller than plain, otherwise plain. With zstd requested, strings
  go FSST only if it pays on the column's first 32,768 rows; otherwise they keep zstd.
- **Where:** one table per column, trained on a sample of the whole column, stored in every page's
  descriptor. Training per page, as Lance does, doubled the write time of `high_card` for no
  measurable size gain. FSST pages use the multi-chunk page layout from the list work (1,024 values
  per chunk), so a flat string column that FSST takes goes through the same writer as list items --
  the single-chunk pages would have paid the table every 32 KiB.
- **Reading got faster too.** The decoder appended one symbol at a time and grew the output per value;
  it now decodes a whole chunk in one pass into scratch space sized for the worst case (every code a
  full 8-byte symbol), copying symbols as whole words. On the same `high_card` file: 10.0-11.2 ms ->
  7.35 ms, 78M -> 52M instructions.

Data bytes, 20,000 rows (before -> after, pylance):

| Column | before | after | pylance |
|---|---:|---:|---:|
| unique short ids | 308,552 | 186,504 | 200,188 |
| URLs | 988,744 | 250,952 | 285,500 |
| sentences | 1,081,352 | 286,152 | 284,156 |
| sentences with nulls | -- | 235,720 | 255,688 |
| `list<string>`, distinct items | 303,112 | 285,448 | 329,392 |
| random hex | -- | 418,184 | 421,820 |

`tools/bench.py`, `high_card` (200,000 rows, best of 5 writes / 7 reads; this machine is noisy):
size 18.45 B/row (zstd was 18.02); rust lance reading nanolance's file ~7 ms (was 10.8-18.7 ms);
nanolance's native read 7.6-8.0 ms (was 7.7-10.6 ms), still behind rust lance reading its own file
(6.1-6.7 ms); write 38 ms (was ~35 ms).

**It follows the structural-encoding switch.** The writer's column scan tags a string column that
neither dictionary encoding takes as `nanolance:packing = fsst`, and only a tagged column gets FSST,
so `set_structural_encoding(false)` still writes plain pages -- which is what four C++ tests use as
their uncompressed baseline, and which they caught.

**Two pre-existing bugs found on the way.** First, the reader planned a string column from its FIRST
page's descriptor and applied it to every page -- including page 0's FSST symbol table. Every page
carries its own descriptor: Lance trains a table per page, and a page too small for FSST is written
plain, which is exactly what nanolance now does with the last 100 rows of a 32,868-row column. The
plain page was decoded as FSST ("FSST code 120 is not in the symbol table") -- found by the 100 MB
single-value test, whose column is split across two pages. String pages are now planned one by one;
`test_a_column_mixing_fsst_and_plain_pages` fails without it. Second, a nullable plain string chunk could fill all 32,760 value
bytes its control word describes and ALSO carry up to 128 bytes of definition levels; the footprint
overflowed the word and stock Lance read past the page ("the offset + length of the sliced Buffer
cannot exceed the existing length"). 1,024 nullable ~33-byte strings were enough. Chunks carrying
levels now leave room for them. `test_nullable_chunks_leave_room_for_their_levels` fails without
the fix.

Verified: `tests/test_fsst_writes.py` -- eight shapes (ids, URLs, sentences with and without nulls,
`large_utf8`, list items, a null struct's field, map values), with and without zstd, read by both
readers whole and by range, and held within 5% of pylance's size; several fragments fed by sliced
batches, then a deletion; binary, small and incompressible columns still round-trip. C++ unit tests
round-trip every byte value, empty values, 0xFF as data and random noise through the serialized
table and the reader's own parser, and check training is deterministic. A new fuzz target,
`fuzz_fsst_encode`, asserts every value round-trips and never more than doubles (51,995 runs clean,
now in CI); `fuzz_fsst` ran 22 million decodes and the page-decoding fuzzer, seeded with
nanolance-written FSST pages, 704,805, all clean.

## A memory budget for writing (edge devices)

nanolance's writer buffers every batch until a commit and only then encodes and writes the fragment,
so the resident set while saving is the whole pending fragment plus the encoder's working space. The
only control was `max_rows_per_fragment`, Python-only and in rows -- not what a memory-constrained
device (the C++ user this library is most likely to have) has to budget.

`max_pending_bytes` -- in the C options struct and `nano_lance_writer_set_max_pending_bytes`, the C++
`WriteOptions` and typed `writer::options`, Python's `LanceWriter`, and both CLIs -- makes
`write_batch` commit a fragment itself whenever the writer's buffered data reaches the budget. The
count is the capacity of every buffer the writer owns plus borrowed caller buffers, which stay pinned
until the flush. The caller's code does not change: the final `commit(false)` appends whatever is
still pending, and is a no-op when a flush already took everything. It cannot be combined with blob
URI dictionary mode, which cannot append (refused by name either way).

Measured (fresh batches of 64 Ki rows, uint64 + uint32 + string, 60 MiB in total; RSS sampled per
batch through the Python writer):

| budget | peak writer RSS | fragments |
|---:|---:|---:|
| none | 73.4 MiB | 1 |
| 32 MiB | 46.9 MiB | 2 |
| 8 MiB | 15.4 MiB | 7 |
| 2 MiB | 7.6 MiB | 16 |

and in C++ (`tests/test_writer_memory.cpp`, kernel high-water mark): 52 MiB of rows peak at 138-145
MiB unbounded and 15-17 MiB under a 4 MiB budget. So plan for about 3-4x the budget plus one batch:
buffers grow by doubling, and encoding a fragment needs room of its own (FSST's worst-case scratch,
page buffers). Encoding at about twice the pending data is the next thing to shrink if it matters.

A false lead worth recording: measuring against input READ FROM A FILE with pyarrow showed the writer
"leaking" ~2 MiB per batch at any budget. The input's buffers were memory-mapped, and each page the
writer read for the first time joined the process's RSS. Writing the same batch 30 times, or fresh
in-memory batches, shows no growth at all -- which is how the numbers above were taken.

Verified: the C++ test checks the accounting after every batch (pending bytes stay under the budget
plus a batch), row order across flushes through nanolance's reader, the final commit after automatic
flushes (and as a no-op with nothing left), the typed writer with borrowed spans, the dictionary-mode
refusal through both the setter and the options struct, and the RSS effect itself (skipped only under
sanitizers, whose quarantine holds freed memory by design). `tests/test_writer_memory_budget.py`
covers every page layout that commits a fragment -- flat, dictionary, FSST, lists -- through both
readers, ranges across flush boundaries, the budget together with the row limit, a later append and
a deletion, explicit flushes between automatic ones, and the `convert --max-pending-bytes` flag.

## Roadmap F1: page size -- measured, no change

The item assumed nanolance's pages were ~1024 rows and that stock Lance paid for it. Neither holds
any longer. Bitpacked integer columns are written as 32,768-row pages of 32 chunks; flat columns
(random ids, say) keep 4,095-row single-chunk pages. Rust lance reading nanolance's files against
its own, warmed-up medians of 21 reads at 200,000 rows:

| dataset | rust reads rust | rust reads nanolance | pages (rust / nanolance) |
|---|---:|---:|---:|
| pcap_ref | 10.38 ms | 11.81 ms | 3 / 9 |
| wide_int | 3.83 ms | 5.09 ms | 4 / 22 |
| high_card | 7.75 ms | 7.93 ms | 2 / 56 |
| float_smooth | 11.45 ms | 8.87 ms | 2 / 74 |
| bool_flags | 2.40 ms | 2.86 ms | 1 / 7 |

A throwaway experiment routing every plain fixed-width column through the multi-chunk page writer
(the one lists and FSST use) left `wide_int` unchanged. The "6x slower" figure that motivated this
was a cold first read in a noisy run: the first `to_table()` in a process took 12 ms, the rest ~5.
Larger pages remain possible, but they cost write-side memory, which the memory budget (above) now
bounds -- so no page-size change, and no knob until someone needs one.


## Performance from the benchmark matrix

The first full run of the new benchmark matrix (`tools/bench_matrix.py`, 27 datasets, every reader on
every writer's file; report in `docs/BENCHMARKS.md`) found three things worth fixing before
publishing numbers: one real interop problem, write paths doing avoidable work, and reads paying
the allocator for memory they did not need. It also found two faults in the harness itself.

### The harness

- **Unequal input.** The C++ writer got 64K-row batches from an IPC file; the Python and Rust writers
  got the table as one chunk. Batching alone costs nanolance ~40% on a narrow column (a batch is
  copied into the writer's buffer), so every writer now gets the same 64K-row batches.
- **A cold process per C++ write.** `arrowipc2lance` was run once per timing, so every write paid a
  fresh heap's page faults (int64 ids: 40 ms cold, 23 ms warm). `nlbench --write` now loads the batches
  once and times repeated writes in one process, like the reads; `nlbench` reports `warm_median_ms`,
  the median without the warm-up run.
- **`binary_blobs` was not random.** It reseeded one of seven generators per row, so the column held
  343 distinct values and both writers dictionary-encoded it. It now draws from one stream.

### Pages of many chunks (F1, reopened)

nanolance wrote flat, bit-packed, bool and byte-stream-split columns one chunk per page. A 2M-row
int64 column was 1,954 pages of ~2.5 KB, and Rust Lance, which pays a set-up cost per page, read it in
104 ms against 18 ms for its own one-page file (int32: 97 vs 13; timestamps: 90 vs 22). F1 had been
closed on a 200,000-row measurement that claimed bit-packed columns were already multi-chunk -- they
were not, on this path. `MiniblockPageWriter` now gathers chunks into pages of ~512 KiB (every
non-final chunk a power of two, the final one's log2 nibble 0, as Lance requires), and Rust reads
nanolance's numeric files at its own speed. The page size was swept from 64 KiB to 8 MiB: Rust levels
off from ~512 KiB, nanolance's reader is best at 64-256 KiB and slows past that (each page is read into
one buffer before its chunks are copied out; 8 MiB pages made float64 reads 4x slower). A side effect:
byte-stream-split chunks are now capped at 16 KiB raw, leaving room for a zstd frame that does not
shrink under the 32 KiB chunk limit.

### Writes

Profiled with callgrind (instruction counts, then confirmed on wall clock):

- **The dictionary check sampled first.** Each string page hashed values until half the page was
  distinct before giving up -- a third of a string column's write. A page of 8,192+ items is now
  sampled (1,024 values across it) and its cardinality estimated with Chao1 (`d + f1^2/(2 f2)`), which
  a frequent value such as the empty string of null rows does not fool (a plain distinct ratio did:
  `string_nulls` kept the full pass). Only a page estimated at over twice the rule's limit skips it.
- **`try_emplace` instead of `emplace`** in the dictionary maps: `emplace` builds the node before it
  looks, so every repeated value allocated and freed one.
- **FSST.** Compression into a reused scratch buffer instead of a zero-filled 2x output per page; a
  fast path (one 8-byte load per step, a merged 64K-entry table that settles the two-byte, one-byte and
  escape cases at once, a zero-padded tail) checked against the plain loop by a new test.
- **Gathering chunk values** by one copy when the chunk's items are consecutive (nearly always), by
  `memcpy` into a pre-sized buffer otherwise, instead of an `insert` per value.
- **The rep/def serializer** counts levels in its first pass so the second allocates once, visits the
  innermost list's items in a loop rather than a call each, and stores `is_slot` as bytes, not
  `vector<bool>`.
- **The column-level run check** stops as soon as the runs cannot fit the one-chunk page it plans.

### Reads

A warm read of a 45 MB string column faulted in ~105 MB of fresh memory per read: glibc returns large
buffers to the OS on free, so the next read maps and faults them again, and a vector doubling its way
to size copies into fresh pages each time. Rust and pyarrow use allocators that keep memory.

- **Projected reservations.** After each chunk of a string column and each page of a nested one, the
  output is reserved for the whole column at the rate seen so far (plus an eighth, capped like the
  existing reservation) instead of doubling into place.
- **Dictionary pages sized exactly**: one pass sums the selected entries' lengths, then the values are
  copied into a buffer grown once.
- **Transparent huge pages for large outputs** (Linux, `madvise(MADV_HUGEPAGE)` on the untouched part of
  buffers of 8 MiB or more): 512x fewer faults where the system allows THP (`enabled` = `madvise` or
  `always`, the common defaults). This is what glibc's `glibc.malloc.hugetlb=1` tunable does for every
  allocation; here only for the buffers a read returns. At most one partly used 2 MiB page per buffer.
- **The unraveler** appends validity bits without a size check per bit.

### Before and after

The last commit against this one, the same inputs, both timed with this `nlbench` (in-process,
median of 7 warm runs, pinned to one core, glibc's default allocator), in ms:

| dataset | write before | write after | read before | read after |
|---|---:|---:|---:|---:|
| `int64_ids` | 27.1 | 21.2 | 17.1 | 7.1 |
| `int32_small` | 15.3 | 10.1 | 8.7 | 2.5 |
| `date32` | 17.3 | 12.0 | 9.0 | 4.8 |
| `timestamp_us` | 31.3 | 25.7 | 15.6 | 10.7 |
| `int64_nulls` | 37.0 | 32.4 | 17.3 | 11.5 |
| `string_ids` | 218.5 | 113.7 | 48.7 | 24.7 |
| `string_text` | 189.3 | 80.3 | 67.0 | 24.6 |
| `string_urls` | 184.8 | 104.2 | 57.2 | 24.1 |
| `string_long` | 229.9 | 146.3 | 112.4 | 35.9 |
| `string_nulls` | 191.7 | 110.3 | 54.6 | 25.5 |
| `string_lowcard` | 81.6 | 49.3 | 31.2 | 15.9 |
| `binary_blobs` | 76.0 | 74.0 | 51.7 | 13.9 |
| `vector_f32x128` | 121.8 | 118.5 | 55.3 | 32.0 |
| `struct_mixed` | 123.8 | 75.5 | 25.6 | 24.7 |
| `list_int64` | 133.7 | 86.2 | 81.3 | 45.1 |
| `list_string` | 103.3 | 72.1 | 51.0 | 37.1 |
| `list_struct` | 59.2 | 53.8 | 28.8 | 25.9 |
| `map_string_int64` | 110.9 | 100.7 | 58.1 | 53.6 |
| `float64_smooth` | 20.5 | 21.5 | 8.9 | 9.2 |
| `uint8_codes` | 3.5 | 3.5 | 0.6 | 0.6 |

Everything else moved within noise (±1 ms). Nested reads remain the widest gap to Rust (see
`docs/BENCHMARKS.md`): the unraveler and per-page assembly are instruction-bound rather than
allocation-bound, and are the next place to look.

Verified: 54 C++ tests (also under AddressSanitizer and UBSan), the Python suite, and the new
`tests/test_multichunk_pages.py` -- every fixed-width path (flat, bit-packed, bool, byte-stream-split,
nullable, decimal, fixed-size binary and lists) written as multi-chunk pages with both structural
settings and with compression, read back by both readers, row ranges and `take` across page
boundaries, deletions, and columns of 1 to 4,097 rows.

## Lance metadata on nanom's protobuf codec

`generated/lance_minimal.pb.cpp` no longer hand-writes protobuf. The Lance messages nanolance
reads and writes are declared once, in nanom's `nanom/formats/lance_protobuf.hpp`:

- Manifest, with every field the index and type work added;
- DataFragment, DataFile, DeletionFile, DataStorageFormat;
- Field, Schema, FileDescriptor, Metadata;
- ColumnMetadata, Page, Encoding;
- IndexSection, IndexMetadata, IndexFile.

nanom's protobuf codec reads and writes them, and this file only converts between that model and
nanolance's structs. The public header is unchanged.

nanom is header-only, pinned by commit and fetched with FetchContent
(`FETCHCONTENT_SOURCE_DIR_NANOM` points at a local checkout). It is compiled as C++23 in that one
.cpp file only: the public headers and every other target stay C++20.

**Same behaviour as the hand-written codec.**
- Unknown fields:
  - Fields nanolance does not model are kept and written back, through nanom's `pb_unknown`.
    This covers Field, DataFile, DeletionFile, DataFragment, Manifest and IndexMetadata.
  - The manifest's positions into its own file (fields 4 and 21) are dropped.
- Indices:
  - An index a commit does not touch is copied byte for byte (`IndexSection` holds each index as
    raw bytes).
  - An index whose coverage changes is re-encoded with its new fragment bitmap, keeping the rest.
- A side-by-side differential compiled the previous codec next to this one on 20,000 random
  manifests. These carried random unknown fields, deletion files, timestamps, flags, the config
  and table-metadata maps, and index-section positions. Three properties hold:
  - this decoder reads the previous encoder's bytes exactly as the previous decoder did;
  - the previous decoder reads this encoder's bytes the same way;
  - both round trips agree.

  5,000 index sections (made, rewritten with changed coverage, and untouched) agree the same way,
  and untouched indices are byte-identical. The one exception is a map entry with an empty key.
  It is no longer written, as prost leaves it out too, and the previous decoder rejected such
  entries.

**What the bytes look like now.** The output is canonical proto3, the same as Lance's prost
writer:
- zero and empty implicit fields are left out;
- repeated int32 / uint64 fields are packed (`DataFile.fields` / `column_indices`,
  `Metadata.batch_offsets`, column buffers, an index's field ids).

A 1,000-fragment manifest shrinks from 262 KB to 166 KB.

**Reading is more accepting.**
- Packed `batch_offsets` are read (they used to be skipped).
- A map entry without a key is read.

Truncated or malformed messages are still rejected.

**Checked:**
- All 56 ctest targets and the 1,826-test Python suite pass, including `test_index_preservation.py`,
  `test_scalar_index.py` and the pylance interop tests.
- New in `tests/test_lance_minimal_pb.cpp`:
  - a manifest written by the hand-written encoder, kept as bytes, decodes field for field;
  - deletion files round-trip;
  - unmodelled fields of a manifest, a field and a fragment survive a rewrite, while the in-file
    positions are dropped;
  - untouched indices are byte-identical, and a changed index keeps a field from a newer Lance;
  - negative field ids, column encodings and packed batch offsets round-trip;
  - truncations and groups are rejected.
- `tests/smoke_deletion_append_pylance.sh`: rows deleted with pylance stay deleted after a
  nanolance append. This was a real bug in the first hand-written encoder. Main fixed it in the
  same period; this test now guards it.
- nanom's own tests cover the codec:
  - the protobuf spec's golden bytes;
  - Google's protobuf as an oracle in both directions, including unknown-field preservation;
  - fuzzing.

**Speed** against the previous codec:
- Micro-benchmark, 1,000-fragment x 50-column manifest, best of 7, three runs:
  - manifest encode: 1.12–1.23x faster;
  - manifest decode: 1.17–1.39x faster;
  - column metadata: 2.0–2.2x faster to encode, 1.3x faster to decode;
  - decoding manifests the previous encoder wrote: at parity.
- End to end, `arrowipc2lance` on tmpfs, median of 7 alternating runs, three runs:
  - appending to a 4,000-fragment dataset with a BTree index: 0.93–1.09x, i.e. parity (process
    start-up dominates);
  - creating a 2M-row dataset: 0.98–1.11x.

### Page descriptors through nanom

`src/page_layout.cpp` no longer parses protobuf by hand. A page's `/lance.encodings21.PageLayout`
descriptor is decoded with nanom's model (`nanom/formats/lance_encodings.hpp`). The model's
encoding children are `pb_lazy`, so the descriptor tree is decoded node by node without
allocating. The result is converted to the same `Compressive` / `MiniBlock` / `Constant` /
`FullZip` structs the decoder dispatches on; `page_layout.hpp` is unchanged.

The file moved into `nanolance_proto`, the one target compiled as C++23.

**Same answers as the hand-written parser.** Both parsers were compiled side by side.
- Every descriptor of files written by pylance (formats 2.1, 2.2, zstd) and by nanolance decodes
  to the same structure and the same `describe()` line.
- On 300,000 mutated descriptors under ASan / UBSan, the new parser never accepted what the old
  one refused, and never disagreed on what both accepted.
- It refuses 826 more mutants, all malformed: field number 0, two members of one oneof, a
  repeated message field, trailing garbage.

**Speed.** Parsing a descriptor in isolation runs at 0.83-0.86x of the hand-written parser (about
400 ns against 340 ns). End to end it is at parity, because the manifest and column-metadata
decodes got faster. Reading a 4,000-fragment dataset of 12,000 small pages, where per-page cost
matters most, took 120-127 ms against 122-129 ms (best of 15, four alternating runs).

### Format 2.0 encodings through nanom

`src/lance_v20_decoder.cpp` reads a 2.0 page's `/lance.encodings.ArrayEncoding` tree, and a
column's `ColumnEncoding` (the blob marker), with nanom's model. The `Enc` tree the decoder
dispatches on is unchanged.

To make that possible, nanolance's own sources now compile as C++23. That adds no requirement:
`nanolance_proto` already needed a C++23 compiler, and the public headers and API stay C++20.

**Compared side by side with the hand-written parser:**
- Every 2.0 page encoding written by pylance (plain, nullable, list, struct, dictionary, FSST,
  packed struct, blob) decodes identically.
- 300,000 mutated encodings under ASan / UBSan: the new parser is never more lenient, and it
  refuses 213 more malformed inputs.
- 2 mutants decode differently, both a message field occurring twice. The new parser merges the
  occurrences as protobuf specifies; the old one kept the last.

**Speed.** The parse alone runs at 0.78x (330 ns against 260 ns per page). End to end, the worst
case (a 4,000-fragment 2.0 dataset of 12,000 small pages) reads in 116.6-119.0 ms against
114.8-116.3 ms (best of 15, three alternating runs): about 1.5% slower, on the legacy format only.

### Page descriptors written through nanom

`src/data_file_writer.cpp` no longer assembles page descriptors from byte strings and hand-placed
keys and lengths. A small internal helper, `src/lance_descriptors.hpp`, builds each
CompressiveEncoding (Flat, Variable, OutOfLineBitpacking, InlineBitpacking, Fsst, Rle,
ByteStreamSplit, General, FixedSizeList) and each layout (MiniBlock, Constant, FullZip) on nanom's
model, and nanom computes every length prefix and varint. The column encoding is built the same
way. The old code baked in one-byte lengths in places (`0x12, 0x06, ...`), correct only while a
bit width fit in one varint byte.

**Same bytes.** Data files written by the previous and the new writer are byte-identical, across
three inputs in three modes (default, `--compress`, `--no-structural`), plus a large-value input.
The outputs cover:
- flat, nullable, bool and fixed-size-list columns;
- dictionary, dictionary + RLE, RLE and constant columns;
- inline bitpacking;
- zstd strings and BSS + zstd floats;
- FSST;
- lists of numbers, strings and long strings, empty lists, maps, structs;
- FullZip pages, with and without nulls.

Changing one bit width in one builder changes 2 of those files, so the comparison does catch a
difference. 56/56 ctest and the Python suite (1,808 tests, pylance interop included) pass.

**Speed.** Write speed is unchanged: 1.00-1.05x across those inputs.

### LZ4 and byte-stream-split from nanom

nanolance had its own copies of two kernels nanom already ships, and parquet2nanoarrow already uses
them. Both copies are gone:
- `src/lz4_block.cpp` keeps only Lance's envelope: the `[u32 size]` prefix, the decoded-size limit
  and the 255x bound checked before allocating. The block is decoded by nanom's
  `codec::lz4_block_decompress`.
- `include/nanolance/byte_stream_split.hpp` is deleted. The writer and the reader call nanom's
  `byte_stream_split_encode` and `byte_stream_split`.

**Same answers.** The old and new LZ4 decoders were compiled side by side under ASan / UBSan:
- 3,000 real blocks (pyarrow's `lz4_raw`, 0 B to 200 KB, random to highly repetitive) all decode
  identically.
- Of 300,000 mutants of them, none is accepted by one decoder and refused by the other, and none
  decodes differently.
- One hand-made test block ended in a match. The LZ4 format, the reference decoder and lz4_flex
  (which Lance writes with) all require the last sequence to be literals only, and nanom refuses
  such a block. The test now uses a valid block, and a new test checks the refusal.

Float columns written with `--compress` (byte-stream-split + zstd, widths 4 and 8, with and without
nulls) are byte-identical to `main`'s output in all three writer modes, and pylance reads them back
equal.

**Speed.** LZ4 decoding is 2.75x faster: 1.46 GB/s instead of 0.53 GB/s on one core, over the
same 3,000 blocks.

### FastLanes bit packing in nanom

`include/nanolance/fastlanes_bitpack.hpp` moved into nanom as `nanom/fastlanes.hpp` (unpack) and
`nanom/columnar_encode.hpp` (pack), with:
- a bit-by-bit reference of the layout checked against both kernels for every word type and width;
- golden bytes from this kernel, which pylance reads;
- fuzzing of the unpacker.

The writer and both readers (2.1+ and 2.0) call nanom's `_unchecked` kernels at the same places as
before: each call site already validates the width and the buffer sizes.

Data files written from integer, nullable, boolean, dictionary, list and all-zero columns are
byte-identical to `main`'s in all three writer modes, and pylance reads them back equal to the
source. Reading them is at parity: 13.6-14.4 ms best of 15, old and new interleaved on one core.

### FSST in nanom

The FSST codec moved into nanom: `nanom/fsst.hpp` reads the table and decodes,
`nanom/fsst_encode.hpp` trains, compresses and serializes. `include/nanolance/fsst.hpp` and
`src/fsst.cpp` are now an adapter that keeps nanolance's calling convention (byte vectors, error
strings with the size, symbol or code in them) over nanom's span API, so every caller, test and fuzzer
is unchanged.

**Same answers.** The old and new codecs were compiled side by side under ASan / UBSan:
- 400 corpora (random bytes, URL-like words, small alphabets, tiny values) train to byte-identical
  tables;
- their 586,694 values compress to identical bytes and round-trip;
- across 1.76 million mutated tables and code streams, accept / refuse, outputs and error messages
  are identical.

URL, text, nullable and list-of-string columns are byte-identical to `main`'s in all three writer
modes, and pylance reads them back equal.

**Speed.** Same machine, old and new interleaved:
- reading those columns: parity (45.3-48.6 ms best of 9 for both);
- writing them (training included): parity (229-241 ms);
- the codec alone: decode and train at parity, compress 8% faster.

### Validity bitmaps through nanom's bitmap kernels

Two decoder helpers built validity bitmaps one bit at a time: `append_validity_bits`, which joins
pages' and nested leaves' validity, and `append_item_validity`, for fixed_size_list element validity.
Both now use nanom's word-at-a-time `copy_bits`, `set_bits` and `count_bits`, the kernels
parquet2nanoarrow reads with. The second lost a source-bit argument that every caller passed as 0.

The rest of the decoder was surveyed against nanom's value kernels, and nothing else moved:
- **Levels to validity:** stays here. Lance's convention (level 0 = valid) differs from Parquet's.
  The portable kernel tried in nanom was 2.4-5x slower than this SSE2 compare / movemask, and no
  other nanom user needs it: the Parquet reader expands levels straight from the RLE stream.
- **UTF-8:** nanolance does not validate strings, so there was nothing to replace. Adding validation
  would be a behaviour change, not a deduplication.
- **Null spreading:** does not apply. Lance stores a value slot for every row, null or not.

**Same answers.** Old and new helpers were compiled side by side under ASan / UBSan: 200,000 random
append sequences produced identical bits, bitmap sizes and null counts. The sequences mixed absent,
all-valid and mixed sources of 0-69 bits at every offset, with junk past the end of each source.

**Faster.** A pylance-written dataset (400,000 rows: nullable int64 and double, a fixed_size_list
with null elements, nullable lists) reads in 16.8-17.8 ms instead of 23.0-24.5 ms (best of 15, old
and new interleaved on one core), 1.35x. It reads back equal to the source.


### Deliberate deviations (not defects)

- **The nullable opt-out was not needed** — simpler than planned.
- **`tests/test_framed.pcapng` (15.4 MB) stays.** Replacing a real capture with a synthetic fixture
  would change what the pcapng2lance tests cover, and those tests cannot be built without the
  nanotins submodule. Tracked size is ~18 MB, down from 22.8 MB.
- **Two Phase 4 items were closed by measuring rather than implementing** (4.3 default-init buffers,
  4.4 mmap), and one unplanned refactor was rejected the same way (making `MiniBlockChunkView` an
  actual view, 3.8% for a span type threaded through six signatures). The numbers are above.

### Every core, and real training data

Asked: use more cores without a large library and without slowing one core; prove it on a widely
used dataset. Done with a ~200-line `std::thread` pool (`src/parallel.cpp`) and no dependency:

- **Reads** cut a fragment into row ranges decoded side by side, one Arrow batch each (no
  concatenation). Cuts need pages that give up a row range: list pages via their repetition index,
  flat MiniBlock pages via their chunk counts, FullZip pages via their row index -- the same windows
  that made take() on Rust's one-page audio column 118 s -> 0.45 s per epoch and cut a full read of it
  from 1.9 s / 921 MB peak to 0.49 s / 192 MB. Morsels are sized by decode work (values x width, 32 per
  string, 32 per list layer), so columns that decode in a millisecond or two stay whole.
- **Writes** encode a file's columns side by side (unless one column is most of the data: COCO's
  images stream), and inside a page split FSST compression, page dictionaries and level serialization
  across threads -- byte-for-byte the file one thread writes.
- **A buffer pool** reuses released output buffers: glibc does not recycle memory across threads, and
  without it parallel memory-bound reads re-faulted their output every run.
- **One thread is the old path.** No pool, no split; A/B against the pre-threading build pinned to one
  core, every dataset read and wrote as fast or faster (lists faster still: `unravel` inlines
  `append_bit` and writes offsets by pointer, list<list<int32>> 10.9 -> 6.5 ms).

Result on 4 cores (docs/BENCHMARKS.md): reads 2.78x Rust Lance with both on all cores, 2.91x with
both on one; writes 1.92x / 1.48x; 26 of 27 reads and 21 of 27 writes faster on all cores. COCO 2017
val: write 1.0 s vs 3.1 s, an epoch 0.39 s vs 4.1 s, a shuffled epoch 0.38 s vs 0.89 s; Speech Commands:
1.06 / 0.09 / 0.29 s vs 1.90 / 0.38 / 0.31 s. Still behind Rust on all cores: writes of a single
large list or map column (a column's pages are cut sequentially) and of random binary blobs.
Verified with ASAN and ThreadSanitizer, and the suites run at 1, 4, and 8 threads with 1 KiB morsels.

## Scalar indexes: BTree, Bitmap, LabelList, built and used

Asked: build and use Lance's indexes, so that the same index serves pylance and nanolance with the same
results, at about pylance's speed, starting with the scalar indexes (the plan: BTree, Bitmap and
LabelList; then IVF_PQ / IVF_FLAT; then LanceDB's full-text defaults).

- **Building** (`src/scalar_index.cpp`): the column is scanned with row addresses (deleted rows left
  out), its values -- a label list's elements -- ordered nulls first, then ascending (IEEE total order
  for floats), ties by row address, and written as Lance writes them: BTree's `page_data.lance` and
  `page_lookup.lance` (4096-row pages, min / max / null count, `batch_size` metadata), Bitmap's and
  LabelList's `bitmap_page_lookup.lance` (serialized RowAddrTreeMaps; a label list's null lists in
  global buffer 1). The manifest entry is field for field Lance's. A BTree sorts in parallel on 8-byte
  key prefixes taken past the column's common prefix; a bitmap groups by hash and sorts only the
  distinct values. Standalone Lance files with schema metadata and extra global buffers came first
  (`write_lance_file`), and the reader learned files with more than one global buffer.
- **Using** (`src/index_search.cpp`): a filter's AND / OR / NOT structure over single-column
  predicates (`expr::Condition`, `expr::Predicate`), each evaluated with the filter engine's own
  semantics on an index's keys or a BTree page's bounds, gives per covered fragment the rows that may
  pass; the reader decodes only those (`read_candidate_rows`) and applies the whole filter to them.
- **What it took besides**: `array_has_any` / `array_has_all` / `array_contains` in the filter engine
  (list columns); float comparisons as Lance makes them (IEEE total order with the zeros equal:
  -NaN < -inf < ... < -0 = +0 < ... < +inf < +NaN; a literal compared with a float32 column is
  first rounded to float32); a vectorized path for column-against-constant predicates (`f > 3.5` over 5M rows
  95 -> 25 ms); late materialization (a selective filter's other columns are taken for the rows that
  pass); and take() reading only the chunks holding its rows of a flat MiniBlock page, with the
  whole-column take cache no longer thrashing once its budget is full (all columns of 1,000 scattered
  rows of 5M: 338 -> 20-45 ms).
- **Verified**: pylance lists, validates and answers from nanolance's indexes (its plans name them;
  `test_scalar_index.py`), their contents equal pylance's for the same data, and nanolance answers
  from either builder's indexes exactly as a scan does. 216 of pylance's own tests pass (190 before),
  lance-c's `test_index_lifecycle` passes. ASan clean on the index tests and on 5M-row queries.
- **Speed** (`docs/BENCHMARKS.md`, "Scalar indexes"): building 1.5-3.6x faster than pylance; indexed
  reads 1.1-3.2x faster but for a one-row lookup returning every column (2.9 ms vs 1.3 ms).
- **Not yet**: the other scalar index kinds (INVERTED is Phase 3), build options, stable row ids,
  and decimal comparisons in the filter engine (an index on a decimal column is built, but
  nanolance's filters cannot compare decimals yet). Vector indexes: next section.

## Vector indexes: IVF_FLAT and IVF_PQ, built and searched

Asked: the plan's Phase 2 -- build and search Lance's IVF_PQ / IVF_FLAT indexes, so that the same
index serves pylance and nanolance with the same results, at about pylance's speed.

- **Format first** (`docs/VECTOR_INDEX.md`): what pylance 12 writes and how it searches, step for
  step, read from the `lance` / `lance-index` 12.0.0 sources and checked by a numpy model that
  reproduces pylance's `nearest` results from the index files alone -- including Lance's adaptive
  probe count (a k-dependent margin, floor and cap for IVF_FLAT under L2 / cosine; a factor of the
  nearest centroid's distance otherwise), its 4-bit PQ's u8-quantized distance tables, and ties
  broken by row id.
- **Searching** (`src/vector_search.cpp`): `nearest=` on pylance-built or nanolance-built indexes
  returns pylance's rows, order and distances (IVF_FLAT and 8-bit IVF_PQ; 4-bit up to the order of
  equal quantized distances), with refine, prefilter / post-filter, deletions, unindexed fragments
  searched exactly, `fast_search`, `distance_range`, metric overrides and exact search without an
  index. Partitions are scored on the thread pool; PQ tables sweep a transposed codebook.
- **Building** (`src/vector_index_build.cpp`): Lance's k-means and defaults, PQ by L2 on residuals
  (as Lance's v3 builder trains it -- training or encoding dot indexes by dot instead cost them most
  of their recall), files and manifest entry as Lance writes them. pylance lists, searches and
  `optimize_indices` them.
- **What it took besides**: the manifest decoder keeps an index's details message; float filters
  order NaN by its sign and round literals compared with a float32 column to float32, as Lance does
  (pylance's `test_nan_handling`); a bind-time read of an uninitialized `ArrowSchemaView::time_unit`
  in the filter engine (UBSan); `lance.vector.vec_to_table` and `_coerce_query_vector`.
- **Verified**: `test_vector_search.py` (40 tests) against pylance; 241 of pylance's own tests pass
  (216 before); ASan + UBSan clean on the index tests.
- **Speed** (`docs/BENCHMARKS.md`, "Vector indexes"; 200,000 x 128, 256 partitions, 4 cores):
  searches 2.6-3.7x faster than pylance (IVF_PQ k=10 2.9 ms vs 7.6 ms, IVF_FLAT 1.5 ms vs 5.3 ms),
  builds 1.3x faster (IVF_PQ 12.7 s vs 16.4 s, IVF_FLAT 3.0 s vs 3.8 s), recall comparable
  (k-means is randomly seeded in both).
- **Not yet**: IVF_HNSW_*, IVF_SQ and IVF_RQ (searched exactly instead), batch and multivector
  queries, binary (Hamming) vectors, building on float16 / float64 vectors, `IndicesBuilder`, stable
  row ids. The k-means kernels are baseline x86-64 (no AVX dispatch): the next lever for builds.

## Full-text indexes: INVERTED, built and searched

Asked: the plan's Phase 3. Build and search Lance's full-text (INVERTED) index with LanceDB's
default settings, so that the same index serves pylance and nanolance with the same results, at
about pylance's speed.

- **Format first** (`docs/FTS_INDEX.md`). What Lance 12 writes and how it scores came from the
  `lance-index` / `lance-tokenizer` / `fst` / `lance-bitpacking` sources: BitPacker4x posting blocks
  with block scores, impact skip data, and a vocabulary in an `fst` map. A numpy model working from
  the index files alone reproduced pylance's scores bit for bit. Search also needs the rules around
  the index:
  - Indexed rows always use the index's statistics, even after deletions.
  - Unindexed fragments use the index's statistics plus their live (and prefiltered) rows'.
  - A string query is the best of the indexed columns.
  - A column without an index is Lance's flat search, with the bare simple tokenizer.
- **The analyzer** (`src/fts_tokenizer.cpp`) produces Lance's tokens byte for byte:
  - The Unicode alphanumeric, lower-case and folding tables and the stop words are generated from
    the Rust crates by a small Rust tool (`tools/fts_tables`).
  - The Snowball English stemmer is machine-translated from frostem's Rust (`tools/fts_stem_translate.py`).
  - It was checked on 177k words and 98k lines; 3,000 lines are kept as goldens.
- **The FST** (`src/fts_fst.cpp`) ports the `fst` crate's reader and builder: node encodings,
  suffix sharing through the same registry, and the CRC32C trailer. Built maps equal the crate's
  byte for byte (goldens from the crate). The reader survives 3,000 corrupted maps under ASan.
- **Searching** (`src/fts_search.cpp`): Match (OR / AND, boost), MultiMatch, Boost and Boolean
  queries, with filters before or after, deletions, unindexed fragments, `fast_search` and several
  partitions or segments. Scores accumulate in dense per-document arrays. With a limit, each
  match keeps only its best rows. A one-word search walks runs of 128 postings by their exact best
  weight and stops when no run can beat the rows kept: a top-10 for a word in 90% of 500k
  documents went from 16.9 ms to 0.3 ms.
- **Building** (`src/fts_index_build.cpp`): parallel tokenizing, token ids by first appearance, and
  Lance's block scores and impacts. For the same data, the files equal pylance's and LanceDB
  0.39's, byte for byte, except the partition number and document order when Lance's workers take
  fragments out of order. pylance and LanceDB search such an index with the same scores, and
  pylance's `optimize_indices` merges into it.
- **What it took besides**:
  - `lance.query` (pylance's query classes);
  - `_score` placement in projections;
  - a shared JSON reader;
  - `LanceFileExtras::bitpack_integers`, so index files bit-pack their integer columns as Lance's
    do (the docs file 1.15 MB -> 248 KB on 98k rows).
- **Verified**:
  - `test_fts.py` (24 tests) checks reads, builds, every query kind, filters, deletions, unindexed
    rows and analyzer settings against pylance.
  - About 2,000 random queries matched pylance bit for bit while developing.
  - 254 of pylance's own tests pass (241 before), 13 of them full-text.
  - ASan + UBSan are clean on the tests and the fuzzing.
- **Speed** (`docs/BENCHMARKS.md`, "Full-text (INVERTED) indexes"; 500k documents, 4 cores):
  - builds 1.7x faster (4.1 s vs 7.1 s), for an index of the same size (16.1 MB vs 16.3 MB);
  - searches 1.8-8.9x faster (best 10 for one common word 0.31 ms vs 2.74 ms; three words 1.23 ms
    vs 2.65 ms; AND 1.21 ms vs 2.35 ms).
- **Not yet**:
  - positions and phrase queries (LanceDB's default leaves positions off);
  - fuzzy matching;
  - the icu, ngram, code, jieba and lindera tokenizers, and languages other than English;
  - full-text search over list columns;
  - format-v3 (256-document) posting blocks;
  - stable row ids;
  - a multi-word top-k that skips postings (WAND / MaxScore). Multi-word searches still score
    every posting, and are already about 2x faster than pylance.

## Keeping indexes up to date, and the rest of lance-c's search API

Asked: the next index phase from the roadmap (indexes kept up to date after appends and compaction)
and the planned lance-c work (wire vector and full-text search into the C API; UPDATE_IF and
include_deleted_rows).

- **optimize_indices** (`src/index_optimize.cpp`, `include/nanolance/index_optimize.hpp`), as Lance's
  `lance/src/index/append.rs` does it:
  - segment selection by `num_indices_to_merge` (default 1; 0 adds a delta segment) and `retrain`;
  - one new segment per index, over the replaced segments' live fragments and every uncovered one,
    with their parameters: a scalar index's type, an INVERTED index's analyzer and documents (deleted
    rows kept, as Lance keeps them, so BM25 statistics -- and scores -- stay pylance's), a vector
    index's centroids and codebook;
  - Lance 12's partition rebalance for vector indexes: split partitions over 4x the target size by
    seeded k-means, keeping only centroids that win rows; join those under a quarter of it into the
    nearest of their 64 nearest survivors with room; also with nothing new to fold in.
  - The builders were split into "write a segment over these fragments" and a commit
    (`src/index_build.hpp`), so creating and optimizing share one path.
- **Compaction** folds its rewritten fragments back into the indexes that covered them (a second
  version; `CompactionOptions::reindex`, Python `compact_files(reindex=False)`).
- **lance-c** (`src/lance_c.cpp`): vector index creation and `nearest` with every setter; INVERTED
  in `create_scalar_index` (`params_json`); `full_text_search` and prepared Match contexts with
  STRICT / INDEX_ONLY coverage; `include_deleted_rows` (deleted rows with a NULL `_rowid`);
  `use_scalar_index`; merge insert `UPDATE_IF`, also in the core and in Python
  (`when_matched_update_all(condition=...)`), evaluated over `source.*` / `target.*` on zero-copy
  slices of runs of matched rows. Stubs: 39 -> 20, all index segments.
- **Verified**:
  - `test_index_optimize.py` optimizes copies with pylance and with nanolance (indexes built by
    either) and compares with pylance: segments, coverage, filter rows, full-text scores bit for bit,
    vector distances and partition counts.
  - `test_pylance_compat.py`: conditional upserts give pylance's rows.
  - `tests/test_lance_c_search.cpp` drives the C API end to end against nanolance's own search; clean
    under ASan, UBSan and LeakSanitizer.
  - lance-c's suite stays at 90 of 106 (the rest need index segments).
  - 257 of pylance's own tests pass (254 before).
- **Not yet**: index segments in lance-c (20 functions), HNSW / SQ vector indexes, phrase and fuzzy
  full-text queries, and compaction that remaps indexes in place in the same version (nanolance
  rebuilds the affected segments in the next one).

## IVF_HNSW_SQ: searched and built

Asked: IVF_HNSW_SQ, the next index gap on the roadmap -- its format, search and build.

- **Search** (`src/vector_search.cpp`): Lance's HNSW graphs (`index.idx`) and 8-bit SQ codes
  (`auxiliary.idx`), searched with Lance's greedy descent, level-0 beam (`ef`), prefilter rules
  (plain / exact under 10% passing / filtered beam) and `distance_range`, ties ranked as Rust's
  `BinaryHeap` ranks them; partitions past the first probes searched `query_parallelism` at a time as
  Lance does. Also 0-bit bit-packed definition levels (no level buffer), which pylance writes for the
  neighbour lists.
- **Build** (`src/hnsw_build.cpp`, `src/vector_index_build.cpp`): `create_index(..., "IVF_HNSW_SQ",
  m=, ef_construction=, max_level=)`; SQ bounds from a sample; each partition's graph after
  lance-index's `HnswBuilder` -- Lance's fixed-seed node levels and entry points exactly (rand 0.9
  `SmallRng`, seed 42), beam insertion, Algorithm 4 neighbour selection, reciprocal pruning, repair of
  unreachable nodes, nodes inserted in parallel. `optimize_indices` keeps such indexes up.
- **lance-c**: `LANCE_INDEX_IVF_HNSW_SQ` in `create_vector_index`; `set_ef` and
  `set_query_parallelism` now reach the search.
- **Verified**: `test_hnsw.py` against pylance -- pylance's indexes searched with pylance's rows,
  order and distances (L2, cosine; dot unfiltered), and nanolance's indexes searched by pylance with
  nanolance's answers at the recall of pylance's own; the lance-c test builds and searches one; ctest,
  the Python suite, pylance's suite (257) and lance-c's (90 of 106) all pass as before.
- **Speed** (`docs/BENCHMARKS.md`, "Vector indexes"; 200,000 x 128, 256 partitions, 4 cores):
  search 3.9-6.5x faster than pylance (k=10 2.8 ms vs 18.0 ms), build 1.2-1.4x faster, recall@10
  0.960 vs 0.962. The HNSW branch is kept out of line: inlined, it slowed IVF_PQ search by 40%.
- **Not yet**: IVF_HNSW_PQ, IVF_HNSW_FLAT, IVF_SQ, IVF_RQ; `stats.index_stats`, which most of
  pylance's own HNSW tests read.

## lance-c: index segments

Asked: continue lance-c compatibility, function before speed, without degrading function or speed.

- **Index segments** (`src/lance_c.cpp`, `src/index_segments.cpp`, `include/nanolance/index_segments.hpp`),
  the last 20 functions that only returned `LANCE_ERR_NOT_SUPPORTED`:
  - builders over chosen fragments with an assigned UUID, for BTREE / BITMAP / LABEL_LIST /
    INVERTED and IVF_FLAT / IVF_PQ / IVF_HNSW_SQ, with progress callbacks, Lance's default names
    and its name and UUID rules;
  - IVF and PQ model training exported as Arrow with lance-c's provenance metadata, and models
    injected into builds (AUTO / LOCAL_TRAIN / PRECOMPUTED); the core's builders gained a fixed
    UUID, progress stages, a train-only mode and partial models (`src/index_build.hpp`);
  - segment metadata parsing and accessors; commit as Lance's `commit_existing_index_segments`
    (validation, stale-coverage pruning, coverage-driven replacement, vector compatibility);
    listing;
  - searches restricted to segments: nearest (`NearestQuery::segments`), prepared full-text over
    some segments scored with all of them (`FtsSearchRequest::segments`), scoped scalar scans.
- **Fixed**: a prepared full-text context took only one segment of a multi-segment index when
  another index was listed first; `tools/lance_c_suite.py` now gives each test binary fresh
  fixtures, as upstream does.
- **Verified**: lance-c's suite 106 of 106 (90 before); `tests/test_lance_c_search.cpp` checks
  that per-segment searches merged equal the whole search, the commit rules, scoped scans and
  per-segment FTS scores; pylance lists, searches and uses the indexes committed this way; ctest,
  the Python suite and pylance's suite (257) unchanged. Speed: index builds and vector,
  full-text and filtered searches as before (FTS search instruction count within 0.2%).
- **Not yet**: BTREE / BITMAP / LABEL_LIST parameters, search with `set_fragment_ids`, IVF_SQ /
  IVF_HNSW_PQ / IVF_HNSW_FLAT, Hamming, multivector, phrase and fuzzy full-text, Substrait, object
  stores. Full-text ties come back in another order than pylance's.

## Phrase queries: positions in INVERTED indexes

Asked: continue toward feature completeness and performance parity; new features function first,
without degrading current function or speed.

- **Read** (`src/fts_search.cpp`): Lance 12's positions (`shared_stream_v2`, `packed_delta_v1` and
  `varint_doc_delta_v2`), decoded per token on first use and cached. Other layouts are refused for
  phrases only.
- **Search**: `PhraseQuery` with slop, and a string in double quotes, as Lance does them (exact and
  slop checks from `wand.rs`, the flat check for unindexed rows from `flat_search.rs`, query
  positions counted from the first term kept); pylance's errors for a missing column or an index
  without positions. lance-c's `lance_dataset_prepare_fts_phrase_query` works.
- **Build** (`src/fts_index_build.cpp`): `with_position=True` writes positions as Lance 12 does,
  with the same contents as pylance's build; pylance's phrase queries and `optimize_indices` use it.
- **Verified**: 1,050 random phrases (pylance-built and nanolance-built indexes, unindexed and
  deleted rows, limits) give pylance's rows and scores; `test_fts_phrase.py`; the lance-c phrase
  test under ASan/UBSan; pylance's suite 259 (257 before), lance-c's 106 of 106, ctest, the Python
  suite. Speed: FTS builds and match searches as before (search instructions 0.1% fewer; the
  phrase check sits outside the plain match's loops).
- **Not yet**: fuzzy matching; full-text ties under a limit can keep other rows than Lance's
  block-max WAND keeps (`docs/ROADMAP.md`).

## Writes with part of the schema

Asked: work through the Tier 1 list of `docs/ROADMAP.md` (what pylance users hit most), function
first, without degrading function or speed.

- **Append some of the columns** (`src/writer.cpp` `writer_project_append`): the new files hold the
  given columns alone, with the dataset's field ids, as pylance writes them; leaving out a
  non-nullable column, or giving one the dataset lacks, is pylance's error, word for word.
- **Reading columns a fragment lacks** (`src/lance_table_reader.cpp`): a struct or list column
  whose fragment has no data reads as a null value, as in Lance (it read as a struct of nulls,
  and a missing list column could not be read at all).
- **`merge_insert` with part of the columns**: matched rows keep their values in the other
  columns, inserted rows get nulls there; `when_matched_delete`, `when_matched_fail`,
  `write_mode`, the schema's unenforced primary key as default `on`, and pylance's error for a
  merge that changes nothing.
- **Verified**: `tests/test_partial_schema.py` (every case run on nanolance and pylance, read back
  by both: structs, nested structs, lists, lists of structs, fixed-size lists, deletions, multi-key
  merges); pylance's suite 271 (259 before); lance-c's 106 of 106; ctest; the Python suite.
  Full-schema appends and merges take the same path as before.
- **Found**: pylance retries a commit that lost a race to a compatible one; nanolance refuses it,
  so `test_compact_with_write` fails about 1 run in 16, before this change too (now in the
  roadmap's Tier 1). nanolance casts an append's column types where pylance refuses them.

## Concurrent writers

- **Retries, as Lance's** (`src/dataset.cpp`, `src/manifest_writer.cpp`, `src/dataset_ops.cpp`,
  `src/index_optimize.cpp`): a commit that loses its version is no longer refused when it still
  applies -- an append or overwrite is built again on the newer version; a change made from an
  older version is rebased when the other writers only added fragments (its own new fragments
  renumbered after theirs, with the index coverage that names them); a delete, update,
  merge_insert, compaction or index optimization whose fragments another writer rewrote runs
  again on the latest version. Up to 20 tries with a short jittered pause, Lance's default.
- **Fixed: writers in different processes overwrote each other's data files.** Files were named
  from a counter over `data/`; two processes took the same name, and the later file replaced the
  earlier one, committed or not. Names are now 128 random bits, drawn from the entropy source on
  every call (a seeded generator is copied into forked processes, which then all draw the same
  names); deletion file ids take random bits too. This also drops a directory listing per file.
- **Verified**: `tests/test_concurrent_writers.py` (processes and threads: appends racing a
  compaction, deletes, an update and a merge; every row once, every change landed, pylance reads
  what nanolance reads, the index still answers); pylance's `test_compact_with_write` 16 of 16 in
  parallel (15 of 16 before); pylance's suite 271, lance-c's 106 of 106, ctest, the Python suite.

## System columns

- `_rowoffset`, `_row_created_at_version` and `_row_last_updated_at_version` in takes and scans,
  where the projection names them (the version columns read 1 without stable row ids, as Lance's
  do; refused with them); `_rowoffset` of a take is non-nullable, as pylance's.
- Filters on `_rowid` / `_rowaddr`: the scan returns every row with its address, the filter is
  evaluated by the core's own expression engine (new `_filter_mask` binding), then offset and
  limit; `count_rows` too.
- `_distance` (and `_score`) after the named columns, or left out with
  `disable_scoring_autoprojection`; a dataset opened with `default_scan_options` shows `_rowid` /
  `_rowaddr` in its schema (internal paths use the data schema); `batch_size_bytes` and
  `LANCE_DEFAULT_BATCH_SIZE`; pylance's limit/offset error wording.
- **Verified**: `tests/test_system_columns.py` against pylance (takes, projections, filters with
  limit/offset, distance placement); pylance's suite 283 (271 before); ctest, lance-c 106 of 106,
  the Python suite. Plain scans take the same path as before.

## Nested field paths

- A projection of `s.x`, `MetaData.deep.z` or `` `meta-data`.`user-id` `` reads the top-level
  column and returns the field under the name as written (pylance's names); `create_scalar_index`
  resolves a path as Lance does (exact name, else the one case-insensitive match; back-quoted
  segments) and stores the schema's own names (`MetaData.userId_idx`).
- **Verified**: `tests/test_nested_paths.py` against pylance; pylance's suite 286 (283 before).
- **Not yet**: INVERTED indexes on nested fields; SQL expressions in projections; `order_by`.

## Version housekeeping

- **Tags** (`src/dataset_refs.cpp`): `_refs/tags/<name>.json` in Lance's format (camelCase,
  RFC 3339 times, manifest size, metadata), Lance's name rules and messages; created without
  replacing (hard link, as `put_if_absent`). pylance reads nanolance's tags and the reverse;
  `checkout_version("tag")`, `lance.dataset(uri, "tag")`.
- **Cleanup**, Lance 12's rules: the read version, every newer and every tagged one kept (or an
  error naming the tags); a removed version's files removed, files no version names only when 7
  days old (or `delete_unverified`); only files no newer than the earliest kept version are listed,
  unless a removed version is newer than a kept one; index directories by UUID; Blob v2 sidecars
  with their data file; `retain_versions`, `versions`, `older_than`, `delete_rate_limit`; explain
  with candidate files. Old manifests are removed before the files, so a cleanup cut short never
  leaves a version whose files are gone.
- **Auto cleanup**: every commit (`publish_manifest`) runs what the new version's
  `lance.auto_cleanup.*` config asks for, as of the version before, errors ignored, as Lance's
  hook. `auto_cleanup_options` goes into the first version's config. lance-c's create defaults
  (every 20 versions, older than 14 days), already recorded, now take effect, as in Lance.
- **Drop**: refused unless the path holds a manifest that reads (or a namespace marker).
- **Verified**: `tests/test_version_housekeeping.py`: nanolance's and pylance's explain on copies of
  the same dataset (nanolance- and pylance-written; overwrites, deletes, an update, an index,
  compaction, a stray file; five policies) pick the same files, kinds and stats; after the
  cleanup pylance and nanolance read every version left as before. Tags both ways; auto cleanup
  matches pylance's version for version. pylance's suite 305 (286 before); ctest, lance-c 106/106,
  the Python suite.

## order_by

- `order_by` in `to_table` / `to_batches` / `scanner` and on fragments: column names or
  `ColumnOrdering(column, ascending, nulls_first)`, names exact (as Lance). The filtered rows are
  read without offset and limit (plus sort columns not projected), sorted by stable passes from
  the last key to the first (each key its own direction and null placement), then sliced.
- Floats sort in IEEE total order (`-NaN < -inf < ... < -0.0 < 0.0 < ... < inf < NaN`, nulls
  apart), as Lance's sort does; Arrow's own sort ranks NaN with the nulls and -0.0 equal to 0.0, so
  float keys are sorted by their total-order integer image.
- **Verified**: `tests/test_order_by.py` (12 random specs over int / float with NaN and ±0 /
  string / bool / timestamp keys with nulls, filters, offset and limit, projections without the
  sort columns; the forms; fragments) equal to pylance's results; pylance's suite 311 (305 before).

## Transaction files

- **The bug**: pylance, committing from a read version older than a version nanolance committed,
  failed with Lance's internal error "Dataset version N does not have a transaction file" -- any
  pylance writer sharing a dataset with a nanolance writer could hit it.
- **The fix** (`src/transaction_file.cpp`): every commit (`publish_manifest`) writes
  `_transactions/{read_version}-{uuid}.txn`, a lance.table.Transaction, before the manifest that
  names it (field 12), and removes it when the commit does not land. The operation is read off the
  change from the version before (fragments added / deletion files changed / fragments replaced,
  schema, indices, config), with the commit's own word where the change cannot say it: append,
  overwrite, restore, compaction (Rewrite), update and merge_insert (Update, as Lance records them
  even when every row was an insert).
- **Speed**: the append and overwrite paths describe themselves, so no manifest is re-read; the
  remaining cost is the file itself, about 0.2 ms a commit on a 1,000-row append (Lance writes the
  same file).
- **Verified**: `tests/test_transactions.py`: pylance decodes every kind of nanolance commit as the
  operation it records itself; pylance commits (insert, delete) from a stale version over each kind
  of nanolance commit land with the right rows, or conflict exactly as two pylance writers would
  (over an overwrite; an append over a schema change); one transaction per version after racing
  writers. pylance's suite 312 (`test_cleanup_with_retain_versions` now passes); ctest, lance-c
  106/106, the Python suite.

## Compaction parity

- **Planner, as Lance's** (`compact_once`): a fragment is a candidate when more of it is deleted
  than the threshold (rewritten even alone) or it has fewer *physical* rows than the target (only
  with neighbours); runs of candidates end at an excluded fragment, a non-candidate, or a change of
  index coverage (an indexed fragment never joins an unindexed one); a lone neighbours-only
  fragment is dropped; runs split where a piece has reached the target and the rest can too.
  Before: live rows, no index boundary, and bins cut before the target, so nanolance compacted
  differently from pylance on the same dataset.
- **Options**: `max_source_fragments` / `max_source_rows` / `max_source_bytes` (whole tasks in
  order; a budget of 0 refused, as Lance), `excluded_fragment_ids`, `max_bytes_per_file` with
  `batch_size` (rows fed that many at a time, a data file cut whenever the buffered bytes reach
  the limit), a threshold of 1.0 turning materialization off.
- **Commits, as Lance's**: a ReserveFragments version first, then the Rewrite made on top of it
  with the reserved ids (a fragment changed in between is a conflict, and the compaction is planned
  again); the reserve rebases above other writers' ids.
- **Verified**: `tests/test_compaction_plan.py`: 16 random datasets (sizes, deletions, an index,
  exclusions, budgets) compacted by nanolance and pylance leave the same fragments (new ids and row
  counts as sets: Lance numbers them in task completion order), the same rows, versions and
  transaction kinds. pylance's suite 316 (312 before). Compaction speed unchanged (200 fragments).
- **Not yet**: index remapping inside the Rewrite (see the roadmap).

## JSON columns and filter functions

- **The gap**: JSON columns did not interoperate at all. pylance stores `pa.json_()` as JSONB
  (Databend's binary JSON, the `jsonb` crate) under logical type "json"; nanolance refused those
  datasets and wrote its own as text, which pylance refused.
- **JSONB** (`src/jsonb.cpp`): the crate's lenient parser (single quotes, unquoted keys, NaN /
  Infinity, hex, `+1`, `.5`, `123.` as an integer; duplicate keys and trailing commas refused),
  its encoder (sorted keys, u64 / i64 / f64 number types, smallest integer width), its printer
  (compact; floats positional for exponents -5..15 with ".0" when integral, else `1e+20`; NaN and
  infinities as null; `\b \f \n \r \t \u00XX` escapes), and JSONPath selection. Checked against
  pylance on 3,000 random documents: the same bytes written, the same text read.
- **Columns**: an `arrow.json` column is encoded on the way in (every write path goes through
  `_coerce_reader`) and stored as logical type "json" (a large-binary column marked `lance.json`);
  reads return `arrow.json` text, filters and compaction see the JSONB.
- **Functions**: `json_get` (key, or index of an array), `json_get_string / int / float / bool`
  (the crate's conversions; an impossible one fails the query, as in Lance), `json_extract`,
  `json_exists`, `json_array_contains`, `json_array_length`; `::` casts, `arrow_cast` to scalar
  types, `regexp_match` / `regexp_like`, `current_date()` / `now()`, `ARRAY[...]`, `0x..` binary
  literals, pyarrow's `add_checked` & co. and its timestamp / date literals; filter errors worded
  as pylance's ("Invalid user input: ...", "No field named ..."). Fixed on the way: an error about a
  non-ASCII character cut it in half, and Python could not decode the message.
- **Speed**: the new functions are dispatched by a family set at parse time; filters using the
  existing functions run as before (2M rows, `lower(s) = ...` and `starts_with(...)`).
- **Verified**: `tests/test_json_columns.py` (bytes and text both ways, appends across libraries,
  compaction, 15 JSON filters and 8 other functions equal to pylance's results); pylance's suite 336
  (316 before); ctest, lance-c 106/106, the Python suite.

## Small dataset APIs

- **`stats`**: `dataset_stats` (deleted rows, fragments, small files), `data_stats` (bytes on disk
  per field: the page buffers of each field's columns, summed over the data files, as Lance sums
  them -- `dataset_data_stats`), `index_stats` / `index_statistics` (coverage from the manifest,
  per-segment details from the index files: BTree min / max / pages, bitmap counts, the inverted
  index's params, tokens and documents, and for IVF_FLAT / IVF_PQ / IVF_HNSW_SQ the model --
  partitions, centroids, k-means loss, the sub-index metadata -- read by
  `read_vector_index_model`). Equal to pylance's on pylance's indexes and on nanolance's.
- **`lance_schema`** and `lance.schema.LanceSchema` / `LanceField` (ids, parents, metadata, the
  primary and clustering keys, lookups by path, `from_pyarrow`, pickling, pylance's repr), and
  **`update_field_metadata`** (set, remove, replace; recorded as an UpdateConfig of field metadata).
- **Transactions**: `read_transaction` / `get_transactions` decode `_transactions/*.txn` into
  `LanceOperation.*` (Append, Delete, Overwrite, CreateIndex, Rewrite, Merge, Restore, Update,
  Project, UpdateConfig, DataReplacement; ReserveFragments shows as a bare `BaseOperation`, as in
  pylance) and `Transaction`; writes record `transaction_properties` and `commit_message`.
  nanolance and pylance read every transaction of either library's history the same way.
- **`merge`** (a hash join on a key column, the right key not added, unmatched rows null) and
  **`validate`** (Lance's manifest and file checks).
- **Fixed on the way**: a restore's transaction was recorded as an Overwrite; struct fields were
  written with a "plain" encoding (pylance writes none, and "dictionary" for dictionary fields);
  Merge and Project transactions now carry `preserves_nullability` as Lance computes it; the
  standalone file reader keeps the file schema's metadata.
- **Verified**: `tests/test_small_apis.py`; pylance's suite 383 (336 before, 23 of them from
  `test_vector_index.py`, which now loads); ctest 60/60, lance-c 106/106, the Python suite.

## Writer tail

- **Arrow dictionary columns**: written as Lance writes them -- logical type
  `dict:<value>:<index>:<ordered>`, dictionary pages of the indices at the index type's width with
  the dictionary the batches carried (unused entries and order kept) -- and read back as dictionary
  columns, with the stored dictionary, from either library's files. Inside nanolance a dictionary
  field is its VALUES (`LanceField::dictionary_index_format` beside the value type), so decoding,
  filters, takes and the encoders work unchanged; only the Arrow form at the edges differs. The
  decoder reads 8/16/32/64-bit indices flat, bit-packed or in runs (pylance writes all of them).
- **Empty structs**: a column of their own, one constant page (byte for byte what pylance writes);
  a nullable one, or one in a list, is refused with Lance's error.
- **Null elements in fixed-size lists**: `FixedSizeList{has_validity}` mini-block chunks of two
  buffers (element bits, then values), as pylance writes them; both libraries read either's.
  Under a list or a struct they stay refused: accepting them at the top level would otherwise have
  let a nested one through as 0.0.
- **Also**: Lance's error for a zero-dimension fixed-size list, pydantic enums (a string enum is a
  dictionary column), `truncate_table`.
- **Speed**: unchanged (2M rows: write 0.128 s / 0.130 before, scan 0.021 s, filter 0.039 s).
- **Verified**: `tests/test_writer_tail.py` (every shape written by both libraries and read by
  both); pylance's suite 393 (383 before); ctest 60/60, lance-c 106/106, the Python suite.


## Distributed index builds

- **Segments**: `create_index_uncommitted` builds one segment over chosen fragments without
  committing it (BTree, Bitmap, LabelList, Inverted, IVF_FLAT, IVF_PQ, IVF_HNSW_SQ; vector segments
  with a shared `ivf_centroids` / `pq_codebook` or a model of their own) and returns pylance's
  `Index` -- uuid, fields, version, `fragment_ids` as a `lance.bitmap.Bitmap`, the files written,
  the index details. `merge_existing_index_segments` folds a group into one new segment and
  `commit_existing_index_segments` publishes segments of either library as one logical index. The
  core gained `dataset_build_index_segment`, `dataset_merge_index_segments`,
  `dataset_train_vector_model`, `infer_index_segment_details` and the IndexMetadata codec
  (`include/nanolance/index_segments.hpp`) over the segment builders lance-c already used.
- **Merging** is a build over the union of the segments' fragments at the oldest of their versions,
  with the first segment's settings and, for vector segments, its model -- after Lance's checks
  (one keyed field, disjoint coverage, one type; metric, partition count, centroids and codebook
  equal within 1e-5, Lance's "... mismatch across shards" errors; SQ bounds from the first segment,
  as Lance's merger takes them). The result is the index Lance's merge produces; the files are not
  a byte-level merge of the segments'.
- **The legacy INVERTED flow**: `create_scalar_index(fragment_ids=, index_uuid=)` stages parts under
  `_indices/<uuid>/`, `merge_index_metadata` builds the index there (reporting Lance's three
  progress stages as `lance.progress.IndexProgress`), and `LanceDataset.commit(...,
  LanceOperation.CreateIndex(...))` commits it; an `Index` without details gets them from its
  files, as Lance infers them.
- **`lance.indices.IndicesBuilder`**, pylance's module adapted, with its native calls in
  `lance.lance.indices` (where pylance's tests patch them): IVF and PQ training on float16 / 32 / 64
  vectors and chosen fragments, `prepare_global_ivf_pq`, and the transform / shuffle / load steps.
  `IvfModel` / `PqModel` files are written and read by either library. Also `centroids()`,
  `get_ivf_model()`, and `lance.bitmap.Bitmap` (pylance's `test_bitmap.py`: 55 of 66).
- **Fixed on the way**: training a model (lance-c's `lance_index_train_*_model` too) on a dataset
  that already had an index of the default name failed with "already exists"; nanolance's data files
  did not carry the schema metadata Lance writes into each (so `LanceFileWriter` lost it, and with it
  `IvfModel.save`'s distance type).
- **Verified**: `tests/test_distributed_index.py` (17 tests: nanolance's segments answer in pylance
  as pylance's one-go index -- rows, FTS scores, and with a shared model the same vector rows and
  distances as pylance's own segments; cross-built segments both ways; models both ways); pylance's
  suite 603 (483 before: `test_indices.py` 23, `test_bitmap.py` 55, 32 more in
  `test_scalar_index.py`, 8 more in `test_vector_index.py`); ctest 60/60, lance-c 106/106, the
  Python suite (2156), the interop suite.
