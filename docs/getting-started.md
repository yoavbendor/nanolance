# Getting started

nanolance depends only on **nanoarrow + zstd** (fetched automatically via CMake `FetchContent` on
first configure) — no Rust `lance` core, no packet-parsing machinery.

## Standalone build

Requires CMake 3.22+, C++20, and network access for the first-time `FetchContent` fetch.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -L smoke --output-on-failure
```

!!! note "Cloning"
    Building the `pcapng2lance` example needs the `nanotins` git submodule — clone with
    `git clone --recursive`, or run `git submodule update --init --recursive`. Building nanolance
    itself (library, tools, tests) needs no submodule.

Optional S3 external blobs:

```bash
cmake -S . -B build -DNANOLANCE_ENABLE_S3=ON
```

## Quick start (Python)

```python
import pyarrow as pa
import nanolance

table = pa.table({"id": [1, 2, 3], "name": ["alpha", "beta", "gamma"]})
nanolance.write_table(table, "out.lance", compression=True)
assert pa.table(nanolance.read_table("out.lance")).equals(table)
```

Install for development: `pip install -e "bindings/python[test]"` then `pytest` in that directory. No
hard `pyarrow` runtime dependency (uses the Arrow PyCapsule interface); does not shadow `import lance`
(the official `pylance` SDK).

## Quick start (C write API)

```c
#include "nanolance/nano_lance_writer.h"
#include <nanoarrow/nanoarrow.h>

NanoLanceWriter w = {0};
nano_lance_writer_init(&w, "out.lance", /*compression_level=*/3);
nano_lance_writer_set_compression(&w, true);         // Lance-compatible compression (off by default)
nano_lance_write_batch(&w, &arrow_array, &arrow_schema);  // repeatable; schema locks after batch #1
nano_lance_writer_commit(&w, /*is_append=*/false);   // false = create, true = append a fragment
nano_lance_writer_close(&w);
// On any non-zero return: nano_lance_writer_last_error(&w).
```

Read back with `nano_lance::lance_table_read_dataset(...)` (C++, [`lance_table_reader.hpp`](https://github.com/yoavbendor/nanolance/blob/main/include/nanolance/lance_table_reader.hpp))
or `nano_lance_table_read_dataset` / `nano_lance_table_read_dataset_ex` (C ABI,
[`nano_lance_reader.h`](https://github.com/yoavbendor/nanolance/blob/main/include/nanolance/nano_lance_reader.h)) — the
`_ex` variant takes an explicit `trusted_input` flag (see [Memory safety](SAFETY.md)).

## Gotchas & lifecycle

- **Schema locks after the first `write_batch`** — every batch in a session shares it.
- **Call all `set_*` options before the first `write_batch`** (compression, nullability, URI
  dictionary).
- **`bool` row fields are not supported** (Arrow's 1-bit storage vs. the byte-wide writer path) — use
  `uint8` for flags.
- **Compression is off by default.** One switch (`set_compression`) picks the right Lance encoding per
  column.
- **To get small files, model external refs as plain `uri`/`position`/`size` columns**, not the packed
  `lance.blob.v2` descriptor (~41 B/row vs ~3.4 B/row).
- **The reader is hardened against untrusted files** — see [Memory safety](SAFETY.md) for the threat
  model and reviewer checklist.

## Library layout

- **nanolance** (namespace `nano_lance`, include prefix `nanolance/`): the Lance writer/reader. CMake
  targets: `nanolance_proto`, `nanolance_reader`, `nanolance` (writing); link `nanolance_reader` alone
  if you only fetch external blobs.
- **Tool:** `arrowipc2lance` — Arrow IPC stream → Lance dataset; `nlance2table` — Lance dataset →
  CSV/NDJSON text (for validation).

For the full integration guide — API lifecycle, the measured per-column compression table, the
data-model recipe for small files, and interop verification — see
[AI agent integration](agents.md) (also useful for humans).
