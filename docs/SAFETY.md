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

## Why it isn't slower

Every hot decode loop is *per value*. The rule we follow: **validate once per page/chunk/header, then
run the inner per-value loop check-free.** A declared size or offset table is verified against the real
buffer in a prologue; the tight `memcpy`/materialization loop that follows carries no branches. The
safety cost is therefore O(pages), not O(values) — invisible next to the bytes moved. (The reader's
throughput benchmark is unchanged by this work.)

## How it's proven

Not asserted — exercised in CI ([`.github/workflows/memory-safety.yml`](../.github/workflows/memory-safety.yml)):

- **ASan + UBSan over the whole test suite.** UBSan halts on any undefined behavior; ASan catches
  out-of-bounds and use-after-free. The read path is clean (no OOB / UAF / UB).
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
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan -L smoke
```

## Reviewer checklist (for Rust folks)

- [x] No `reinterpret_cast` of disk bytes to a typed pointer — integers loaded via `load_le<T>`.
- [x] Every disk-derived size/offset validated against the real buffer with overflow-safe math before
      use.
- [x] Declared allocation sizes (zstd, row counts, column counts) capped by a tunable budget.
- [x] Manifest-derived file paths are confined under the dataset; `..`/absolute paths are rejected.
- [x] Read path is ASan + UBSan clean and continuously fuzzed in CI.
- [x] Malformed inputs are rejected with an error return, never a crash or unbounded allocation.

## Known follow-ups (tracked, not yet landed)

- **LSan (leak) checking** is staged off in the sanitizer CI job: a few small leaks remain in
  *writer-side test harnesses* (e.g. Arrow schemas built in tests and not released). The read path is
  leak-clean; enabling LSan globally is a cleanup task.
- **Error-path Arrow release** on partial reads (releasing already-built `ArrowArray`/`ArrowSchema`
  when a multi-column read fails midway) is the next hardening phase (see the project's memory-safety
  plan). External blob range validation currently relies on read/EOF behavior.
