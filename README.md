# nanolance

Standalone C++ Arrow ↔ Lance writer and reader — write **Lance v2.2** datasets with minimal protobuf overhead (no Rust `lance` core), and read back what you wrote. Its headline feature: rows keep large payloads external (referenced by `uri` + offset + size, never copied) — so a Lance table of pcap packets stays single-digit bytes per row of references *regardless of packet size*, while the payload bytes live once in the source capture and are fetched on demand (numbers in [bench/linux-results.md](bench/linux-results.md)).

nanolance itself depends only on **nanoarrow + zstd** (and local code) — no packet-parsing machinery. The repo also ships **[`examples/pcapng2lance`](examples/pcapng2lance/)**, a streaming pcapng → Lance converter built on the sister **[nanotins](https://github.com/yoavbendor/nanotins)** parsing stack — **soatins** (reflection: describe a struct once → SoA + Arrow) and **nanotins** (pcap/pcapng + L2/L3/L4 decode via the wire_spec core + the spec_dag DAG/FSM dispatcher). That stack is the **example's** dependency, not the library's: it's vendored as a git submodule under [`examples/pcapng2lance/extern/nanotins`](examples/pcapng2lance/extern/nanotins/). (A GPU/CUDA executor layer is a planned future addition, developed separately.)

> **Cloning:** building the `pcapng2lance` example needs the nanotins submodule — clone with `git clone --recursive`, or run `git submodule update --init --recursive`. Building nanolance itself (library, tools, tests) needs no submodule.

> Integrating programmatically (or via an AI agent)? See [AGENTS.md](AGENTS.md) for the current
> include path / CMake targets, the write API, and how to enable each compression measure.

## Layout

- **soatins** (namespace `soatins`, include prefix `soatins/`): reflection nucleus — `be<>`/`le<>` endian-aware fields, bitfield `bits<>`, `soa<T>` columnar store, Arrow `arrow_schema<T>()` / `to_arrow()`. Header-only, depends only on nanoarrow + boost. CMake target: `soatins::core`.
- **nanotins** (namespace `nanotins`, include prefix `nanotins/`): pcap/pcapng scanner + L2/L3/L4 wire structs and decode, built on the **wire_spec** declarative spec system (one explicit-offset spec → host read + device-callable read + SoA + Arrow; see `protocol_specs.hpp`) + **spec_dag** DAG/FSM dispatcher. Also has scheduler-agnostic `bulk_for_each` (over stdexec). Header-only, depends on soatins + header-only stdexec. CMake targets: `nanotins::pcap`, `nanotins::protocols`, `nanotins` (umbrella). (A GPU/CUDA executor layer is developed separately and not vendored here for now.)
- **nanolance** (namespace `nano_lance`, include prefix `nanolance/`): the Lance writer/reader. CMake targets: `nanolance_proto`, `nanolance_reader`, `nanolance` (writing); link `nanolance_reader` alone if you only fetch external blobs.
- **Tool:** `arrowipc2lance` — Arrow IPC stream → Lance dataset (`--version` prints nanolance version); `nlance2table` — Lance dataset → CSV/NDJSON text (for validation).

## Standalone build

Requires CMake 3.22+, C++20, network for first-time FetchContent (nanoarrow, zstd, CLI11).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# Full smoke needs Python with pyarrow for several arrowipc2lance scripts; otherwise use a venv:
#   cmake -S . -B build -DNANO_LANCE_WRITER_PYTHON=/path/to/venv/bin/python
ctest --test-dir build -L smoke --output-on-failure
```

Optional S3 external blobs — just turn the option on and nanolance reads `s3://` on its own using a small
built-in reader (libcurl + OpenSSL, AWS SigV4 range GETs — no AWS SDK):

```bash
cmake -S . -B build -DNANOLANCE_ENABLE_S3=ON
```

The built-in reader resolves its configuration from the environment, following the usual AWS conventions:

| Variable | Purpose |
| --- | --- |
| `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` | Credentials (required). |
| `AWS_SESSION_TOKEN` | Temporary-credential session token (optional). |
| `AWS_REGION` / `AWS_DEFAULT_REGION` | Region for virtual-hosted addressing (default `us-east-1`). |
| `AWS_ENDPOINT_URL` | Custom endpoint, e.g. `http://localhost:9000`; when set, requests use path-style addressing for S3-compatible stores (MinIO, etc.). |
| `AWS_MAX_ATTEMPTS` | Total tries per range GET (default `3`); transient failures (timeouts, dropped connections, `429`/`500`/`502`/`503`/`504`) are retried with full-jitter exponential backoff. |

The reader uses connect/stall timeouts (so an unreachable or hung endpoint fails fast instead of blocking),
auto-corrects a wrong-region bucket once via the `x-amz-bucket-region` redirect hint, and reuses one
keep-alive connection per object. Reading credentials from `~/.aws/*` is out of scope — supply them via the
environment.

The SigV4 signer is unit-tested against AWS's published vectors (`nano_lance_s3_min_sigv4`, runs by default).
A live round-trip test (`nano_lance_s3_min_integration`) is **gated** — it skips unless `NANOLANCE_S3_TEST_URI`
(+ `AWS_*`) point at a bucket holding the pattern object it expects. To exercise it against a throwaway MinIO:

```bash
cmake --build build --target nano_lance_s3_min_integration_test
tests/s3_minio_integration.sh build/nano_lance_s3_min_integration_test   # needs Docker; skips if absent
```

The script starts MinIO, uploads the object, runs ranged reads + multi-window stitching, and asserts a wrong
secret is rejected (proving the endpoint really verifies the signature).

To instead reuse an existing AWS-SDK-backed S3 helper from a parent project, point nanolance at it (this
overrides the built-in):

```bash
cmake -S . -B build -DNANOLANCE_ENABLE_S3=ON \
  -DNANOLANCE_S3_TARGET=your_s3_helper_target \
  -DNANOLANCE_S3_INCLUDE_DIR=/path/to/headers
```

## Writing external references (C++)

nanolance is write-centric and its headline feature is pointing rows at raw bytes that live
elsewhere (a local file, or an object in S3) instead of copying them into the dataset. A row's
`payload_ref` carries a `uri` + `position` + `size`; the bytes are never read at write time.

```cpp
#include "nanolance/blob_builder.hpp"   // build_epb_table_{schema,array}, BlobV2Row
#include "nanolance/nano_lance_writer.h" // C writer API
#include <nanoarrow/nanoarrow.h>

// Two packets whose payloads live in one S3 object at different offsets — no bytes copied.
std::vector<std::uint64_t> packet_ids = {0, 1};
std::vector<nano_lance::BlobV2Row> refs = {
    {/*inline_data=*/std::nullopt, /*uri=*/"s3://bucket/capture.pcapng", /*position=*/2048, /*size=*/1500},
    {/*inline_data=*/std::nullopt, /*uri=*/"s3://bucket/capture.pcapng", /*position=*/3548, /*size=*/512},
};

ArrowSchema schema{};
ArrowArray batch{};
std::string err;
nano_lance::build_epb_table_schema(schema, err);
nano_lance::build_epb_table_array(packet_ids, refs, batch, err);

NanoLanceWriter writer{};
nano_lance_writer_init(&writer, "capture.lance", /*compression_level=*/3);
nano_lance_write_batch(&writer, &batch, &schema);
nano_lance_writer_commit(&writer, /*is_append=*/false);
nano_lance_writer_close(&writer);
// Read the referenced bytes back later with nano_lance_fetch_external_blob(uri, position, size, ...).
```

Link `nanolance` (writer) for the build; `nanolance_reader` is enough if you only fetch blobs.

### Shrinking many refs to one object (URI dictionary, opt-in)

By default each row stores its URI inline, matching Lance's on-disk blob-v2 layout (verified against
`lance` 7.0.0: it stores the external URI per row and only generally compresses it — it does **not**
dictionary-dedup). When many rows point at the *same* object (e.g. millions of packets in one
`.pcapng`), call `nano_lance_writer_set_blob_uri_dictionary(&writer, true)` before writing. Each
distinct URI is then stored once and referenced per row by index, so a reference costs a handful of
bytes instead of the full URI.

This is a **nanolance-only** layout — stock Lance/lance-c cannot read those blob columns — so it is
off by default and create-mode only (not append). nanolance's own reader resolves the URIs
transparently, so the data you read back is identical either way.

## Compression (Lance-compatible)

`nano_lance_writer_set_compression(&writer, true)` (CLI: `--compress`) turns on Lance-compatible
compression, off by default. The output stays readable by stock `lance` (verified against `lance`
7.0.0); nanolance's own reader decodes it transparently.

- **String / binary columns → zstd.** Each chunk's value buffer is stored as `[uint64 LE
  uncompressed size][zstd frame]` and the `PageLayout` advertises `General(ZSTD)`, exactly as the
  Lance reference writer does. A 5000-row repetitive string column round-trips identically and is
  ~2.7× smaller. The zstd level is the writer's `compression_level`.
- **Integer columns (8/16/32/64-bit) → FastLanes bitpacking.** Values are packed in 1024-element
  blocks at the minimum bit width, emitting `InlineBitpacking` (a faithful port of Lance's vendored
  `spiraldb/fastlanes` kernel). A 5000-row int64 column with ~10-bit values is ~6.4× smaller and
  reads back identically under stock `lance`.

Float/bool fixed-width columns are written uncompressed (Lance uses byte-stream-split / other
schemes there, not yet implemented).

## Embedded in streamingtestapps

The parent project sets `NANOLANCE_SOURCE_DIR` and calls `add_subdirectory` with `NANOLANCE_ENABLE_S3`, `NANOLANCE_S3_TARGET`, and `NANOLANCE_BUILD_TESTS` so it reuses nanoarrow, zstd, and `stream_helper_s3` from the main tree.

## Repo size (why a zip looked huge)

**The library sources are small** (on the order of 1–2 MB without `build/`). What explodes disk usage is the **CMake `build/` directory** after a standalone configure:

- FetchContent checkouts live under `build/_deps/` (nanoarrow, zstd, CLI11).
- CMake clones those repos with **`.git` object packs`** (often tens of MB each) unless shallow clones are used.
- Object files and static libs add more under `build/`.

**Nothing in `build/` is part of the “library” you distribute** — delete it before making a zip, or never add it to the archive:

```bash
rm -rf build
du -sh .   # expect ~1–3MB for sources + .git + small fixtures
```

**Prefer a clean export** (no build, no deps):

```bash
git archive --format=zip -o nanolance-src.zip HEAD
```

Standalone configures use **`GIT_SHALLOW TRUE`** on FetchContent to keep *future* `build/_deps` smaller; an existing `build/` from before that change should still be removed or reconfigured from scratch to drop old full clones.

## Benchmarking

Performance benchmarks (pcap-style columns: run-length URI + monotonic position + constant size) and native-reader profiles are in [bench/linux-results.md](bench/linux-results.md). External blob fetching (nanolance vs. Rust Lance) is in [bench/README_blob_fetch.md](bench/README_blob_fetch.md). The per-column compression ratios and method selection are documented in [AGENTS.md](AGENTS.md#3-enabling-the-compression-that-was-measured).

## Version

Version **0.2.0** — macros are generated into `build/include/nanolance/version.h`. Runtime string: `nanolance::library_version()` (declared in `include/nanolance/version.hpp`). `arrowipc2lance --version` includes this string.

Standalone FetchContent pins **nanoarrow** to commit `ffe61d3cd2d02da9e60cfc405cd6c50c512f64b9` for reproducibility (align with a known-good streamingtestapps fetch).

## License

[Apache-2.0](LICENSE). See [NOTICE](NOTICE) and [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) for attributions.

nanolance is an independent C++ implementation of the **Lance** columnar format — it does not link the Rust `lance` crate, but its protobuf message definitions are derived from Lance's Apache-2.0 `.proto` schemas (pinned at `v7.0.0-rc.1`). Apache-2.0 is chosen to align with the Arrow/nanoarrow + Lance ecosystem (all Apache-2.0) and for its explicit patent grant. Build-time deps: nanoarrow (Apache-2.0), zstd (BSD-3-Clause), CLI11 (BSD-3-Clause).
