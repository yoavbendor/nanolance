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
| **Phase 1.3 — read stock-Lance files** | **steps 1 and 2 of 3 done** (parse, oracle, dispatch) |
| Fuzz coverage for the descriptor parser | done — found one real bug in 25 executions |
| 1.1 Real nullability | **done for fixed-width columns**, read and write; strings and null structs still refused |
| 1.2 timestamp / date / time / decimal | **done** — plus a pre-existing width-declaration bug it exposed |
| Phase 2 — wheels, CMake install | not started |
| Phase 3 — streaming read, projection in Python | not started |
| Phase 4 — read-path optimization | not started (deliberately last) |

Test suite: **46 ctest** (was 42) and **146 pytest** (was 22), all passing.

Fuzzers: `nanolance_fuzz_decode` and `nanolance_fuzz_page_layout`, both clean; the longest
campaign run here was 95,896,936 executions.

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

## Phase 1.3: the reader generalization, step 1 of 3

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

---

## Deviations from the plan, and open items

- **The nullable opt-out was not needed** (see above) — simpler than planned.
- **`large_utf8` was refused rather than fixed.** The plan said "prefer fixing; reject as the
  stopgap". Fixing it means touching five variable-width page paths' hand-assembled protobuf, for a
  type central to neither target audience. The exact fix is recorded where the rejection is.
- **`tests/test_framed.pcapng` (15.4 MB) stays.** Replacing a real capture with a synthetic fixture
  would change what the pcapng2lance tests cover, and those tests cannot be built without the
  nanotins submodule. Tracked size is ~18 MB, down from 22.8 MB.
- **Not yet verified on this branch:** macOS and Windows (CI is Linux-only), and the ASan/UBSan
  workflow, which runs in CI rather than here. The libFuzzer workflow's targets were built and run
  locally (see above).
