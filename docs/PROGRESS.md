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

Test suite: **50 ctest** (was 42) and **399 pytest** (was 22), all passing -- and nothing skipped: the one
ctest that used to report a green SKIP for a real interop failure now passes for real.

Fuzzers: `nanolance_fuzz_decode`, `nanolance_fuzz_page_layout`, `nanolance_fuzz_fsst` and
`nanolance_fuzz_lz4`, all clean; the longest campaign run here was 95,896,936 executions. Between
them they have found three real bugs on this branch.

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

## Deviations from the plan, and open items

- **The nullable opt-out was not needed** (see above) — simpler than planned.
- **`large_utf8` was refused rather than fixed.** The plan said "prefer fixing; reject as the
  stopgap". Fixing it means touching five variable-width page paths' hand-assembled protobuf, for a
  type central to neither target audience. The exact fix is recorded where the rejection is.
- **`tests/test_framed.pcapng` (15.4 MB) stays.** Replacing a real capture with a synthetic fixture
  would change what the pcapng2lance tests cover, and those tests cannot be built without the
  nanotins submodule. Tracked size is ~18 MB, down from 22.8 MB.
- **GitHub Actions is not running on this repository right now.** Every workflow run on this branch
  — including long-standing ones untouched by this work (`linux-bench` #229, `Memory safety` #77,
  `Python bindings` #125) — completes as a failure within 3–7 seconds and produces no downloadable
  logs at all (the log endpoint 404s). That is what an account- or repository-level Actions problem
  looks like (spending limit, Actions disabled, no runner minutes), not a workflow defect: it
  predates the workflows added here and hits jobs whose files have not changed in weeks. Nothing in
  this document that says "verified" rests on CI — the ctest, pytest and fuzzer numbers were all
  produced locally — but the **macOS job and the wheel builds specifically cannot be confirmed until
  Actions runs again**, because there is no local macOS or manylinux to run them on. The Linux wheel
  itself WAS built and installed into a clean virtualenv locally; what is unverified is cibuildwheel
  driving that across CPython 3.9–3.13 and macOS.

- **Not yet verified on this branch:** macOS and Windows (CI is Linux-only), and the ASan/UBSan
  workflow, which runs in CI rather than here. The libFuzzer workflow's targets were built and run
  locally (see above).
