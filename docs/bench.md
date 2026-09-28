# Benchmarks

Two independent claims, each reproducible and each with its own results page:

1. **[Write/read vs Parquet & Rust lance](bench-write-read.md)** — nanolance's write and read
   throughput against `parquet` (zstd) and Rust `lance`, on three synthetic schemas (a pcap-reference
   shape, a wide-int shape, and a high-cardinality-string shape). Auto-generated on every push to
   `main` by [`.github/workflows/linux-bench.yml`](https://github.com/yoavbendor/nanolance/blob/main/.github/workflows/linux-bench.yml)
   — this page always reflects the latest `main`.
2. **[Safety-parity: trusted vs default read](bench-trusted-parity.md)** — proves the memory-safety
   posture costs nothing: `trusted_input=true` (which skips only the untrusted-input DoS/OOM budget
   checks — every bounds check keeps running regardless, see [Memory safety](SAFETY.md)) measures as
   no faster than the default, fully-checked read. Reproduce locally with
   `bench/read_parity_bench.sh [build_dir] [rows]`.

Reproduce the write/read comparison locally with `bench/run-local-bench.sh` (writes
`bench/linux-local-results.md`, kept separate from the CI-owned file so local runs and the CI
auto-commit never collide on the same path).

Reading the write/read table: `parquet`, `rust lance` and `nanolance (py)` all write and read in
the benchmark's own Python process, from the Arrow table already in memory. Those three rows are the
like-for-like comparison. `nanolance (cli)` runs `arrowipc2lance` and `nlbench` as separate
processes. Its `write(proc)` is their whole wall clock: process start, then parsing the Arrow IPC
stream from stdin (on `pcap_ref`, 14 MB of IPC takes about 9 ms, mostly page faults), then the write.
Even its `write(core)` runs in a fresh process, whose first allocations fault in new pages. So the
CLI rows measure the command-line tool, not the encoder.
