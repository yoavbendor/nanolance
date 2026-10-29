# nanolance

[![linux-bench](https://github.com/yoavbendor/nanolance/actions/workflows/linux-bench.yml/badge.svg)](https://github.com/yoavbendor/nanolance/actions/workflows/linux-bench.yml) [![memory-safety](https://github.com/yoavbendor/nanolance/actions/workflows/memory-safety.yml/badge.svg)](https://github.com/yoavbendor/nanolance/actions/workflows/memory-safety.yml) [![Python bindings](https://github.com/yoavbendor/nanolance/actions/workflows/bindings-python.yml/badge.svg)](https://github.com/yoavbendor/nanolance/actions/workflows/bindings-python.yml) [![docs](https://img.shields.io/badge/docs-yoavbendor.github.io%2Fnanolance-indigo)](https://yoavbendor.github.io/nanolance/)

A C++20 implementation of the [Lance](https://lance.org/format/) columnar format, without the Rust `lance` core:
it writes and reads Lance datasets, builds and searches Lance's indexes, and changes datasets the way Lance does
(appends, deletes, updates, merge-insert, schema changes, compaction, versions, tags, branches), on local disk and
on S3-compatible object stores. What it writes, pylance reads; what pylance writes, it reads; the two use each
other's indexes with the same answers (checked against `pylance` 12.0.0 in CI).

It depends on **nanoarrow** and **zstd**, plus the header-only **[nanom](https://github.com/yoavbendor/nanom)** for
Lance's protobuf metadata. A program links only the parts it calls: a writer of local datasets is under 1 MB.

📖 [Documentation site](https://yoavbendor.github.io/nanolance/) ·
🧱 [How the code is organized](#how-the-code-is-organized) ·
📋 [Spec coverage](#spec-coverage) ·
🛡️ [Memory safety](docs/SAFETY.md) ·
📊 [Benchmarks](#benchmarks) ·
🤖 [Agent integration guide](AGENTS.md)

## What it does

- **Files:** writes Lance file format 2.2 and reads 2.0, 2.1 and 2.2, with Lance's encodings:
  bitpacking, run-length, dictionary, FSST, byte-stream split, zstd and LZ4, constant and all-null pages,
  mini-block and full-zip layouts, nested and repeated columns ([src/encoding](src/encoding/README.md),
  [src/file](src/file/README.md)).
- **Tables:** manifests, fragments, deletion files, stable row ids, data overlays, transactions with Lance's
  conflict rules, versions, tags and branches ([src/table](src/table/README.md)).
- **Indexes,** built and searched in Lance's format: scalar (BTree, Bitmap, LabelList, ZoneMap, BloomFilter,
  NGram), full-text (INVERTED, with phrase, fuzzy and boolean queries), and vector (IVF_FLAT, IVF_PQ, IVF_SQ,
  IVF_RQ, IVF_HNSW_FLAT/PQ/SQ). An index survives the dataset's appends, deletes, updates and compactions
  ([src/index](src/index/README.md)).
- **External payloads:** a row can reference bytes that live elsewhere (`uri` + `position` + `size`, a local file
  or `s3://`) instead of copying them in, so a table of packets costs a few bytes a row whatever the payload
  size; the bytes are fetched on demand.
- **Object stores:** reads and writes on `s3://` and S3-compatible stores (MinIO, R2), with Lance's commit
  semantics, through [nanos3reader](https://github.com/yoavbendor/nanos3reader) (libcurl + SigV4, no AWS SDK).
- **Text in any script:** filters compare by character (`LIKE`, `ILIKE`, `lower`, `upper`, `regexp_like` answer as
  pylance does for Hebrew, Arabic, Cyrillic, Greek...), and full-text indexes can ignore Hebrew and Arabic
  pointing and match words with and without diacritics ([docs/FTS_INDEX.md](docs/FTS_INDEX.md)).
- **Three ways in:** a C++ library; a C writer/reader API; `liblance_c`, lance-c's C API over nanolance (code
  written for lance-c links unchanged, [docs/LANCE_C_COMPAT.md](docs/LANCE_C_COMPAT.md)); and Python bindings
  whose `nanolance.lance` module speaks pylance's API ([docs/PYLANCE_COMPAT.md](docs/PYLANCE_COMPAT.md)).
- **A hardened reader:** every size and offset read from a file is bounds- and overflow-checked before it drives
  an allocation or a copy, allocations are budgeted, paths are jailed, and the decode path runs under ASan, UBSan
  and libFuzzer in CI ([docs/SAFETY.md](docs/SAFETY.md)).
- **WebAssembly:** the reader builds for the browser and reads datasets over HTTP range requests (`wasm/`).

## Quick start

### Python

```bash
pip install -e bindings/python        # wheels: .github/workflows/wheels.yml
```

```python
import pyarrow as pa
import nanolance.lance as lance       # pylance's API; or nanolance.lance.install_as_lance(), then `import lance`

table = pa.table({"id": [1, 2, 3], "text": ["שלום עולם", "hello world", "fată frumoasă"]})
ds = lance.write_dataset(table, "out.lance")
ds.create_scalar_index("id", "BTREE")
ds.create_scalar_index("text", "INVERTED")
ds.to_table(filter="id > 1", columns=["text"])
ds.to_table(full_text_query="hello")
ds.delete("id = 3")
lance.dataset("out.lance", version=1).take([2, 0])
```

The smaller native API (`nanolance.write_table`, `read_table`, `open_stream`) and the `nanolance` command line
(`convert` from Parquet, `inspect`) are described in [bindings/python/README.md](bindings/python/README.md).

### C

```c
#include "nanolance/nano_lance_writer.h"
#include <nanoarrow/nanoarrow.h>

NanoLanceWriteOptions options = {0};                 // zeroed == the defaults
options.compression_level = 3;
options.compression = true;                          // zstd for strings, byte-stream split for floats

NanoLanceWriter w = {0};
nano_lance_writer_open(&w, "out.lance", &options);
nano_lance_write_batch(&w, &arrow_array, &arrow_schema);  // repeatable; the schema is fixed by the first
nano_lance_writer_commit(&w, /*is_append=*/false);   // false: create, true: append a fragment
nano_lance_writer_close(&w);
// A non-zero return: nano_lance_writer_last_error(&w).
```

### C++

```cpp
#include "nanolance/writer.hpp"
#include "nanolance/lance_table_reader.hpp"

nano_lance::Writer writer;                           // owns the handle; the destructor closes it
if (!writer.open("out.lance", {.compression = true}) || !writer.write_batch(&array, &schema) || !writer.commit()) {
    return writer.error();
}

nano_lance::LanceScanRequest request;                // columns, filter, range, version, row ids...
const std::string filter = "id > 1";
request.filter = &filter;                            // answered from the scalar indexes when they can
ArrowSchema out_schema{};
std::vector<ArrowArray> batches;
std::string error;
nano_lance::lance_dataset_scan("out.lance", request, out_schema, batches, error);
```

## Building

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -L smoke               # or -L layer:table for one layer's tests
```

CMake 3.22+ and a C++20 compiler (GCC 12 or later, Clang, AppleClang); the first configure fetches nanoarrow, zstd,
nanom and CLI11. Behind a firewall, `NANOLANCE_GIT_BASE` points every fetch at a mirror
([docs/BUILDING_BEHIND_A_FIREWALL.md](docs/BUILDING_BEHIND_A_FIREWALL.md)).

| Option | Default | |
|---|---|---|
| `NANOLANCE_ENABLE_S3` | OFF | `s3://` reads and writes through nanos3reader (needs libcurl) |
| `NANOLANCE_BUILD_TESTS` | ON standalone | the C++ tests, registered with ctest |
| `NANOLANCE_BUILD_TOOLS` | ON | the `nanolance` command line (import, info, cat, stitch) |
| `NANOLANCE_BUILD_LANCE_C` | ON | `liblance_c`, lance-c's C API |
| `NANOLANCE_WITH_JIEBA` | ON | the jieba tokenizer for Chinese full-text search (embeds its model) |
| `NANOLANCE_ENABLE_AVX2` | OFF | compile for Haswell and later (AVX2, BMI2, FMA) |
| `NANOLANCE_WERROR` | OFF | warnings are errors on nanolance's own targets (CI turns it on) |
| `NANOLANCE_SANITIZER` | | e.g. `address,undefined` |
| `NANOLANCE_BUILD_FUZZERS` | OFF | libFuzzer harnesses over the decode path (Clang) |
| `NANOLANCE_BUILD_EXAMPLES` | ON standalone | `examples/pcapng2lance` (needs the nanom submodule: `git clone --recursive`) |

**What a program links** is what it calls: the library is static and split by layer, so a writer of local
datasets does not carry the index code or the S3 client. Measured, stripped, x86-64 Release: a local writer
about 0.9 MB, a writer to S3 about 1.2 MB, everything about 3.6 MB. A reader that must not link the index code
calls `lance_dataset_scan_core` instead of `lance_dataset_scan` ([src/scan](src/scan/README.md)).

Embedding in another CMake project: `add_subdirectory` with `NANOLANCE_BUILD_TESTS` off, then link `nanolance`
(`nanolance::nanolance`); `nanolance_reader` alone is enough to fetch external payloads.

## How the code is organized

`src/` is eleven layers, each using only the ones below it (`tools/check_layers.py` checks every include in CI).
Each layer's README says what it is for, how to use it, and what of the Lance specification it implements;
its tests are in `tests/<layer>/` (`ctest -L layer:<layer>`).

The table runs from the bottom layer (1, base) to the top layer (11, api). [docs/LAYERS.md](docs/LAYERS.md) has
the diagram and each layer's direct dependencies.

| Number | Layer | What it is |
|---|---|---|
| 1 | [base](src/base/README.md) | threads, buffers, caches, Unicode, small utilities |
| 2 | [io](src/io/README.md) | where bytes live: local files and object stores |
| 3 | [arrow](src/arrow/README.md) | Arrow arrays and schemas, and how they map to Lance's types |
| 4 | [encoding](src/encoding/README.md) | how one page of one column is laid out, both directions |
| 5 | [file](src/file/README.md) | a `.lance` file: pages, column metadata, global buffers, footer |
| 6 | [table](src/table/README.md) | what a version is: manifest, fragments, deletions, row ids, transactions, refs |
| 7 | [expr](src/expr/README.md) | what a filter means |
| 8 | [scan](src/scan/README.md) | reading rows of fragments, given a row selection |
| 9 | [index](src/index/README.md) | each index kind: build, search, describe |
| 10 | [dataset](src/dataset/README.md) | user operations: writes, updates, merge-insert, compaction, index lifecycle |
| 11 | [api](src/api/README.md) | the C APIs and lance-c |

[src/README.md](src/README.md) has the rules between them. Public headers are in `include/nanolance/`.

## Spec coverage

Every layer README ends with a table: one row per item of the [Lance format specification](https://lance.org/format/)
(an encoding, a page layout, a manifest feature, a transaction and its conflict rules, an index's files), its
status, where it is implemented, and the tests that prove it, marked *unit* (nanolance's own tests) or *interop*
(compared against files pylance writes or reads). `tools/check_spec.py` checks in CI that every named function and
test exists and that every module has a test of its own.

<!-- spec-summary:start -->
| layer | done | partial | missing | n/a | modules with a test in their layer |
|---|---|---|---|---|---|
| [base](src/base/README.md#spec-coverage) | 14 | 0 | 0 | 1 | 16/16 |
| [io](src/io/README.md#spec-coverage) | 7 | 2 | 2 | 0 | 5/7 |
| [arrow](src/arrow/README.md#spec-coverage) | 18 | 3 | 0 | 1 | 8/8 |
| [encoding](src/encoding/README.md#spec-coverage) | 22 | 6 | 6 | 1 | 19/19 |
| [file](src/file/README.md#spec-coverage) | 14 | 3 | 3 | 1 | 4/4 |
| [table](src/table/README.md#spec-coverage) | 37 | 16 | 13 | 1 | 17/17 |
| [expr](src/expr/README.md#spec-coverage) | 17 | 1 | 3 | 0 | 1/1 |
| [scan](src/scan/README.md#spec-coverage) | 19 | 2 | 1 | 1 | 1/1 |
| [index](src/index/README.md#spec-coverage) | 31 | 14 | 9 | 0 | 18/20 |
| [dataset](src/dataset/README.md#spec-coverage) | 22 | 4 | 0 | 0 | 6/7 |
| [api](src/api/README.md#spec-coverage) | 13 | 2 | 1 | 0 | 5/9 |
| **all** | **214** | **53** | **38** | **6** | |
<!-- spec-summary:end -->

What the tables mark missing is the roadmap's raw material ([docs/ROADMAP.md](docs/ROADMAP.md)).

## Types

Every Arrow type either round-trips or is refused by `write_batch` with a message; nothing is written that
nanolance or Lance cannot read back.

| Arrow type | |
|---|---|
| `int8`..`int64`, `uint8`..`uint64`, `float16`, `float`, `double`, `bool` | round-trips |
| `utf8`, `large_utf8`, `binary`, `large_binary`, `fixed_size_binary(N)` | round-trips |
| `timestamp` (with or without an IANA time zone), `date32/64`, `time32/64`, `duration`, `decimal128/256` | round-trips |
| `struct` (any depth, null structs included), `list`, `large_list`, `map`, `fixed_size_list` | round-trips, with null and empty lists and null items at any level |
| `null` | round-trips (Lance's all-null page) |
| `lance.blob.v2` | external references written; inline, packed, dedicated and external blobs read |
| Arrow `dictionary` | refused: cast to the value type (low-cardinality strings are dictionary-encoded on disk anyway) |
| `timestamp` with a UTC-offset zone (`+05:30`) | refused: Lance supports IANA names only |
| a null element inside a valid `fixed_size_list` row | refused |

Nulls are stored in every column kind.

## Things to know

- **The schema is fixed by the first batch** of a write session.
- **Compression:** structural encodings (bitpacking, constant pages, run-length, dictionary) are on by default
  and cost nothing to read; `compression = true` adds zstd for high-cardinality strings and byte-stream split
  + zstd for floats. Per-column measurements: [AGENTS.md §3](AGENTS.md#3-enabling-the-compression-that-was-measured).
- **Small files of references:** model them as plain `uri` / `position` / `size` columns rather than the packed
  `lance.blob.v2` descriptor (about 3 against 41 bytes a row): [AGENTS.md §4](AGENTS.md#4-data-model-how-to-actually-get-small-files-important).
- **Memory while writing:** the writer holds rows until a commit; `max_pending_bytes` makes `write_batch` commit a
  fragment whenever the buffered data reaches the budget (peak about 3-4x the budget).
- **Threads:** reads, takes and writes use the machine's cores (`nanolance.set_threads(n)` /
  `nano_lance_set_threads(n)` / `NANOLANCE_THREADS`); files do not depend on the thread count.
- **nanolance-only features**, which pylance cannot read: the external-URI dictionary
  (`nano_lance_writer_set_blob_uri_dictionary`), and full-text indexes built with the multi-script settings
  (pylance refuses those by name rather than searching them wrongly).
- **S3 credentials** are resolved as the AWS tools do (environment, profiles, `credential_process`, ECS/EKS,
  IMDSv2); SSO and assume-role profiles are not: export their credentials first
  ([docs/s3_reader_notes.md](docs/s3_reader_notes.md)).

## Testing

| What | Where |
|---|---|
| C++ unit tests, by layer | `tests/<layer>/`, `ctest -L layer:<layer>` |
| cross-layer and smoke tests | `tests/integration/`, `tests/*.sh` |
| Python, including every comparison with pylance | `bindings/python/tests` (`pytest`) |
| pylance's own test suite against nanolance | [docs/PYLANCE_COMPAT.md](docs/PYLANCE_COMPAT.md) |
| lance-c's tests against `liblance_c` | [docs/LANCE_C_COMPAT.md](docs/LANCE_C_COMPAT.md) |
| Lance's Rust encoding tests, read back by nanolance | [docs/RUST_SUITE.md](docs/RUST_SUITE.md) |
| datasets published on the Hugging Face Hub, read by both | [docs/REAL_DATASETS.md](docs/REAL_DATASETS.md) |
| fuzzing and sanitizers | `tests/fuzz/`, `.github/workflows/memory-safety.yml` |

## Benchmarks

**Live results: [yoavbendor.github.io/nanolance/bench](https://yoavbendor.github.io/nanolance/bench/)**, updated by
every benchmark run. The table below is the latest run's summary.

<!-- bench-summary:start -->

Latest run: 2026-10-25 14:05 UTC on `claude/nanolance-optimization-review-h3rxi7` (`9c1c79c`), [full results](https://yoavbendor.github.io/nanolance/bench/).

| Benchmark | Reference | nanolance speed | Faster in |
|---|---|---:|---:|
| Read, C++ library, all cores | pylance | 2.99× | 27 of 27 |
| Read, C++ library, 1 core | pylance | 3.87× | 27 of 27 |
| Write, C++ library, all cores | pylance | 2.07× | 26 of 27 |
| Write, C++ library, 1 core | pylance | 1.97× | 26 of 27 |
| Read, Python binding, all cores | pylance | 2.86× | 27 of 27 |
| Write, Python binding, all cores | pylance | 1.61× | 27 of 27 |
| Scalar indexes, build | Rust Lance | 2.37× | 5 of 5 |
| Scalar indexes, query | Rust Lance | 3.66× | 14 of 14 |
| Vector indexes, build | Rust Lance | 1.13× | 1 of 3 |
| Vector indexes, search | Rust Lance | 3.20× | 12 of 12 |
| Full-text index, build | Rust Lance | 1.36× | 1 of 1 |
| Full-text index, search | Rust Lance | 11.76× | 7 of 7 |

<!-- bench-summary:end -->

![nanolance benchmark summary](https://yoavbendor.github.io/nanolance/bench/summary.svg)

Speed is the reference's median time over nanolance's: above 1× nanolance is faster. The matrix covers 27 Arrow
data types written and read by the C++ library and the Python binding, every read checked against its source;
the index suites build and query scalar, vector and full-text indexes against the `lance` crate with no Python in
the process, and the answers must agree before a time is reported. [docs/PERFORMANCE_GATE.md](docs/PERFORMANCE_GATE.md)
is the gate that fails a CI run when nanolance loses ground; memory, file sizes, program sizes and the full-scale
runs are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md). Those full-scale numbers are dated 2026-10-17, from a local
machine and an earlier commit; the table above is the latest CI run, and its full record is on the results page.

## Related projects

- [nanoarrow2parquet](https://github.com/yoavbendor/nanoarrow2parquet): the same Arrow batches to Parquet.
- [nanom](https://github.com/yoavbendor/nanom): the parser-combinator library behind `examples/pcapng2lance`, a
  streaming pcapng to Lance converter, and its DPAR / LLDP tools.
- [nanos3reader](https://github.com/yoavbendor/nanos3reader): the S3 client.

## Version and license

Version **0.2.0** (`nanolance::library_version()`, `include/nanolance/version.hpp`).
[Apache-2.0](LICENSE); see [NOTICE](NOTICE) and [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md). nanolance is
an independent implementation of the Lance format: it does not link the Rust `lance` crate; its protobuf message
definitions are derived from Lance's Apache-2.0 `.proto` schemas.
