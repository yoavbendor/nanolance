---
title: nanolance — a C++ Arrow ↔ Lance writer/reader with a hardened read path
---

# nanolance

**A standalone C++ Arrow ↔ Lance v2.2 writer and reader — no Rust `lance` core.** Its headline
feature: rows keep large payloads **external** (referenced by `uri` + `position` + `size`, never
copied), so a Lance table of large-payload rows costs single-digit bytes per row regardless of
payload size, while the bytes themselves live once in the source and are fetched on demand (local
file or `s3://`). Everything nanolance writes is readable by stock `lance` (verified against `lance`
7.0.0) unless a feature is explicitly marked nanolance-only.

[Get started](getting-started.md){ .md-button .md-button--primary }
[Memory safety & Rust reviewers](SAFETY.md){ .md-button .md-button--primary }
[AI agent integration](agents.md){ .md-button }
[Benchmarks](bench.md){ .md-button }
[GitHub](https://github.com/yoavbendor/nanolance){ .md-button }

## Rust roots, hardened read path

nanolance's format is Lance — a Rust-native columnar format. If you're evaluating a C++ implementation
against reaching for the Rust crate directly, the question is always the same one: *does the C++ side
give up Rust's memory-safety guarantees to get there?* nanolance's answer is that the **reader** — the
only code that touches untrusted bytes (on-disk manifests, data-file footers, encoded column buffers,
and external blob fetches by `(uri, position, size)`) — is hardened the same way, and proven, not
asserted:

- **Overflow-checked arithmetic everywhere a disk-derived size feeds an allocation or bounds compare**
  (`checked_add`/`checked_mul`/`range_in_bounds`) — no `offset + size` or `rows * width` can wrap into
  a too-small allocation that a later `memcpy` then overflows.
- **No `reinterpret_cast` of disk bytes to a typed pointer.** Multi-byte integers are loaded via
  byte-assembly + `std::bit_cast` (`load_le<T>`), avoiding alignment/aliasing UB.
- **A tunable allocation budget (`ReadLimits`)** bounds what a hostile file can make the reader
  allocate: declared zstd uncompressed size (cross-checked against `ZSTD_getFrameContentSize`),
  row/column/manifest-element counts.
- **A path jail** confines every manifest-derived data-file path under the dataset directory, and the
  external `file://` blob fetch rejects `..` traversal — a hostile `../../etc/passwd` path can't escape.
- **Mid-read failures release everything already built** — no leaked `ArrowArray`/`ArrowSchema` on a
  partial read.
- **Continuous ASan + UBSan + LSan CI** and a **libFuzzer harness** over the full decode chain
  (manifest → footer → column decode), seeded and run on every push.

All of this validates **once per page/header, not once per value** — a declared size or offset table
is checked against the real buffer in a prologue, then the tight per-value `memcpy`/materialization
loop that follows is check-free. The safety cost is O(pages), not O(values): unmeasurable next to the
bytes moved.

**Coming from Rust?** [Memory safety & Rust reviewers](SAFETY.md) states the threat model, the
defenses and where they live in the source, how they're proven (fuzzing, sanitizer CI, negative-corpus
tests), and a reviewer checklist you can walk through yourself.

### Safety costs nothing — proven, not asserted

A self-produced pipeline (round-tripping your own writer output) can opt into `trusted_input=true` to
skip the untrusted-input **budget** checks — every bounds check still runs unconditionally either way.
Measuring it proves the point: the checks it skips were already free.

```
mode           |    best ms |     avg ms
---------------+------------+-----------
default        |    92.0356 |    98.0309
trusted_input  |     90.682 |    99.1971
```

**trusted/default best-ms ratio: 0.985x** — within noise (1,000,000 rows, best of 9 reads each;
reproduce with `bench/read_parity_bench.sh`). See [Benchmarks](bench.md) for the full write/read
comparison against Parquet and Rust `lance`.

## Why nanolance

- **External-payload rows.** Model a large payload as `uri`/`position`/`size` columns; the writer
  never copies the bytes in, so a packet/blob table stays a few bytes per row.
- **No Rust core.** Depends only on **nanoarrow + zstd** — small dependency footprint, easy to embed
  in an existing C++ build.
- **Interop-first.** Reads and writes are verified against stock `lance` 7.0.0; you're not locked into
  a nanolance-only ecosystem.
- **Python bindings.** Zero-copy via the Arrow PyCapsule interface — `import nanolance` alongside (not
  instead of) `import lance`.
- **Built for automated integration.** A dedicated [AI agent integration guide](agents.md) covers the
  API lifecycle, the measured per-type compression table, and the data-model recipe for small files —
  written so an agent can get it right on the first try.

## 60-second tour (C write API)

```c
#include "nanolance/nano_lance_writer.h"
#include <nanoarrow/nanoarrow.h>

NanoLanceWriter w = {0};
nano_lance_writer_init(&w, "out.lance", /*compression_level=*/3);
nano_lance_writer_set_compression(&w, true);         // BEFORE the first write_batch
nano_lance_write_batch(&w, &arrow_array, &arrow_schema);  // schema locks after batch #1
nano_lance_writer_commit(&w, /*is_append=*/false);   // false = create, true = append a fragment
nano_lance_writer_close(&w);
// non-zero return -> nano_lance_writer_last_error(&w)
```

Reading back — default (fully-checked) or `trusted_input=true` for a known-good, self-produced file:

```cpp
#include "nanolance/lance_table_reader.hpp"

ArrowSchema schema{};
std::vector<ArrowArray> batches;
std::string error;
nano_lance::lance_table_read_dataset("out.lance", schema, batches, error);
```

```python
import pyarrow as pa
import nanolance

table = pa.table({"id": [1, 2, 3], "name": ["alpha", "beta", "gamma"]})
nanolance.write_table(table, "out.lance", compression=True)
assert pa.table(nanolance.read_table("out.lance")).equals(table)
```

Read on: **[Getting started](getting-started.md)** · **[Memory safety](SAFETY.md)** ·
**[AI agent integration](agents.md)** · **[Benchmarks](bench.md)**.
