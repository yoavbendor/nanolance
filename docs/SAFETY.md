# Memory safety in nanolance

nanolance is a C++ clone of a Rust-rooted format (Lance). If you're evaluating it against a Rust
implementation, this page is for you: it states the **threat model**, the **defenses**, and how we
**prove** them — and why the safety posture costs no measurable throughput.

## Threat model

nanolance has two kinds of code with very different trust levels:

- **The writer is trusted.** It serializes *your* in-memory Arrow data into bytes we produce. Its inputs
  are Arrow arrays handed to it by your program via the Arrow C Data Interface.
- **The reader is the attack surface.** It parses **untrusted on-disk bytes**: the manifest protobuf,
  the data-file footer, and encoded column buffers (FastLanes bitpacking, dictionary, RLE, constant,
  zstd). It can also **fetch external blobs** by `(uri, position, size)` values that come from the
  file. A hostile or corrupt `.lance` file must not be able to cause out-of-bounds reads/writes,
  use-after-free, integer-overflow-driven allocations, or unbounded memory blow-ups (DoS).

So the hardening effort — and this document — is about the **read path**.

## What's defended (and where)

All read-path size/offset/count fields are attacker-controlled, so before any allocation, `memcpy`,
or slice we validate them. The primitives live in [`include/nanolance/read_safety.hpp`](../include/nanolance/read_safety.hpp):

- **Overflow-checked arithmetic** — `checked_add` / `checked_mul` return `false` instead of wrapping.
  Used for every `offset + size`, `rows * width`, and run/row accumulator that feeds a `resize`,
  `reserve`, or bounds compare.
- **`range_in_bounds(offset, len, total)`** — the canonical "is this slice inside the buffer?" check,
  computed without wrapping. Replaces raw `offset + size > file_size` comparisons that could overflow.
- **`load_le<T>`** — reads little-endian integers from disk via `memcpy` + `std::bit_cast`, never by
  `reinterpret_cast`-ing a disk pointer to `T*` (avoids alignment/aliasing UB).
- **Read limits (`ReadLimits`)** — a budget that bounds what a *hostile* file can make the reader
  allocate, tunable at build time (`-DNANOLANCE_MAX_*`) or at runtime:
  - `max_uncompressed_bytes` (default 8 GiB) — caps a declared zstd uncompressed size *before*
    allocating, and cross-checks it against `ZSTD_getFrameContentSize`.
  - `max_rows_per_column` (default ~17e9) — bounds the summed page row count so constant/RLE/dict-RLE
    expansion can't be told to materialize 10^18 rows.
  - `max_columns` (default 2^20) — bounds the data-file footer column count.
  - `max_manifest_elements` (default 2^24) — bounds manifest field/fragment/file counts.

Concretely, the hardened spots include: the zstd unframer, `append_repeated_value` (constant / RLE /
dict-RLE expansion), the variable-width offset-table sizing, the FastLanes bitpacked-chunk value count,
the data-file footer descriptor/column-count/offset math, and the manifest element counts. The protobuf
decoders already length-bound every field against the remaining input.

### Path & blob-fetch jail

File *paths* on the read path are attacker-controlled too, so they get the same treatment
([`include/nanolance/path_safety.hpp`](../include/nanolance/path_safety.hpp)):

- **Data-file path jail** — `safe_join_under(base, relative)` confines a manifest's `data_file.path`
  under `<dataset>/data/`. A hostile `..`/absolute path (which `path::operator/` would otherwise let
  escape the tree) is rejected before the reader opens anything; the same jail guards the dataset
  stitcher. The writer only ever stores a bare filename here, so real datasets are unaffected.
- **External `file://` blob fetch** — the fetch always rejects a `..` traversal component, and an
  optional base-directory jail (`NANO_LANCE_BLOB_BASE_DIR`) confines every `file://` fetch under a
  configured root when set. `s3://` fetches are unchanged.

### Error-path resource safety

Leaks are a memory-safety story too, especially in a library other people embed:

- **Partial multi-batch reads release everything on failure.** `lance_table_read_dataset[_projected]`
  build one `ArrowArray` batch per data file; if a *later* file fails to decode, the batches already
  pushed for earlier files are no longer silently leaked — the read path releases every batch built so
  far (and the schema) before returning the error, restoring the "failure means empty output, not
  partial ownership" contract the C API (`nano_lance_table_read_dataset`) already assumed.
- **`build_epb_table_schema`** (the pcapng2lance blob-table schema builder) no longer leaks a
  one-byte allocation per call: it re-initialized an already-initialized child `ArrowSchema` slot
  (via a nested `ArrowSchemaInit`) without releasing it first.

## Why it isn't slower

Every hot decode loop is *per value*. The rule we follow: **validate once per page/chunk/header, then
run the inner per-value loop check-free.** A declared size or offset table is verified against the real
buffer in a prologue; the tight `memcpy`/materialization loop that follows carries no branches. The
safety cost is therefore O(pages), not O(values) — invisible next to the bytes moved. (The reader's
throughput benchmark is unchanged by this work.)

### Trusted-input mode: the escape hatch that proves the point

`lance_table_read_dataset(..., trusted_input = true)` (C API: `nano_lance_table_read_dataset_ex`) lets a
self-produced pipeline — round-tripping your own writer output, never a file from another party — opt
out of the four untrusted-input DoS/OOM *budget* checks (declared zstd size, row/column/manifest-element
counts vs. `ReadLimits`). It does **not** disable a single bounds check: `checked_add`/`checked_mul`/
`range_in_bounds` and every offset/size compare built on them are unconditional code, not gated by this
flag, so they run exactly the same whether or not the caller trusts the input.

Because those budget checks were already free (validated once per page/header, not per value), turning
them off measures as noise, not a speedup — which is the whole point: default is already fast, so there's
no safety/speed tradeoff to make. `bench/read_parity_bench.sh` proves it on a synthetic dataset mixing
plain fixed-width, low-/high-cardinality utf8, and constant columns (the writer auto-picks
bitpack/RLE/dict-RLE/constant/plain per column):

```
mode           |    best ms |     avg ms
---------------+------------+-----------
default        |    92.0356 |    98.0309
trusted_input  |     90.682 |    99.1971
```

trusted/default best-ms ratio: **0.985x** (1,000,000 rows, best of 9 reads each — regenerate with
`bench/read_parity_bench.sh [build_dir] [rows]`; results land in
[`bench/read_parity_results.md`](../bench/read_parity_results.md)).

## How it's proven

Not asserted — exercised in CI ([`.github/workflows/memory-safety.yml`](../.github/workflows/memory-safety.yml)):

- **ASan + UBSan + LSan over the whole test suite.** UBSan halts on any undefined behavior; ASan
  catches out-of-bounds and use-after-free; LSan (`detect_leaks=1`) catches leaks. The whole suite —
  read path and writer — is clean.
- **libFuzzer over the decode chain** ([`tests/fuzz/fuzz_decode.cpp`](../tests/fuzz/fuzz_decode.cpp)) —
  feeds arbitrary bytes to the manifest / file-descriptor / column-metadata protobuf decoders and,
  via a temp file, to the data-file footer + column-metadata reader. A local 45s run did 1.1M
  executions with **no crash and bounded RSS** (direct evidence the DoS caps hold). Build it yourself:
  ```
  cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DNANOLANCE_BUILD_FUZZERS=ON
  cmake --build build-fuzz --target nanolance_fuzz_decode
  ./build-fuzz/nanolance_fuzz_decode -max_total_time=60 corpus/
  ```
- **Negative-corpus tests** ([`tests/test_read_safety.cpp`](../tests/test_read_safety.cpp)) — hand-built
  malformed footers (oversized column count, overflowing descriptor bounds) and garbage protobuf must
  be *rejected cleanly*, and the checked-math/`load_le` helpers are unit-tested.

Reproduce the sanitizer run locally:
```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNANOLANCE_SANITIZER=address,undefined
cmake --build build-asan -j
UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan -L smoke
```

## Reviewer checklist (for Rust folks)

- [x] No `reinterpret_cast` of disk bytes to a typed pointer — integers loaded via `load_le<T>`.
- [x] Every disk-derived size/offset validated against the real buffer with overflow-safe math before
      use.
- [x] Declared allocation sizes (zstd, row counts, column counts) capped by a tunable budget.
- [x] Manifest-derived file paths are confined under the dataset; `..`/absolute paths are rejected.
- [x] Read path is ASan + UBSan + LSan clean and continuously fuzzed in CI.
- [x] A mid-read failure releases every batch already built, not just the schema.
- [x] Malformed inputs are rejected with an error return, never a crash or unbounded allocation.
- [x] `trusted_input` only skips DoS-budget checks, never a bounds check — and measures as no faster.

## Known follow-ups (tracked, not yet landed)

- External blob range validation currently relies on read/EOF behavior rather than an upfront
  `position + size` check against the source's known length (cheap for `file://`, not always available
  for `s3://` without an extra HEAD request).
