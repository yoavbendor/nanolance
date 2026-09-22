# nanolance — integration guide for AI agents

This file tells an automated agent how to integrate with nanolance **as it is now**, after a round of
interface changes and compression work. If you previously knew this library, re-read §1 — the include
path and CMake targets changed.

nanolance is the "nanoarrow of Lance": a small C++ library that **writes Lance v2.2 datasets** (and
reads back what it wrote) with no Rust `lance` core. Its headline feature is pointing rows at raw
bytes that live elsewhere (local file or S3) instead of copying them in. Everything it writes is
readable by stock `lance` (verified against `pylance` 12.0.0), unless a feature is explicitly marked
"nanolance-only" below.

## 1. What changed (breaking)

- **Public headers moved to `include/nanolance/`** (was `include/nano_lance_writer/`). Update every
  include: `#include "nanolance/nano_lance_writer.h"`, `#include "nanolance/nano_lance_reader.h"`, etc.
- **CMake targets are `nanolance`, `nanolance_reader`, `nanolance_proto`** plus the alias
  `nanolance::nanolance`. The old `nano_lance_writer` / `nano_lance_reader` / `nano_lance_proto_minimal`
  target aliases were removed. Link `nanolance` for writing; `nanolance_reader` alone if you only fetch
  external blobs.
- The C ABI symbol names are unchanged (`nano_lance_writer_*`, `nano_lance_*`). Only the header path
  and CMake target names changed.

## 2. Minimal write flow (C API)

```c
#include "nanolance/nano_lance_writer.h"
#include <nanoarrow/nanoarrow.h>

NanoLanceWriter w = {0};
nano_lance_writer_init(&w, "out.lance", /*compression_level=*/3);
nano_lance_writer_set_compression(&w, true);         // enable Lance-compatible compression (see §3)
nano_lance_write_batch(&w, &arrow_array, &arrow_schema);  // call repeatedly; schema is fixed after #1
nano_lance_writer_commit(&w, /*is_append=*/false);
nano_lance_writer_close(&w);
// On any non-zero return, read nano_lance_writer_last_error(&w).
```

Lifecycle rules:
- Schema is locked after the first batch; all batches in a writer session share it.
- All `set_*` options must be called **before the first `write_batch`**.
- **Nulls are stored** via Lance's definition-level layer, and stock Lance reads them back: in
  fixed-width columns (int, float, bool, timestamp/date/time, decimal, `fixed_size_binary`) and in
  variable-width ones (`utf8`, `binary`). Still refused, by name: a null **struct** (as opposed to a
  null field inside one), and a null in a `lance.blob.v2` external-reference column.
  `nano_lance_writer_set_ignore_nullability` is a no-op kept for compatibility; the manifest's
  nullable flag now mirrors the Arrow schema, as pylance's does.
- Types nanolance cannot round-trip are refused at `write_batch` rather than written: `list`,
  `large_utf8`/`large_binary`, Arrow `dictionary` columns, Arrow's `null` type, and a `timestamp`
  whose timezone is a UTC offset rather than an IANA name (Lance panics on those). Supported:
  int/uint 8–64, `float`, `double`, `bool`, `utf8`, `binary`, `fixed_size_binary(N)`, `timestamp`
  (s/ms/us/ns, optionally with an IANA timezone), `date32/64`, `time32/64`, `decimal128/256`,
  nested `struct`, and `lance.blob.v2` external references.
- nanolance reads back everything it writes, and now every non-nested column type the Rust `lance`
  crate writes: fixed-width and temporal types, nullable columns (bit-packed *and* run-length-encoded
  definition levels), `utf8`/`large_utf8`/`binary` including FSST-compressed ones, and categorical
  columns with their LZ4-compressed dictionary. `list` and `struct` are still refused **by name**,
  not misread (see README "What nanolance can read").
- `commit(is_append=false)` creates; `commit(is_append=true)` (or `nano_lance_writer_init_append`)
  adds a fragment to an existing dataset.

`arrowipc2lance` reads its Arrow IPC stream from `-i/--input FILE` or, with neither given, stdin.

Read back with `nano_lance::lance_table_read_dataset(path, schema, batches, error)` (C++,
`nanolance/lance_table_reader.hpp`) or fetch external bytes with
`nano_lance_fetch_external_blob(uri, position, size, ...)`.

## 2a. Python bindings

First-class Python bindings live in [`bindings/python/`](bindings/python/) (`pip install -e bindings/python`).
They wrap the same C API via nanobind and the Arrow PyCapsule interface — zero-copy in/out of pyarrow,
polars, etc. The module is `import nanolance` (not `lance`; that name is the official `pylance` SDK).

```python
import pyarrow as pa
import nanolance

table = pa.table({"uri": ["s3://b/f.pcapng"] * 1000, "position": range(1000), "size": [1500] * 1000})
nanolance.write_table(table, "out.lance", compression=True)
roundtrip = pa.table(nanolance.read_table("out.lance"))

# Projection, and a fragment-at-a-time stream for datasets larger than memory:
subset = pa.table(nanolance.read_table("out.lance", columns=["uri", "size"]))
for batch in pa.RecordBatchReader.from_stream(nanolance.open_stream("out.lance")):
    ...
```

Key options mirror the C writer: `compression=True`, `WriteOptions(append=True)`, etc. See
`bindings/python/README.md` for install, tests (`pytest`), and pylance interop checks.

`read_table` decodes up front and raises at call time; `open_stream` decodes one fragment per pull,
so errors surface from the first pull instead (as `OSError`) and the handle is single-shot. Prefer
`open_stream` for bounded peak memory (measured 0.09x the dataset against 1.01x) and a fast first
batch; prefer `read_table` when you want the whole thing and up-front errors.

The package also installs a CLI, `nanolance` (equivalently `python -m nanolance`):

```bash
nanolance convert in.parquet out.lance --compress   # batch at a time; prints both sizes
nanolance inspect out.lance                         # rows, on-disk bytes, schema
```

`convert` takes `--columns`, `--rows-per-fragment`, `--batch-size`, `--no-structural`,
`-l/--compression-level` and `--overwrite`. It refuses a non-parquet extension and an existing output
by name rather than guessing.

Parquet Python bindings are a separate package in
[nanoarrow2parquet](https://github.com/yoavbendor/nanoarrow2parquet) (`nanoarrow_io.parquet`).

## 3. Enabling the compression that was measured

There are **two independent knobs** (this changed — there used to be one `--compress` switch):

- **Structural encodings** — bitpacking, ConstantLayout, RLE, dictionary, dict+RLE. These are lossless
  and cheap (usually *faster* to write than plain, always smaller), so they are **ON by default**:
  `nano_lance_writer_set_structural_encoding(&w, true)` (default). Disable with the setter, or
  `arrowipc2lance --no-structural`, to emit plain flat/variable-width pages ("raw" Lance output).
- **zstd** — a real CPU-for-size tradeoff, applied to high-cardinality variable-width (string/binary)
  columns, and to `float`/`double` fixed-width columns (byte-stream-split + zstd — the same technique
  stock Lance itself uses for these types, since compressing already-interleaved float bytes barely
  helps). **Off by default**: `nano_lance_writer_set_compression(&w, true)` (CLI `--compress`).

Both stay readable by stock `lance`; nanolance's own reader decodes them transparently. The picked
encoding per column type:

| Column type | Encoding applied | Knob | Measured (50k pcap-like rows) |
|---|---|---|---|
| Integer 8/16/32/64-bit | FastLanes **InlineBitpacking** (1024-value blocks) | structural (default) | int64 ~10-bit values: 84 → 3.4 B/row |
| Constant column (all rows equal), fixed **or** string/binary | **ConstantLayout** (value stored once; ~0 data bytes) | structural (default) | constant int → 0.007, constant URI string → 0.009 B/row |
| Low-cardinality run-length column, fixed **or** string | **RLE** (ints) / **Dictionary+RLE** (strings) | structural (default) | run-length URI: 2.90 → 0.033 B/row (87×); run-length int: → 0.027 B/row |
| String / binary (non-constant, high-cardinality) | **zstd** (`General(ZSTD)`, `[u64 len][zstd]` per chunk) | `--compress` (opt-in) | repetitive string: 7.6 → ~3 B/row |
| `float` / `double` fixed-width | **byte-stream-split + zstd** (`General(ZSTD){ ByteStreamSplit{ Flat } }`) | `--compress` (opt-in) | smooth double column: ~23% smaller |
| `bool` fixed-width | **bit-packed** (`Flat{bits_per_value:1}`, LSB-first, same as stock Lance) | always on | 200k rows: ~200 KB → ~25 KB (8×) |

So a blob.v2 run-length URI column is now tiny **without** `--compress`; `--compress` only adds zstd on
top for the genuinely high-cardinality string columns. `--compress` output is byte-identical to the
old single-switch `--compress` (structural was always part of it); the default (no `--compress`) output
now carries the structural encodings instead of plain pages.

`compression_level` is the zstd level (also used as a hint; 0 = zstd default) and applies only to the
zstd path; structural encodings ignore it.

### nanolance-only option: external-URI dictionary

`nano_lance_writer_set_blob_uri_dictionary(&w, true)` deduplicates identical external URIs in a
`lance.blob.v2` column (stores each URI once, references by index). It is **not Lance-readable** for
that column and is create-mode only. Use it only if you stick with the packed blob-v2 descriptor and
do not need third-party Lance tools to read those blob columns.

## 4. Data model: how to actually get small files (important)

To compete with Parquet, **model external references as ordinary typed columns**, not the packed
`lance.blob.v2` descriptor:

- Good: three columns `uri` (string), `position` (uint64), `size` (uint64). You fetch payloads
  yourself with `nano_lance_fetch_external_blob(uri, position, size, ...)`.
- Avoid for size: the blob-v2 FullZip packed descriptor. Measured at ~41 B/row because it row-zips
  raw `position`/`size`/`uri` per row with no per-column dictionary/RLE/bitpacking.

Measured, same 50k rows, `--compress`, columns model: **3.39 B/row** (constant URI 0.009 + constant
size 0.007 + bitpacked monotonic position 3.38), at parity with the Lance reference writer (3.21) and
now dominated only by the `position` column. The blob-v2 packed descriptor for the same data was
41 B/row. Prefer the columns model unless you specifically need Lance's native blob-fetch semantics.
To push `position` lower, store it as first-offset + deltas at the application level (the delta column
becomes constant/low-range → ~0); Lance has no transparent delta encoding. True low-cardinality
(non-constant) string columns still fall back to zstd until RLE/dictionary land.

Tips that help the encoders:
- Constant fields (snaplen, a fixed capture size, a per-file URI as a separate constant column) →
  ConstantLayout → ~0.
- Monotonic offsets: bitpacking handles them; storing first-offset + deltas as the column (your
  choice) makes the delta column constant/low-range → near-0 (the un-delta is your application's job;
  Lance has no transparent delta encoding).

## 4a. Measured performance vs Parquet and Rust Lance

200k rows, Ubuntu CI, clang Release, best-of-5 writes / best-of-7 reads. Reproduce with
`tools/bench.py`; the live numbers are committed to `bench/linux-ci-results.md` by the GitHub Actions
workflow on every push (CI-owned file). Local runs write `bench/linux-local-results.md` via
`bench/run-local-bench.sh` so they never clash with the CI auto-commit.

**Write-core ratio vs rust lance per dataset** (after the write-path optimization rounds — scan
early-exits, plan reuse, chunk streaming through reused scratch buffers, reused zstd contexts):

| dataset | nanolance vs rust lance (write core) |
|---|---|
| `float_smooth` (byte-stream-split + zstd) | **~0.75× — faster** |
| `bool_flags` (1-bit packing) | **~0.4× — faster** |
| `pcap_ref` (dict-RLE URI + bitpack + constant) | **~1.05× — parity** |
| `wide_int` (integer bitpack) | ~1.2× |
| `high_card` (zstd strings) | ~1.5× (remaining gap) |

- **Size:** at Lance parity, **beats Parquet** — the design goal.
- **Fair-comparison note (floats):** rust lance's *default* leaves floats essentially uncompressed
  (~raw 12 B/row); `tools/bench.py` sets `lance-encoding:compression=zstd` + `lance-encoding:bss=on`
  field metadata on the rust write of `float_smooth` so both engines do byte-stream-split + zstd
  (~8.4-8.7 B/row) — without that, the bench compared our compressed write to rust's uncompressed one.
- **Write:** `write(core)` excludes subprocess startup + Arrow-IPC parse (3–9 ms of the CLI's wall
  clock); `write(proc)` in the bench includes them.
- **Read:** float/bool reads are at or beyond rust-lance parity (nanolance reads compressed floats
  ~2.5× faster than rust reads its own); integer-bitpack reads ~1.3×; string-heavy reads ~1.6-1.8×
  remain the gap (memory-bandwidth bound on column materialization).

Where Parquet wins: high-cardinality strings and monotonic **high-range** integers (Parquet
delta-encodes; Lance and nanolance bitpack absolute values). Store such columns as app-level deltas to
recover the win. The **flat** fixed-width path (uncompressed floats/doubles and any non-bitpacked
fixed width) now chunks at the same 32 KB miniblock max as the variable-width path; the old 800-byte
cap turned a large float column into thousands of tiny one-chunk pages and made float/struct writes
allocation-bound (fixed — float/struct writes are now at Lance parity or faster).

## 5. Verifying Lance interop (do this after changes)

```bash
# build (see §6), then:
python - <<'PY'
import lance
ds = lance.dataset("out.lance"); t = ds.to_table()
print(t.num_rows, t.schema)   # values must match what you wrote
PY
```
The repo's C++ smoke tests cover writer→reader round-trips without Python. Run:
`ctest --test-dir build -L smoke`. Keep new encodings behind a round-trip test that also checks the
on-disk size shrank.

## 6. Build notes

- Standalone: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`.
  First configure uses FetchContent for nanoarrow (pinned), zstd, CLI11 (needs network).
- **zstd under MinGW/Windows:** if a system MSVC `zstd_static` is on the prefix path, linking fails
  with undefined `__security_cookie`. Force the source build:
  `-DCMAKE_DISABLE_FIND_PACKAGE_zstd=ON`.
- Tools (`arrowipc2lance`) need CLI11; turn off with `-DNANOLANCE_BUILD_TOOLS=OFF` if unavailable.
- **`-DNANOLANCE_ENABLE_AVX2=ON`** compiles nanolance's own targets (not FetchContent'd dependencies)
  with `-march=haswell` (AVX2 + BMI2 + FMA). Off by default because it raises the minimum supported CPU
  to Haswell-class (~2013+ Intel/AMD) for whoever links the library — a real portability tradeoff, not a
  free win, so it must be opted into explicitly. Measured (callgrind, this repo's own benchmark
  datasets): bitpack-heavy columns ~11-20% fewer instructions on write/read; zstd-heavy high-cardinality
  string columns ~4-11% (zstd's own compiled-in dispatch is unaffected — the win here is nanolance's own
  auto-vectorizable loops); float/bool paths see negligible change (already memory-bound). A *scoped*
  per-function `[[gnu::target("avx2")]]`/`target_clones` dispatch was tried and measured to give no
  benefit on the FastLanes pack/unpack kernels specifically, because it blocks inlining across the
  attribute boundary — the win only shows up compiling the whole translation unit at this target, which
  is what this flag does.

## 7. When adding a new Lance encoding (how this codebase does it)

1. Find the authoritative format in the Lance Rust source (`protos/encodings_v2_1.proto` for the
   `PageLayout` / `CompressiveEncoding` field numbers; `rust/lance-encoding/src/encodings/...` for the
   byte layout). Don't guess from prose docs — verify against real `lance` output bytes.
2. Writer: build the exact `PageLayout` protobuf + data buffer in `src/data_file_writer.cpp`.
3. Reader: invert it in `src/lance_column_decoder.cpp`. The decoder dispatches on the on-disk field's
   `encoding` and `nanolance:*` metadata tags set by the writer (Lance ignores those tags and reads the
   real `PageLayout`).
4. Validate both directions: nanolance write → nanolance read, **and** nanolance write → `lance` read.
