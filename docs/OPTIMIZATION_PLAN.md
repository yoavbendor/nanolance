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

- `read_table` calls `nano_lance_table_read_dataset`, which materializes **every** batch, then
  copies them into `ExportedTable`, then frees the originals
  (`bindings/python/src/nanolance_bindings.cpp:253`). Peak memory ≈ 2× the dataset. The docstring's
  promise that a reader "can consume it chunk by chunk" is true of the handle and false of the
  memory profile.
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

**1.3 Decode stock-Lance encodings.** The strategic decision this plan asks for. Today's
"writer-parity only" policy (`docs/lance_table_reader_plan.md`) is what makes §2.2 a wall of FAILs.
Proposal: keep the policy's *safety* discipline (every new decode path fuzzed, budgeted,
bounds-checked) and drop its *scope* restriction. Concretely, work down §2.2 in frequency order —
rust-lance's miniblock layouts for plain int/string first (that is the `int64` and `utf8` rows),
then nulls, then timestamp, then list/dict/struct. Each one is an isolated decoder addition with a
golden file from pylance and a fuzz target.

   This is the largest item in the plan and the one I would most want a decision on before
   starting, because it changes what nanolance *is*: from "a fast Lance writer with a verification
   reader" to "a C++ Lance implementation." The alternative — staying writer-parity-only — is
   coherent, but then the rust-lance switching story should be dropped from the README rather than
   led with, and the project should be positioned purely as a writer.

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

**3.1 Streaming read.** Replace the materialize-everything `read_table` with a real
`ArrowArrayStream` that decodes fragment by fragment. Removes the 2× peak and makes
larger-than-memory datasets work. This is also a C++-side improvement
(`lance_table_read_dataset` has the same shape).

**3.2 Expose projection**, then add row-range/slice and `count_rows`/schema-peek. Projection already
exists in C++ (`lance_table_read_dataset_projected`) and just needs binding.

**3.3 Shape the API like the thing it is replacing.** `nanolance.read_table(path, columns=[...])`,
`nanolance.write_table(table, path, compression=...)` — deliberately `pyarrow.parquet`-shaped, so
the migration from `pq.write_table` is a one-line diff. Document it that way: a side-by-side
"coming from pyarrow.parquet" table beats any benchmark for this audience.

**3.4 A `parquet2lance` entry point.** The single most persuasive demo for the target user is
`python -m nanolance convert in.parquet out.lance` followed by a size comparison. Also add a
non-stdin mode to `arrowipc2lance` (it currently only reads stdin, which surprised me).

### Phase 4 — Close the read gap (weeks, after Phase 1)

Sequenced last deliberately: correctness and reach move adoption, and optimizing decode paths that
are about to be rewritten for nullability (1.1) and stock-Lance layouts (1.3) is wasted work.

**4.1 Decode straight into `ArrowBuffer`.** Delete the `ColumnValues` → `ArrowBuffer` copy (§2.4
cause 2). Largest single win; ~59 MB of redundant traffic on the 1M-row bench.

**4.2 Do not materialize constant columns row-by-row.** Fill the destination buffer once (§2.4
cause 3), or expose Arrow REE/dictionary where the consumer accepts it. Targets exactly the
external-blob workload the project is built around.

**4.3 Default-init byte buffers.** The measured ~6.5% from §2.4. Cheap, self-contained, and the
patch already exists — but land it *after* 4.1/4.2, since those change which buffers still exist.

**4.4 mmap the data file.** Replaces `ifstream` page reads: no zero-fill, no kernel→heap copy, and
for plain fixed-width columns a genuinely zero-copy `ArrowBuffer` pointing into the mapping.
Interacts with the safety model — every bound is still checked, but the "read into a sized buffer"
invariant changes, so this needs its own fuzz pass and a `docs/SAFETY.md` update.

**4.5 Re-profile.** Publish the new callgrind breakdown alongside the bench, the way CI already
does. State the ratio honestly — parity with rust-lance on read is the goal, and if 4.1–4.4 land it
looks reachable.

### Phase 5 — Simplicity (opportunistic)

**5.1 One `WriteOptions` struct, one entry point.** The current C API is eight `set_*` calls that
must all precede `write_batch`, enforced at runtime with `INVALID_STATE`. An options struct passed
to `writer_init` makes the ordering constraint unrepresentable instead of documented. Keep the C ABI
stable; add the struct form alongside and let the setters delegate.

**5.2 A `nano_lance::Writer` RAII type** wrapping init/commit/close, since every C++ example in the
repo hand-rolls the same lifecycle and `close()`-on-exception is currently the caller's problem.

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
(stock-Lance decode) needs the scope decision in §3/1.3 before it starts. Phase 3 follows Phase 1.
Phase 4 last, for the reason stated there.

## 5. Explicit non-goals

- Becoming a query engine. No SQL, no predicate pushdown, no indexes. "Fast, small, honest Lance
  I/O" is the differentiator; scan-and-filter is not.
- Write-side parity with every rust-lance encoding. The current encoder set already hits Lance file
  sizes; adding encodings for their own sake trades simplicity for nothing.
- Dropping the safety posture to win the read benchmark. `bench/read_parity_results.md` shows the
  checks cost ~1.0×; that result is worth protecting, and any mmap work (4.4) must preserve it.
