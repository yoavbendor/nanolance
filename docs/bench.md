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
