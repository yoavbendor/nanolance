# nanolance

[![linux-bench](https://github.com/yoavbendor/nanolance/actions/workflows/linux-bench.yml/badge.svg)](https://github.com/yoavbendor/nanolance/actions/workflows/linux-bench.yml) [![memory-safety](https://github.com/yoavbendor/nanolance/actions/workflows/memory-safety.yml/badge.svg)](https://github.com/yoavbendor/nanolance/actions/workflows/memory-safety.yml) [![Python bindings](https://github.com/yoavbendor/nanolance/actions/workflows/bindings-python.yml/badge.svg)](https://github.com/yoavbendor/nanolance/actions/workflows/bindings-python.yml) [![docs](https://img.shields.io/badge/docs-yoavbendor.github.io%2Fnanolance-indigo)](https://yoavbendor.github.io/nanolance/)

Standalone C++ Arrow ↔ Lance writer and reader — write **Lance v2.2** datasets with minimal protobuf overhead (no Rust `lance` core), and read back what you wrote. Its headline feature: rows keep large payloads external (referenced by `uri` + offset + size, never copied) — so a Lance table of pcap packets stays single-digit bytes per row of references *regardless of packet size*, while the payload bytes live once in the source capture and are fetched on demand (numbers in [bench/linux-ci-results.md](bench/linux-ci-results.md)).

nanolance itself depends only on **nanoarrow + zstd** (and local code) — no packet-parsing machinery. The repo also ships **[`examples/pcapng2lance`](examples/pcapng2lance/)**, a streaming pcapng → Lance converter built on the sister **[nanotins](https://github.com/yoavbendor/nanotins)** parsing stack — **soatins** (reflection: describe a struct once → SoA + Arrow) and **nanotins** (pcap/pcapng + L2/L3/L4 decode via the wire_spec core + the spec_dag DAG/FSM dispatcher). That stack is the **example's** dependency, not the library's: it's vendored as a git submodule under [`examples/pcapng2lance/extern/nanotins`](examples/pcapng2lance/extern/nanotins/). (A GPU/CUDA executor layer is a planned future addition, developed separately.)

📖 **[Documentation site](https://yoavbendor.github.io/nanolance/)** ·
🛡️ **[Memory safety & Rust reviewers](docs/SAFETY.md)** ·
🤖 **[AI agent integration guide](AGENTS.md)** ·
📊 **[Benchmarks](bench/linux-ci-results.md)** ·
🤖 **[llms.txt](llms.txt)** (machine-readable index)

> **Cloning:** building the `pcapng2lance` example needs the nanotins submodule — clone with `git clone --recursive`, or run `git submodule update --init --recursive`. Building nanolance itself (library, tools, tests) needs no submodule.

> Integrating programmatically (or via an AI agent)? See [AGENTS.md](AGENTS.md) for the current
> include path / CMake targets, the write API, and how to enable each compression measure.

## Rust roots, hardened read path

nanolance's format is Lance — a Rust-native columnar format. If you're weighing this C++ implementation
against reaching for the Rust crate directly, the question is always the same one: does the C++ side
give up Rust's memory-safety guarantees to get there? The **writer is trusted** (it serializes your own
in-memory Arrow data); the **reader** — the only code that touches untrusted bytes, whether an on-disk
manifest/data-file or an external blob fetched by `(uri, position, size)` — is hardened the same way a
Rust parser would be, and it's proven, not asserted:

- Overflow-checked bounds arithmetic on every disk-derived size/offset before it drives an allocation or
  `memcpy` — no `checked_add`/`checked_mul` wraps into a too-small buffer that a later copy overflows.
- No `reinterpret_cast` of disk bytes to a typed pointer — multi-byte integers load via byte-assembly +
  `std::bit_cast`.
- A tunable allocation budget caps what a hostile file can make the reader allocate (declared zstd size,
  row/column/manifest-element counts).
- A path jail confines every manifest-derived data-file path under the dataset directory; the external
  `file://` blob fetch rejects `..` traversal.
- Mid-read failures release everything already built — no leaked `ArrowArray`/`ArrowSchema`.
- Continuous **ASan + UBSan + LSan CI** and a **libFuzzer** harness over the full decode chain.

All of it validates once per page/header, not once per value, so the safety cost doesn't show up in a
profile — proven by a read-throughput parity benchmark (`trusted_input=true`, which skips only the
untrusted-input DoS budget checks, measures **0.985x** vs. the default fully-checked read — noise, not a
speedup). Full details, the threat model, and a reviewer checklist: **[docs/SAFETY.md](docs/SAFETY.md)**.

## At a glance

- **What it is:** a write-centric C++ library that emits **Lance v2.2** datasets and reads back what it
  wrote — no Rust `lance` core. Everything it writes is readable by stock `lance` (verified vs `lance`
  7.0.0) **unless** a feature is marked *nanolance-only* below.
- **Headline benefit:** rows keep big payloads **external** (`uri` + `position` + `size`, never copied),
  so a packet table costs a few bytes/row regardless of payload size; bytes are fetched on demand (local
  file or `s3://`).
- **Type coverage:** the Arrow C scalar types + `fixed_size_binary`, nullable columns, and nested
  structs (matches [nanoarrow2parquet](https://github.com/yoavbendor/nanoarrow2parquet), so the same
  Arrow batch feeds either writer). Reads back every fixed-width type it writes.
- **Links:** `nanolance` to write; `nanolance_reader` alone if you only fetch external blobs.
- **Python:** fast zero-copy bindings — `pip install -e bindings/python` then `import nanolance` (see
  [bindings/python/README.md](bindings/python/README.md)). Uses the Arrow PyCapsule interface; no hard
  pyarrow dependency at runtime. Does **not** shadow `import lance` (the official `pylance` SDK).

### Quick start (Python)

```python
import pyarrow as pa
import nanolance

table = pa.table({"id": [1, 2, 3], "name": ["alpha", "beta", "gamma"]})
nanolance.write_table(table, "out.lance", compression=True)
assert pa.table(nanolance.read_table("out.lance")).equals(table)
```

Install for development: `pip install -e "bindings/python[test]"` then `pytest` in that directory.

### Quick start (C write)

```c
#include "nanolance/nano_lance_writer.h"
#include <nanoarrow/nanoarrow.h>

NanoLanceWriter w = {0};
nano_lance_writer_init(&w, "out.lance", /*compression_level=*/3);
nano_lance_writer_set_ignore_nullability(&w, true);  // if your Arrow fields are nullable
nano_lance_writer_set_compression(&w, true);         // Lance-compatible compression (off by default)
nano_lance_write_batch(&w, &arrow_array, &arrow_schema);  // repeatable; schema locks after batch #1
nano_lance_writer_commit(&w, /*is_append=*/false);   // false = create, true = append a fragment
nano_lance_writer_close(&w);
// On any non-zero return: nano_lance_writer_last_error(&w). Read back with
// nano_lance::lance_table_read_dataset(...) or nano_lance_fetch_external_blob(uri, position, size, ...).
```

### Gotchas & lifecycle

- **Schema locks after the first `write_batch`** — every batch in a session shares it.
- **Call all `set_*` options before the first `write_batch`** (compression, nullability, URI dictionary).
- **`bool` row fields are not supported** (Arrow's 1-bit storage vs the byte-wide writer path) — use
  `uint8` for flags.
- **Compression is off by default.** One switch (`set_compression`) picks the right Lance encoding per
  column; see [AGENTS.md §3](AGENTS.md#3-enabling-the-compression-that-was-measured) for the per-type
  table.
- **To get small files, model external refs as plain `uri`/`position`/`size` columns**, *not* the packed
  `lance.blob.v2` descriptor (~41 B/row vs ~3.4 B/row). See [AGENTS.md §4](AGENTS.md#4-data-model-how-to-actually-get-small-files-important).
- **The reader is hardened against untrusted files** — bounds/overflow-checked decode, allocation
  budgets, ASan+UBSan CI, and continuous fuzzing. See [docs/SAFETY.md](docs/SAFETY.md) for the threat
  model and reviewer checklist.

### Not yet supported / nanolance-only

- **`bool` fixed-width columns aren't compressed** (written one byte per value; stock Lance bitpacks to
  1 bit/value). `float`/`double` columns *are* compressed when `set_compression(true)` is set — via
  byte-stream-split + zstd, the same technique stock Lance uses, verified byte-for-byte readable by it.
- **No transparent delta encoding** — store monotonic high-range integers as app-level deltas to stay
  small (otherwise they bitpack as absolute values, where Parquet's delta encoding wins).
- **Read throughput is the known gap** (currently ~2–3.5× `lance`, memory-bandwidth bound on column
  materialization — see [bench/linux-ci-results.md](bench/linux-ci-results.md) for current numbers).
- **`nano_lance_writer_set_blob_uri_dictionary` is nanolance-only** (dedups identical external URIs;
  stock Lance cannot read that blob column; create-mode only).
- **S3:** SSO / assume-role profiles aren't resolved — export credentials to the environment first
  (`credential_process`, env, shared profiles, ECS/EKS, IMDSv2 *are* supported).

For the full agent-oriented integration guide (API lifecycle, the measured per-column compression table,
the data-model recipe, and interop verification), read **[AGENTS.md](AGENTS.md)**.

## For AI agents

**Use this library when** you want a Lance dataset — especially one where rows reference large payloads
that live elsewhere (a file or `s3://`) instead of copying them in. It writes and reads back what it
wrote; stock `lance` reads it too (unless you opt into a *nanolance-only* feature).

**Pick a sibling instead when:** you want Parquet output (no external blobs) →
[nanoarrow2parquet](https://github.com/yoavbendor/nanoarrow2parquet) (the *same* Arrow batch feeds
either). You need to *produce* the Arrow from packets/structs →
[nanotins / soatins](https://github.com/yoavbendor/nanotins).

**Minimal program** (`target_link_libraries(app PRIVATE nanolance)`; `nanolance_reader` if you only fetch):

```c
#include "nanolance/nano_lance_writer.h"
#include <nanoarrow/nanoarrow.h>

NanoLanceWriter w = {0};
nano_lance_writer_init(&w, "out.lance", /*compression_level=*/3);
nano_lance_writer_set_ignore_nullability(&w, true);  // BEFORE the first write_batch
nano_lance_writer_set_compression(&w, true);         // BEFORE the first write_batch
nano_lance_write_batch(&w, &arrow_array, &arrow_schema);  // schema locks after batch #1
nano_lance_writer_commit(&w, /*is_append=*/false);   // false = create, true = append fragment
nano_lance_writer_close(&w);
// non-zero return -> nano_lance_writer_last_error(&w)
```

**Do**
- Call every `set_*` option **before** the first `write_batch`; reuse one schema for all batches.
- For small files, model external refs as plain `uri` / `position` / `size` columns (not the packed
  `lance.blob.v2` descriptor) — see [AGENTS.md §4](AGENTS.md#4-data-model-how-to-actually-get-small-files-important).
- Use `uint8` for flag fields, and `fixed_size_binary` (`std::array<uint8,N>`) for MAC/IP-style fields.
- For S3, export credentials to the environment if your profile uses SSO/assume-role.

**Don't**
- Don't use `bool` row fields — unsupported (Arrow 1-bit vs the byte-wide writer path).
- Don't change the schema between batches in one session.
- Don't enable `nano_lance_writer_set_blob_uri_dictionary` if stock Lance must read that column
  (nanolance-only, create-mode only).
- Don't expect `float`/`bool` columns to compress, or integers to delta-encode — store app-level deltas
  for monotonic high-range integers.

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

Optional S3 external blobs — just turn the option on and nanolance reads `s3://` using
[nanos3reader](https://github.com/yoavbendor/nanos3reader), a small read-only S3 range reader (libcurl +
SigV4, no AWS SDK) that nanolance pulls in via CMake `FetchContent`:

```bash
cmake -S . -B build -DNANOLANCE_ENABLE_S3=ON
```

(Embedders with the legacy AWS-SDK seam can still set `NANOLANCE_S3_TARGET` to use that instead.)

The reader resolves **credentials** the way the AWS tools do, trying in order: environment
variables → the shared profile files (`~/.aws/credentials` and `~/.aws/config`, honoring `AWS_PROFILE`),
including a profile's `credential_process` helper → ECS/EKS container credentials → the EC2 instance role
(IMDSv2). Temporary credentials (session tokens) are supported and refreshed before they expire. When no
credentials are found the error names each source it tried and why it failed. Other configuration:

| Variable | Purpose |
| --- | --- |
| `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN` | Credentials via environment (highest precedence; session token optional). |
| `AWS_PROFILE` / `AWS_DEFAULT_PROFILE` | Profile to read from `~/.aws/credentials` + `~/.aws/config` (default `default`). `AWS_SHARED_CREDENTIALS_FILE` / `AWS_CONFIG_FILE` override the paths. |
| `AWS_REGION` / `AWS_DEFAULT_REGION` | Region for virtual-hosted addressing; also read from the profile's `region`, else defaults to `us-east-1`. |
| `AWS_ENDPOINT_URL` | Custom endpoint, e.g. `http://localhost:9000`; when set, requests use path-style addressing for S3-compatible stores (MinIO, etc.). |
| `AWS_MAX_ATTEMPTS` | Total tries per range GET (default `3`); transient failures (timeouts, dropped connections, `429`/`500`/`502`/`503`/`504`) are retried with full-jitter exponential backoff. |

The reader uses connect/stall timeouts (so an unreachable or hung endpoint fails fast instead of blocking),
auto-corrects a wrong-region bucket once via the `x-amz-bucket-region` redirect hint, and reuses one
keep-alive connection per object. `credential_process` profiles are run directly; SSO and assume-role
profiles are not resolved — for those, export credentials into the environment (e.g. via your AWS tooling)
before running.

For deeper notes — credential resolution, the blob-fetch performance gap vs pylance, the swappable SigV4
crypto backend, small static builds (mbedTLS), and the plan to spin the reader out as a standalone library
— see [docs/s3_reader_notes.md](docs/s3_reader_notes.md).

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

Performance benchmarks (pcap-style columns: run-length URI + monotonic position + constant size) and native-reader profiles are in [bench/linux-ci-results.md](bench/linux-ci-results.md) (CI-owned; local runs go to [bench/linux-local-results.md](bench/linux-local-results.md) via `bench/run-local-bench.sh`). External blob fetching (nanolance vs. Rust Lance) is in [bench/README_blob_fetch.md](bench/README_blob_fetch.md). The per-column compression ratios and method selection are documented in [AGENTS.md](AGENTS.md#3-enabling-the-compression-that-was-measured).

## Version

Version **0.2.0** — macros are generated into `build/include/nanolance/version.h`. Runtime string: `nanolance::library_version()` (declared in `include/nanolance/version.hpp`). `arrowipc2lance --version` includes this string.

Standalone FetchContent pins **nanoarrow** to commit `ffe61d3cd2d02da9e60cfc405cd6c50c512f64b9` for reproducibility (align with a known-good streamingtestapps fetch).

## License

[Apache-2.0](LICENSE). See [NOTICE](NOTICE) and [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) for attributions.

nanolance is an independent C++ implementation of the **Lance** columnar format — it does not link the Rust `lance` crate, but its protobuf message definitions are derived from Lance's Apache-2.0 `.proto` schemas (pinned at `v7.0.0-rc.1`). Apache-2.0 is chosen to align with the Arrow/nanoarrow + Lance ecosystem (all Apache-2.0) and for its explicit patent grant. Build-time deps: nanoarrow (Apache-2.0), zstd (BSD-3-Clause), CLI11 (BSD-3-Clause).
