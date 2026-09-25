# lance_rs_bench

The pure-Rust side of `tools/bench_matrix.py`: the `lance` crate (12.0.0, the version pylance 12.0.0
is built from) writing and reading a dataset with no Python in the process, reporting times and
peak memory in the same JSON shape as `tools/nlbench`.

```bash
# needs protoc (apt-get install protobuf-compiler); ~17 min and ~6 GB of target/ on 4 cores
cargo build --release
./target/release/lance_rs_bench write in.arrow out.lance 8
./target/release/lance_rs_bench read out.lance 8 --dump check.arrow
```

Built with the crate's default features off -- local files only, no cloud object stores -- which is
the smallest program the crate can make. `Cargo.lock` pins every dependency. `bench_matrix.py`
picks the binary up from `target/release/` (or `LANCE_RS_BENCH`) and leaves the rust-native columns
out when it is not there.
