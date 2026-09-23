# nanolance — review & improvement plan

_Reviewed at `b914735`. Every claim below was reproduced on this tree (Ubuntu 24.04, gcc 13.3,
Release), not read off the docs. Commands to re-run each check are inline._

## 1. Verdict

nanolance is a genuinely good **writer**. The encoder work is done and it shows: on the CI bench it
beats rust-lance on `float_smooth` (5.95 ms vs 6.50) and `bool_flags` (0.78 vs 1.26), reaches parity
on `pcap_ref`, and lands at Lance file sizes — which are smaller than Parquet's. The external-blob
data model is a real differentiator with no equivalent in either ecosystem. The read path is
hardened to a standard most C++ projects do not reach, and it is *proven* hardened (ASan/UBSan CI,
libFuzzer, a parity benchmark showing the checks are free).

The problem is that almost none of that is reachable by either audience we want.

For a **rust-lance user**, the deciding question is "can it read my data?" Today: no. A plain
5000-row `int64` column written by `lance` 12.0.0 fails to decode. So does a string column, a
nullable column, a timestamp, a list, a dictionary, a struct. The reader is explicitly scoped
"writer parity only" (`docs/lance_table_reader_plan.md`). That is a defensible engineering choice
and a fatal adoption choice: it means nanolance is not a Lance reader, it is a reader of nanolance's
own output. There is nothing to switch *to*.

For a **"simple parquet" Python user**, the deciding question is "does my dataframe survive the
round trip?" Today: no, and it fails *silently*. `nanolance.write_table` turns nulls into `0` and
`""` with no error and no warning. Timestamps — in practically every parquet file in the world —
are rejected outright. And they cannot `pip install nanolance` at all; there are no wheels.

So the headline is not "make it faster." It is: **the writer is ahead of everything around it.**
Correctness gaps, type coverage, and distribution are what is actually blocking adoption, and the
read-throughput gap is fourth on that list, not first.

One structural note that shapes the whole plan: several items below are not "add a feature," they
are "the docs claim this already works." Nullable columns, nested structs, and `large_utf8` are all
advertised in `README.md` and all broken. Fixing the claims is as urgent as fixing the code, because
a user who hits one of these silently stops trusting the rest of the numbers — including the
benchmark table, which is honest and hard-won.

---

## 2. What was measured

### 2.1 Nulls are silently corrupted (P0)

```python
import pyarrow as pa, nanolance
t = pa.table({"a": pa.array([10, None, 30], type=pa.int64()),
              "s": pa.array(["x", None, "zz"])})
nanolance.write_table(t, "py_nulls.lance")
pa.table(nanolance.read_table("py_nulls.lance")).to_pydict()
```

```
INPUT : {'a': [10, None, 30], 's': ['x', None, 'zz']}
OUTPUT: {'a': [10,   0,  30], 's': ['x',   '', 'zz']}
```

No exception. No warning. The data is just wrong.

The mechanism: `schema_mapper.cpp:225` rejects any nullable field unless `ignore_nullability` is
set, and when it *is* set, `map_field` writes `out.nullable = false` (line 244) and the validity
bitmap is never read. `arrowipc2lance --help` is honest about it ("copy null slot bytes as-is"); the
Python binding is not — `WriteOptions.ignore_nullability` defaults to **`True`**
(`bindings/python/nanolance/__init__.py:22`).

That default is there because pyarrow marks every field nullable by default, so without it the
common case is a hard error:

```
$ arrowipc2lance -c -o big.lance < big.arrow
nullable fields are not supported without --ignore-nullability: pos
```

Which leaves a user with two doors: a confusing error, or silent corruption. The project's own test
knows — `bindings/python/tests/test_lance_parity.py::test_lance_nullable_roundtrip` writes a table
with nulls and then asserts only on the `ctrl` column (which has none) plus the row count. It is
shaped precisely around the bug.

This also quietly undermines the interop claim. Stock Lance reads these files fine; it just reads
wrong values out of them. "Verified against lance" is true at the format level and false at the data
level, which is the worse of the two failure modes.

### 2.2 nanolance cannot read stock-Lance data (P0 for the rust-lance audience)

5000 rows per case, written by `pylance` 12.0.0, read with `nlance2table`:

| stock-lance dataset | nanolance read |
|---|---|
| `int64` plain | **FAIL** — `fixed-width page byte count mismatch` |
| `utf8` | **FAIL** — `variable-width chunk terminal offset out of range` |
| `int64` with nulls | **FAIL** — `unexpected miniblock payload prefix` |
| `timestamp[ms]` | **FAIL** — `unsupported on-disk logical type ... timestamp:ms:-` |
| `list<int32>` | **FAIL** — `unsupported on-disk logical type: list` |
| `dictionary<string>` | **FAIL** — `unsupported on-disk logical type: dict:string:int32:false` |
| `struct` | **FAIL** — `struct field has no children in mapping` |
| `float64` | OK |
| `bool` | OK |

Note the first row. It is not an exotic encoding — it is `pa.array(range(5000))`. A 3-row version of
the same table *does* read, which is worse than a clean failure: it means the smoke test a curious
user writes first will pass, and their real data will not.

Also: the docs pin interop claims to `lance` 7.0.0. Current pylance is 12.0.0. Whatever we assert
about compatibility needs a version floor and a CI job that actually re-checks it.

### 2.3 Arrow type coverage is narrower than advertised

Round-trip via `arrowipc2lance` → `nlance2table`:

| Arrow type | write | read back |
|---|---|---|
| `int8..64`, `uint8..64`, `float`, `double`, `bool` | ok | ok |
| `utf8`, `binary`, `fixed_size_binary` | ok | ok |
| `large_utf8` / `large_binary` | ok | **FAIL** — `variable-width chunk terminal offset out of range` |
| `dictionary<utf8>` | ok | **WRONG** — reads back the index (`0`), not the value (`"a"`) |
| `struct` | ok | **FAIL** — `nested struct children are not supported in this reader build` |
| `timestamp` / `date32` / `time64` | **reject** — `unsupported Arrow C format: tsu:` / `tdD` / `ttu` | — |
| `decimal128` | **reject** — `d:10,2` | — |
| `list` | **reject** — `+l` | — |
| `null` | **reject** — `fixed-width array is missing values buffer` | — |

Three of these — `large_utf8`, `dictionary`, `struct` — are **write/read asymmetries**: the writer
accepts them and produces a file that the library's own reader cannot load. That is strictly worse
than rejecting them, and `README.md` advertises nested structs as supported.

`timestamp` is the one that matters most for the parquet audience. There is no such thing as a
representative parquet file without a timestamp column, and `pa.Table.from_pandas` produces one from
any `DatetimeIndex`. The good news is that it is cheap: `timestamp` is `int64` on the wire, so this
is a `schema_mapper` mapping plus a logical-type string, not an encoder change.

### 2.4 Read path: where the time actually goes

Local callgrind, 1M rows × 4 columns (constant uri, constant size, bitpacked position, bitpacked
int64) — the library's own flagship shape:

```
132,601,959 (100.0%)  PROGRAM TOTALS
 43,155,475 ( 32.6%)  __memset_avx2_unaligned_erms
 32,900,253 ( 24.8%)  __memcpy_avx_unaligned_erms
 16,000,194 ( 12.1%)  append_repeated_value(...)
  9,430,981 (  7.1%)  fastlanes::unpack_1024_w<uint64_t, 20u>
  4,984,624 (  3.8%)  decode_lance_physical_column(...)
```

This matches the CI profile on `pcap_ref` (39.8% memset / 27.0% memcpy). **Roughly 70% of read
instructions are zeroing and copying buffers, not decoding.** Three separate causes:

1. **Zero-then-overwrite.** `std::vector<uint8_t>::resize()` value-initializes. Every
   `read_lance_data_file_bytes` page read, every `append_repeated_value`, every
   `out.fixed.resize(base + page.length)` memsets bytes that the very next line memcpys over.
2. **A whole redundant materialization.** Decode writes into `ColumnValues`, then
   `fill_fixed_child` / `fill_variable_child` copy `ColumnValues` into the `ArrowBuffer`. For this
   dataset that is ~59 MB written, then ~59 MB copied again. The decoder could write straight into
   the destination `ArrowBuffer`.
3. **Constant columns are expanded N times.** `size` (all `1500`) and `uri` (one 27-byte string)
   are stored in ~0 bytes on disk and then blown up to 8 MB and 35 MB in memory — twice, per (2).
   That is the exact workload the external-blob design optimizes for on disk, undone on read.

**I ran the experiment for cause (1).** Swapping the `ColumnValues` byte buffers and the page-read
buffers to a default-initializing allocator (so `resize` skips the memset) — output byte-identical,
verified with `cmp` on the data files:

```
baseline      best_ms: 80.31 / 78.89 / 78.58
no-zero-fill  best_ms: 75.15 / 74.32 / 73.48
```

~6.5% wall clock. Real, but far less than 32.6% of instructions suggests — memset is
bandwidth-bound and cheap per instruction. **The lesson is that the instruction profile overstates
this fix**, and the bigger win is in causes (2) and (3), which delete the work rather than making it
cheaper. I am flagging this explicitly because it is easy to read the callgrind output and promise a
3× that will not materialize.

Current gap to close, from CI: `pcap_ref` read 11.01 ms vs rust-lance 6.24 ms.

### 2.5 The documented quick start does not work

`README.md` and `docs/getting-started.md` both say building the library needs no submodule, and give:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

On a non-recursive clone this fails:

```
CMake Error at examples/pcapng2lance/CMakeLists.txt:14 (message):
  examples/pcapng2lance/extern/nanotins is empty — run: git submodule update --init --recursive
```

`NANOLANCE_BUILD_EXAMPLES` defaults `ON` for standalone builds (`CMakeLists.txt:337`) and the
example hard-`FATAL_ERROR`s. This is the first command a new user runs, and the fix is four lines.

### 2.6 Distribution

- **No wheels.** No `cibuildwheel`, no PyPI publish job. Installing means CMake + a C++20
  toolchain + network for FetchContent. A pandas user will not do this. This is the single biggest
  lever for the "simple parquet user" audience and it is pure packaging work.
- **No `install()` rules at all** in `CMakeLists.txt`. No exported targets, no
  `nanolanceConfig.cmake`. C++ consumers cannot `find_package(nanolance)` — only
  `add_subdirectory`/FetchContent.
- **CI is Linux-only.** No macOS, no Windows, no ARM.
- **Version skew.** `bindings/python/pyproject.toml` says `0.1.0`; the library says `0.2.0`.

### 2.7 Python API gaps

- `read_table` calls `nano_lance_table_read_dataset`, which materializes **every** batch before
  handing back a handle (`bindings/python/src/nanolance_bindings.cpp:253`). The docstring's promise
  that a reader "can consume it chunk by chunk" is true of the handle and false of the memory
  profile.

  **Measured peak is 2.01× the dataset — but not for the reason stated here originally.** On a
  61 MiB, 4M-row table: one fragment → **2.01×**; the same data across 16 fragments → **1.10×**. So
  the 2× is *not* batches accumulating, and `ExportedTable::from_read_result` is not a copy (it is
  `ArrowArrayMove`). It is **within one fragment's decode**: the decoder fills `ColumnValues`
  buffers and then copies them into the ArrowArray, so both exist at once. Split across 16 fragments
  the transient is 1/16 of the data and the peak collapses.

  This changes what fixes it. Streaming (3.1) removes the *accumulation*, which only bites on
  many-fragment datasets; the 2× on an ordinary single-fragment dataset is removed by **4.1,
  decoding straight into `ArrowBuffer`**.
- `lance_table_read_dataset_projected` exists in C++ and is **not exposed to Python**. Column
  projection is the first thing a parquet user reaches for.
- No row-range / slice / `take`, no filter, no `count_rows`, no schema-only peek.

### 2.8 Repo hygiene

22.8 MB tracked, of which ~21 MB is binaries: `tests/test_framed.pcapng` (15.4 MB),
`st_nl.srctrldb` (4.9 MB, a committed Sourcetrail IDE index), plus a checked-in `__pycache__/*.pyc`.
`README.md` has a section explaining that the repo is small and `build/` is what makes zips huge —
while shipping 21 MB of fixtures and an IDE database. Regenerate the big pcapng from a script at
test time and drop the index.

---

## 3. Plan

Ordered by what unblocks adoption, not by what is most interesting to build. Each phase is
independently shippable.

### Phase 0 — Stop the bleeding (days)

The cheap items whose absence is currently costing trust.

**0.1 Make nulls loud instead of wrong.** Until real null support lands (1.1), `ignore_nullability`
must refuse a batch whose validity bitmap has any null set, rather than copying slot bytes. Keep a
separate explicit opt-out (`allow_null_slot_garbage`, or similar) for the pcap pipelines that
knowingly pass nullable-flagged-but-never-null Arrow. Flip the Python default to `False`. This turns
a silent-corruption bug into an error message, in one function, today.

**0.2 Reject what we cannot round-trip.** `large_utf8`, `dictionary`, and `struct` must either read
back correctly or be rejected at `write_batch`. A file our own reader cannot open is not a valid
output. (Prefer fixing; reject as the stopgap.)

**0.3 Fix the quick start.** Make `examples/pcapng2lance/CMakeLists.txt` skip with a
`message(STATUS)` when the submodule is absent, or default `NANOLANCE_BUILD_EXAMPLES=OFF` for
standalone builds. Add a CI job that configures and builds from a **non-recursive** clone, so this
cannot regress.

**0.4 Correct the docs.** Remove or qualify the nullable-column and nested-struct claims in
`README.md` and `AGENTS.md`. Add an explicit, honest "what nanolance cannot read" table (§2.2) near
the top. Reconcile the two version numbers. Re-pin interop claims to a pylance version we actually
test against, not 7.0.0.

**0.5 Repo slim-down.** Drop `st_nl.srctrl*` and the `.pyc`; generate `test_framed.pcapng` from a
fixture script. Target <2 MB tracked.

_Done when:_ a null-bearing write either works or raises; `cmake -S . -B build` succeeds on a fresh
shallow clone in CI; no advertised feature fails its own round trip.

### Phase 1 — Be a Lance reader, and a dataframe-shaped one (weeks)

This is the phase that creates a reason to switch.

**1.1 Real nullability, end to end.** Emit Lance validity/definition information on write; decode it
into Arrow validity bitmaps on read. This is the prerequisite for every other item in this phase and
for any dataframe interop at all. Round-trip tests against pyarrow **and** pylance for every
supported type, at 5k+ rows so the encoders actually engage.

**1.2 Timestamp, date, time, decimal.** All of these are fixed-width integers on the wire; the work
is `schema_mapper` format parsing plus logical-type strings plus reader mapping, not new encoders.
`timestamp` alone unlocks the majority of real parquet files.

**1.3 Decode stock-Lance files by parsing the descriptor the writer already emits.** See §6 —
this turned out to be far smaller and far safer than "implement a general Lance reader," and it
needs no writer change at all.

**1.4 CI that proves it.** A matrix job that, for each supported Arrow type: writes with pylance →
reads with nanolance → compares; and writes with nanolance → reads with pylance → compares. Pin the
pylance version, bump it deliberately. This is the artifact that makes the interop claim credible to
a rust-lance user, and right now nothing like it exists.

_Done when:_ the §2.2 table is all-OK for the types in §2.3, and a nulls-and-timestamps pandas
dataframe round-trips byte-exactly through both readers.

### Phase 2 — Distribution (parallel with Phase 1)

**2.1 Ship wheels.** `cibuildwheel` for manylinux/macOS/Windows × cp39–cp313, publish to PyPI on
tag. Vendor or statically link zstd so the wheel is self-contained. `pip install nanolance` is the
entire on-ramp for the parquet audience, and nothing else in this plan reaches them without it.

**2.2 CMake install/export.** `install(TARGETS ... EXPORT)`, a generated `nanolanceConfig.cmake`, a
`nanolance::nanolance` namespaced target, `find_package(nanolance)` working from an install tree.
Add a consumer smoke test that builds a tiny program against an installed nanolance.

**2.3 Widen CI.** macOS and Windows at minimum for the library and the wheels.

### Phase 3 — The Python API a parquet user expects (weeks)

**3.1 Streaming read.** Add a real `ArrowArrayStream` that decodes fragment by fragment, next to
the materialize-everything `read_table`. This is also a C++-side improvement
(`lance_table_read_dataset` has the same shape).

What it buys, corrected by measurement (see 2.7): **larger-than-memory datasets and time-to-first-
batch**, not the 2× peak. A single-fragment dataset still peaks at 2× after this change, because
that 2× lives inside one fragment's decode. Pair it with 4.1 to get ~1× for any dataset — and note
that 4.1 is the one with the headline number, despite being sequenced later.

**DONE**, as a *separate* entry point rather than a replacement. Making `read_table` stream would
have been a silent contract change: streaming moves error reporting from open to consumption, and
a corrupt file that used to raise `RuntimeError` from `read_table(...)` instead raises `OSError`
from whoever pulls the first batch. Two existing tests caught it. So `open_stream()` is its own
function and `read_table` keeps its eager contract.

Measured on a 61 MiB / 16-fragment dataset: peak **5.6 MiB (0.09×)** against 61.4 MiB (1.01×)
eager, and **3.2 ms** to the first batch against 66.5 ms. Exactly what the correction above
predicted — a throughput-neutral win in peak memory and latency, on top of 4.1's 1.01×.

**3.2 Expose projection**, then add row-range/slice and `count_rows`/schema-peek. Projection already
exists in C++ (`lance_table_read_dataset_projected`) and just needs binding.

**DONE** for projection: `read_table(path, columns=[...])` and `open_stream(path, columns=[...])`,
pushed down to the decoder so unprojected columns are never touched. **`count_rows(path)` and
`read_schema(path)` are done too**, both answered from the manifest without opening a data file
(0.18 ms against 16 ms for a full read of the same 200k-row dataset).

**Row-range/slice is DONE too**, built the honest way described here: the `ReadPlan` now knows each
data file's row span, fragments outside the range are never opened, and only the one or two files a
range partially covers are trimmed. Measured on a 61 MiB / 16-fragment dataset: a 1,000-row range
reads in 2.5 ms against 72.1 ms for the whole table (**29x**), at 0.03x the peak memory.

**3.3 Shape the API like the thing it is replacing.** `nanolance.read_table(path, columns=[...])`,
`nanolance.write_table(table, path, compression=...)` — deliberately `pyarrow.parquet`-shaped, so
the migration from `pq.write_table` is a one-line diff. Document it that way: a side-by-side
"coming from pyarrow.parquet" table beats any benchmark for this audience.

**DONE** for the API shape (it already was) and for the documentation: bindings/python/README.md now
carries a "Coming from `pyarrow.parquet`" side-by-side table, the four differences worth knowing
before switching, and the `nanolance convert` one-liner.

**3.4 A `parquet2lance` entry point.** The single most persuasive demo for the target user is
`python -m nanolance convert in.parquet out.lance` followed by a size comparison. Also add a
non-stdin mode to `arrowipc2lance` (it currently only reads stdin, which surprised me).

**DONE.** `nanolance convert in.parquet out.lance` (and `python -m nanolance convert ...`), plus
`nanolance inspect`. It converts a batch at a time, so a file larger than memory converts fine, and
it prints both sizes. `arrowipc2lance -i/--input FILE` now exists; stdin stays the default.

The demo immediately found something the plan did not anticipate: the converted dataset was
unreadable by stock Lance. See "The dictionary encoding that made whole FILES unreadable" in
PROGRESS -- an item that only surfaced because 3.4 exercised a realistic table.

### Phase 4 — Close the read gap (weeks, after Phase 1)

Sequenced last deliberately: correctness and reach move adoption, and optimizing decode paths that
are about to be rewritten for nullability (1.1) and stock-Lance layouts (1.3) is wasted work.

**That rewrite has now happened** (1.1 and 1.3 are done), so the reason to defer 4.1 is spent. And
the measurement in §2.7 moved 4.1 up in value: it, not 3.1, is what removes the 2× read peak.

**4.1 Decode straight into `ArrowBuffer`.** Delete the `ColumnValues` → `ArrowBuffer` copy (§2.4
cause 2). Largest single win; ~59 MB of redundant traffic on the 1M-row bench.

**DONE**, and taken out of order — see §2.7: this, not 3.1, is what removes the 2× peak. Implemented
by *adopting* the decoded vector's storage into the `ArrowBuffer` (`ArrowBufferDeallocator`) rather
than rewriting the decoder to emit `ArrowBuffer`s, which gets the same zero-copy result for a fraction
of the change. Measured: peak 2.01× → 1.01× (one fragment), read 201 ms → 160 ms.

**4.2 Do not materialize constant columns row-by-row.** Fill the destination buffer once (§2.4
cause 3), or expose Arrow REE/dictionary where the consumer accepts it. Targets exactly the
external-blob workload the project is built around.

**DONE**, and widened. The constant path now writes the value once and doubles it forward
(`log2(N)` memcpys instead of `N`): constant int64 over 4M rows 12.3 ms → 4.5 ms. Constant *strings*
barely move (69 ms → 66 ms) and that is the honest result — 4M copies of a 26-byte value is ~100 MiB
of stores and the fill already runs at ~1.7 GB/s, so that case is memory-bandwidth bound. Only an
encoding that never materializes the bytes (Arrow REE or dictionary) would help, and that changes
the output type, so it is deferred rather than smuggled in.

Profiling the result turned up a bigger, unlisted target: `decode_variable_width_page` appended each
row's offset through its own `vector::resize()`, which callgrind put at **32% of a whole read's
instruction count**. A page's offsets are now grown in one resize and written through a typed
pointer. On a 1M-row int64 + double + string dataset read three times: 373.0M → 235.6M instructions
(−37%), 87 ms → 77 ms wall clock (best of 20, interleaved). memcpy is now 53% of the profile, which
is the floor for a reader that materializes Arrow buffers.

**4.3 Default-init byte buffers. CLOSED — not worth doing.** §2.4 measured ~6.5% for this *before*
4.1 and 4.2. It does not survive them. After 4.2 the remaining zero-fill is the one in
`decode_variable_width_page`'s offset resize, and callgrind still bills it at 5.1% of instructions —
but that number is an artifact: callgrind counts `rep stosb` once **per byte**, so it reports ~12 MB
of zeroing as ~12M instructions. Timed rather than counted, it is free.

The experiment: the same function rewritten to `insert()` the page's offsets (copy, no zero-init)
and shift them in place, which gets the default-init saving with no allocator and no public type
change. Best of 20, interleaved against the resize version on the same dataset:

```
resize + write   80 ms
insert + shift   81 ms
```

Indistinguishable. So the version in the tree is the simpler one, and the allocator-parameterised
`ColumnValues` this item asked for — a custom allocator leaking into a public type, in a library
whose pitch is that it does not do that — buys nothing. This is the second time in this document
that an instruction profile promised a win that wall clock did not pay; §2.4 already warned about
exactly this fix, and the warning was if anything too mild.

**4.4 mmap the data file. CLOSED — the win was in syscalls, and it was not the ones mmap removes.**
The item assumed the cost of `ifstream` page reads was the zero-fill and the kernel→heap copy.
`strace -c` said otherwise. Three reads of the 1M-row dataset issued **38,315 syscalls**, and the
single largest line was not read at all:

```
 48.17%   19,065   newfstatat
 26.68%    9,544   read
 23.00%    9,546   lseek
```

The 19,065 stats were the open-file LRU re-validating size and mtime on *every page-buffer read* —
two syscalls per page, to re-answer a question ("is this still the same file?") that cannot change
partway through one read. `DataFileReadScope` marks a batch read as one operation so each file is
validated on first touch; outside a scope every lookup still validates, so the guarantee is opt-in
rather than something a caller loses by forgetting. The reader also remembers where the stream is and
skips the seek when the next page starts there.

```
newfstatat   19,065 -> 15
lseek         9,546 -> 8,793
wall clock    78 ms -> 68 ms      (best of 15, interleaved)
```

With those gone, the *entire* remaining file-read path — seek, read, and the copy into the page
buffer — prices at **~5% of a read**, measured by doing every page read twice and taking the
difference (74 ms -> 78 ms over three reads). That is mmap's whole ceiling, against a `docs/SAFETY.md`
rewrite, its own fuzz pass, and SIGBUS on a truncated file. Not worth it.

The lseeks that remain are real: Lance pads each page buffer to 8 bytes, so the next buffer usually
starts a few bytes past where the last read stopped. Coalescing a page's two buffers into one read
would remove them, and is worth doing only if that ~5% ever becomes the top of the profile.

**Also measured and rejected: making `MiniBlockChunkView` an actual view.** It is named "View" but
copies each chunk out of the page payload, and callgrind blamed those copies for **25.6%** of a
read's instructions. Priced by running the split twice: 78 ms -> 81 ms over three reads, so removing
it entirely is worth ~3.8%. The refactor would thread a span type through six decoder helper
signatures and give up the `std::swap(chunk.values, raw)` that the zstd and LZ4 paths use. Not a good
trade at that price; revisit only with the coalescing above.

**4.5 Re-profile.** Publish the new callgrind breakdown alongside the bench, the way CI already
does. State the ratio honestly — parity with rust-lance on read is the goal, and if 4.1–4.4 land it
looks reachable.

**Where Phase 4 landed.** Three reads of a 1M-row `int64 + double + string` dataset: **87 ms → 68 ms
(−22%)**, on top of 4.1's 201 ms → 160 ms and its 2.01× → 1.01× peak RSS.

The method mattered more than any single change here, so it is worth stating plainly: **every item
in this phase was decided by wall clock, and the instruction profile was wrong about all four of
them.** It over-sold 4.3 (5.1% of instructions, 0% of time — callgrind counts `rep stosb` once per
byte). It over-sold the chunk-view refactor (25.6% of instructions, 3.8% of time). It under-sold the
file-read path badly enough to hide the largest remaining win entirely (3.9% of instructions, ~24% of
time — syscalls are cheap to count and expensive to make). It was right only about the per-row offset
append, and even there the 32% of instructions paid out as 11% of time. Read `callgrind_annotate` to
find *candidates*; never to size them.

### Phase 5 — Simplicity (opportunistic)

**5.1 One `WriteOptions` struct, one entry point.** The current C API is eight `set_*` calls that
must all precede `write_batch`, enforced at runtime with `INVALID_STATE`. An options struct passed
to `writer_init` makes the ordering constraint unrepresentable instead of documented. Keep the C ABI
stable; add the struct form alongside and let the setters delegate.

**DONE.** `NanoLanceWriteOptions` + `nano_lance_writer_open`. The setters and both `init` forms stay,
and now delegate, so nothing that compiled before stops compiling.

One decision worth recording: **a zeroed struct means the defaults**, and stays meaning them as
fields are added. That forced `disable_structural_encoding` to be spelled as a negation, because
structural encoding is the one option that is ON by default. A positive `structural_encoding` field
would have made `NanoLanceWriteOptions options = {0}` silently disable it — the exact class of trap
this item exists to remove. The C++ `WriteOptions` has member initializers and so spells it
positively.

**5.2 A `nano_lance::Writer` RAII type** wrapping init/commit/close, since every C++ example in the
repo hand-rolls the same lifecycle and `close()`-on-exception is currently the caller's problem.

**DONE.** `nanolance/writer.hpp`. Errors stay in the house style (`bool` + `error()`, not exceptions);
only the lifetime changes. `commit()` defaults `is_append` to whatever `open()` was given, and flips
to `true` after the first successful commit, so the one argument that was easy to get wrong now has a
right answer by default.

Both are guarded by `tests/test_writer_api.cpp`, which asserts **byte-identical data files** between
the old spelling and the new one — for the options struct, for a zeroed struct against plain `init`,
for a NULL options pointer, and for the RAII type against the hand-rolled C sequence. A second way to
write files would be worse than none; the test is there to keep it the same way.

**5.3 Fold `nlance_info`/`nlance2table`/`nlance_stitch`/`arrowipc2lance` into one `nanolance`
CLI** with subcommands. Four binaries with four naming conventions is a discoverability tax; one
`nanolance --help` teaches the whole surface.

**5.4 Retire dead read paths.** `append_fixed_values` in `lance_table_reader.cpp` compares
`arrow_format` as a `std::string` **per value** — it survives only on a fallback path, but it is a
trap for the next person optimizing this file.

---

## 4. Suggested sequencing

Phase 0 first and immediately — it is days of work and it is the difference between "silently wrong"
and "trustworthy." Then Phase 2 (wheels) and Phase 1 (nullability + types) in parallel, since they
are independent and together they are what make the project installable *and* useful. Phase 1.3
(stock-Lance decode) is detailed in §6 and can start immediately — it is additive and needs no
writer change. Phase 3 follows Phase 1.
Phase 4 last, for the reason stated there.

## 5. Explicit non-goals

- Becoming a query engine. No SQL, no predicate pushdown, no indexes. "Fast, small, honest Lance
  I/O" is the differentiator; scan-and-filter is not.
- Write-side parity with every rust-lance encoding. The current encoder set already hits Lance file
  sizes; adding encodings for their own sake trades simplicity for nothing.
- Dropping the safety posture to win the read benchmark. `bench/read_parity_results.md` shows the
  checks cost ~1.0×; that result is worth protecting, and any mmap work (4.4) must preserve it.

---

## 6. Generalizing the reader without touching the writer

_Added after the initial review, in answer to: can the reader be made general while the writer stays
as-is and the verification-reader promise is preserved? **Yes — and it is mostly a dispatch change,
not new decoders.**_

### 6.1 The actual root cause

The writer and the reader speak different languages about the same file.

**The writer already speaks Lance properly.** For every encoding it emits, it writes a real
`/lance.encodings21.PageLayout` message into `ColumnPage` field 4 — `MiniBlockLayout` wrapping a
`CompressiveEncoding` tree (Flat, Variable, Constant, RLE, Dictionary, InlineBitpacking,
ByteStreamSplit, General{ZSTD}). That is exactly why stock Lance reads nanolance output.

**The reader ignores all of it.** `pb::decode_column_page` parses fields 1, 2, 3 and 5 and *skips
field 4* — the encoding descriptor is written and never read back. `decode_lance_physical_column`
instead branches on a nanolance-private side channel in the field metadata:

```cpp
field_metadata_equals(on_disk_field, "nanolance:packing", "constant")   // :415
field_metadata_equals(on_disk_field, "nanolance:packing", "rle")        // :459
field_metadata_equals(on_disk_field, "nanolance:packing", "dict-rle")   // :524
field_metadata_equals(on_disk_field, "nanolance:packing", "dict")       // :644
field_metadata_equals(on_disk_field, "nanolance:packing", "bss-zstd")   // :729
field_metadata_equals(on_disk_field, "nanolance:packing", "bitpack")    // :834
```

A file from stock Lance carries none of those keys, so it falls through to a flat-page assumption
that does not match, and dies on a size check.

### 6.2 How close the two writers actually are

Both writers emit the *same* descriptor namespaces — verified by `strings` on the data files:

```
pylance 12.0.0 :  /lance.encodings.ColumnEncoding   /lance.encodings21.PageLayout
nanolance      :  /lance.encodings.ColumnEncoding   /lance.encodings21.PageLayout
```

And for the case that failed hardest in §2.2 — a plain 5000-row `int64` — the PageLayout bodies are
**byte-identical apart from the chunk row count**:

```
pylance   int64 : 12 12 0a 10 1a 04 2a 02 08 40 32 01 01 38 01 48 88 27 50 01   (0x2788 = 5000 rows)
nanolance int64 : 12 12 0a 10 1a 04 2a 02 08 40 32 01 01 38 01 48 80 08 50 01   (0x0880 = 1024 rows)
                                    ^^^^^^^^^^^
                     CompressiveEncoding{ f5 InlineBitpacking{ bits = 64 } }
```

That node is `InlineBitpacking` — which nanolance **already writes**
(`data_file_writer.cpp:571`, `ce = {0x2a, 0x02, 0x08, bits}`) and **already decodes**
(`unpack_bitpacked_page`, the FastLanes port, fuzzed and ASan-clean).

So the failure on stock-Lance `int64` is not a missing decoder. It is a working decoder that never
gets called, because dispatch asks the field metadata instead of reading the descriptor sitting in
the page. Several of the §2.2 FAILs are this same shape.

### 6.3 The change

Three steps, strictly additive, writer untouched:

1. **Parse `ColumnPage` field 4.** Add a real `PageLayout` / `CompressiveEncoding` decoder to
   `lance_minimal.pb`, generated from the pinned Lance `encodings_v2_1.proto`. This is the mirror of
   serializers the writer already has, and it is what `proto/README.md` has said to do since the
   scaffold went in ("replace or extend that scaffold with nanopb output generated from the pinned
   Lance `v7.0.0-rc.1` files ... `encodings_v2_1.proto`"). The proto dirs are still README-only.
2. **Re-root dispatch on the parsed tree.** Walk `MiniBlockLayout → CompressiveEncoding` and call
   the existing leaf kernels. No kernel is rewritten: FastLanes unpack, RLE expansion, dictionary
   expansion, byte-stream-split and zstd-unframe all stay exactly as they are and as they are
   fuzzed. This is a routing change.
3. **Then** add only what nanolance genuinely never writes: `FullZipLayout`, `AllNullLayout`,
   validity/definition levels (which is the same work as the null support in 1.1), and the logical
   types in 1.2. This is the one genuinely new decoding work, and it is an enumerable list rather
   than an open-ended "support all of Lance."

Steps 1–2 alone should clear the `int64` and `utf8` rows of §2.2 — the two that matter most, since
they are what a rust-lance user's first smoke test contains.

### 6.4 The verification promise survives — and gets stronger

The promise worth keeping is: *everything nanolance writes, nanolance reads back exactly; anything
it cannot handle is a clear error, never silent wrong data.* That is a **test property**. The
current policy enforces it with a **scope restriction** ("only decode what we write"), and the scope
restriction is the part that blocks generality. Swap the mechanism, keep the property:

**A differential oracle, free of charge.** During the transition, keep both decoders. For every file
the writer produces, decode it via the legacy `nanolance:packing` path *and* via the new PageLayout
path and assert the resulting `ColumnValues` are byte-identical. Run it over the whole existing test
corpus, the golden files, and the fuzz corpus. That is an exhaustive proof that the general path
agrees with the verified path on 100% of writer output — a much stronger guarantee than "we only
wrote code for our own files," and something very few parsers get to have when they generalize. When
it is green everywhere, delete the legacy path. The writer can keep emitting the `nanolance:*`
metadata indefinitely; it costs a few bytes and keeps old files readable.

**"Clear error, never best-effort" gets sharper, not weaker.** Today's behaviour is worse than the
policy advertises. `fixed-width page byte count mismatch` on a stock-Lance `int64` is not the reader
recognising a foreign format and declining it — it is the reader *misparsing* foreign bytes under
nanolance layout assumptions and getting caught by a downstream size check. That is why a 3-row
table reads "fine" and a 5000-row one fails: the luck runs out, not the validation. Dispatching on a
typed descriptor replaces that with an honest, named refusal — "unsupported CompressiveEncoding
variant 7" — decided before a single byte is interpreted.

**The safety posture is unchanged and the budget applies as-is.** Every new node goes through the
same `read_safety` overflow-checked bounds and allocation budget; the descriptor is itself untrusted
input and gets validated like any other. Seed `tests/fuzz/fuzz_decode.cpp` with pylance-written
files and coverage goes up, not down. `bench/read_parity_results.md` (~1.0× trusted vs default)
should be re-run after the change and is expected to hold — these checks are per-page, not per-value.

### 6.5 What this does *not* do

It does not make nanolance a general Lance *writer*, and it should not. The writer's encoder set
already hits Lance file sizes; leave it alone. It also does not, by itself, fix nulls (1.1) or the
missing logical types (1.2) — those are genuine feature work that happens to share the
validity-decoding step with 6.3's step 3.

### 6.6 Positioning

With this, the honest claim becomes: **"writes a deliberately small, fast subset of Lance; reads
Lance."** That is a coherent and attractive place to stand — the same shape as the writer/reader
asymmetry argument nanoarrow2parquet's README already makes, inverted. It gives a rust-lance user
something to actually switch *to*, without asking the writer to grow.
