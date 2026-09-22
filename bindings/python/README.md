# nanolance Python bindings

Fast, dependency-light Python bindings for [nanolance](https://github.com/yoavbendor/nanolance) — zero-copy Arrow ↔ Lance read/write with no Rust core.

Data moves through the [Arrow PyCapsule interface](https://arrow.apache.org/docs/format/CDataInterface/PyCapsuleInterface.html) — no buffer copies on the Python ↔ native boundary. Output files are validated against pyarrow, pandas, polars, and the official Lance format SDK (`pylance`).

## Naming: we do not shadow `import lance`

The official Lance columnar format SDK is published on PyPI as **`pylance`** but imported as **`import lance`**. This package is **`import nanolance`** — a separate, complementary fast C++ path.

```python
import lance       # official SDK (pip install pylance)
import nanolance   # fast C++ nanolance bindings (this package)
```

## Install (development)

```bash
cd bindings/python
python -m pip install -e ".[test]"
pytest
```

Optional interoperability tests also need the official SDK:

```bash
pip install pylance
```

## Quick start

```python
import pyarrow as pa
import nanolance

table = pa.table({
    "id": [1, 2, 3],
    "name": ["alpha", "beta", "gamma"],
    "value": [1.5, 2.5, 3.5],
})

nanolance.write_table(table, "out.lance", compression=True)
roundtrip = pa.table(nanolance.read_table("out.lance"))
assert roundtrip.equals(table)

# Official pylance reader (pip install pylance)
import lance
assert lance.dataset("out.lance").to_table().equals(table)
```

### Streaming / chunked writes

For datasets too large to hold as one Arrow table, use `LanceWriter` as a context manager and feed one
`RecordBatch` at a time — only a single chunk stays in memory. Set `max_rows_per_fragment` to flush a
fragment (and free the writer's buffer) periodically, bounding memory; the multi-fragment result reads
back as one table (a fragment is the Lance analogue of a Parquet row group).

```python
import pyarrow as pa
import nanolance

schema = pa.schema([("id", pa.int64()), ("value", pa.float64())])
with nanolance.LanceWriter("big.lance", max_rows_per_fragment=1_000_000) as writer:
    for chunk_id in range(10_000):
        batch = pa.record_batch(
            {
                "id": pa.array(range(chunk_id * 5_000, (chunk_id + 1) * 5_000)),
                "value": pa.array([float(i) for i in range(5_000)]),
            },
            schema=schema,
        )
        writer.write_batch(batch)
        del batch  # only one chunk in memory at a time
# close() (on __exit__) commits pending rows → a valid multi-fragment dataset

# Read back chunk by chunk (one batch per fragment) without materializing one big array:
reader = pa.RecordBatchReader.from_stream(nanolance.read_table("big.lance"))
for batch in reader:
    ...  # process one fragment's rows at a time
```

## API

| Function | Description |
|----------|-------------|
| `write_table(table, path, **opts)` | Write an Arrow table to a Lance dataset (one fragment per input batch) |
| `LanceWriter(path, *, max_rows_per_fragment=0, **opts)` | Streaming context-manager writer: `write_batch(batch)`, `flush()`, `close()` |
| `read_table(path)` | Arrow-exportable Lance reader handle (exports an Arrow C stream, one batch per fragment) |
| `WriteOptions` | `compression`, `compression_level`, `structural_encoding`, `append`, `blob_uri_dictionary`, `ignore_nullability` |

`write_table` and `LanceWriter.write_batch` accept any Arrow-exportable input (pyarrow `Table` / `RecordBatch`, polars via `to_arrow()`, etc.). `LanceWriter` takes the same encoding options as `write_table` plus `max_rows_per_fragment` (0 = single fragment committed on close).

---

## What is writable, readable, and not supported

### Write (`write_table`)

| Category | Supported | Notes |
|----------|-----------|-------|
| Integers | int8–int64, uint8–uint64 | FastLanes bitpacking, RLE, constant layout — **on by default** (`structural_encoding=True`) |
| Floats | float32, float64 | Written **uncompressed** (no byte-stream-split yet) |
| Strings / binary | utf8, binary, fixed_size_binary | dict-RLE, structural dictionary, constant layout **on by default**; zstd for high-cardinality columns only with `compression=True`. `large_utf8`/`large_binary` are refused — cast to the 32-bit offset type first. |
| Structs | nested struct columns | Same type coverage as [nanoarrow2parquet](https://github.com/yoavbendor/nanoarrow2parquet) |
| Nullability | values and schemas | Nulls are **stored**, via Lance's definition-level layer, and stock Lance reads them back — in fixed-width and in `utf8`/`binary` columns. A null **struct** is refused (that is not the same as a struct whose fields are null), as is a null in a `lance.blob.v2` column. `ignore_nullability` is a no-op kept for compatibility. |
| Append | `WriteOptions(append=True)` or `append=True` kwarg | Adds a fragment to an existing dataset |
| Multi-batch tables | yes | Streams all record batches from the input |

| Not supported on write | Workaround |
|------------------------|------------|
| **`bool` columns** | Use `uint8` (0/1) — Arrow's 1-bit layout vs nanolance's byte-wide path |
| **list, map, union** Arrow types | Flatten or serialize to string/binary |
| **`lance.blob.v2` + URI dictionary** (`blob_uri_dictionary=True`) | nanolance-only; stock `lance` cannot read those blob columns |

Two independent knobs control encoding: **structural** (lossless bitpacking / constant / RLE /
dictionary / dict-RLE) is **on by default** (`structural_encoding=True`) and needs no `compression`;
**zstd** for high-cardinality string/binary columns is opt-in via `compression=True` (off by default,
with `compression_level`). Set `structural_encoding=False` for plain "raw" pages. Both stay
stock-`lance`-readable. See the root [README compression section](../../README.md#compression-lance-compatible)
and [AGENTS.md §3](../../AGENTS.md#3-enabling-the-compression-that-was-measured) for per-type encodings.

### Read (`read_table`)

**Scope:** writer-parity reader — reliably reads datasets **written by nanolance** (this package or the C++ library). This is not a full Lance SDK; arbitrary `pylance`-written files that use encodings we never emit may fail.

| Readable | Notes |
|----------|-------|
| Datasets written by `nanolance.write_table` | Round-trip tested (parity tests) |
| Encodings we write | Flat fixed-width, zstd strings, bitpacking, RLE, constant, dict-RLE, structural dictionary |
| Struct columns | Yes |
| Append fragments | Yes (reads latest manifest) |

| Not guaranteed | Use instead |
|----------------|-------------|
| Arbitrary third-party Lance files | `pylance` (`import lance`) |
| `blob_uri_dictionary` blob columns | nanolance reader only (not stock `lance`) |
| On-disk encodings we do not implement | `pylance` |

**Interop rule of thumb:** everything nanolance writes with default Lance-compatible options is readable by **both** `nanolance.read_table` and `lance.dataset()` (verified vs `lance` 7.0.0). The reverse (pylance write → nanolance read) is best-effort only for simple schemas.

---

## Benchmarks vs pylance

Numbers below are from **Ubuntu CI** (clang Release, 200k rows, best-of-5 writes / best-of-7 reads). Source: [`bench/linux-ci-results.md`](../../bench/linux-ci-results.md) (auto-updated on every push). Reproduce C++ baselines with `python tools/bench.py` from the repo root.

The Python bindings call the same C++ writer/reader; expect only minor overhead vs the C++ numbers (PyCapsule import/export, no extra copies).

### Write time (ms, lower is better)

| Dataset profile | nanolance | pylance (`lance`) | Ratio (nl / pylance) |
|-----------------|-----------|-------------------|----------------------|
| **pcap_ref** — run-length URI + monotonic `position` + constant `size` (3 cols) | 24.2 core / 31.8 proc | 10.6 | **~2.3×** slower write |
| **wide_int** — uint64 ts + uint32 caplen + low-card uint8 (4 cols) | 10.6 core / 14.1 proc | 2.6 | **~4.1×** slower write |
| **high_card** — random uint64 id + unique hex strings (2 cols) | 166.5 core / 172.1 proc | 17.6 | **~9.5×** slower write |
| **scattered low-card strings** — `row-{i%500}` cycling (3 cols, `compression=True`) | ~2.9× pylance (post structural-dict) | 1.0× | **~2.9×** slower write, **~1.0×** on-disk size |

`write(core)` = encode + commit only. `write(proc)` for nanolance includes subprocess/IPC overhead in the C++ CLI bench; Python `write_table` is in-process (closer to core).

**Where nanolance wins on size:** pcap-style external-ref columns beat Parquet (~3.6 B/row vs ~4.2 B/row) and match pylance (~3.5 B/row). **Where pylance wins on write:** high-cardinality unique strings (no dictionary benefit); store app-level deltas for monotonic high-range integers if size matters.

### Read time (ms, lower is better)

| Dataset profile | nanolance native read | pylance reading a nanolance file | pylance reading its own file |
|-----------------|----------------------|----------------------------------|------------------------------|
| **pcap_ref** | 10.9 | 8.7 | 5.3 |
| **wide_int** | 12.5 | 11.2 | 3.5 |
| **high_card** | 15.5 | 11.5 | 5.9 |

Native nanolance read is **~2× slower than pylance** on these profiles (memory-bandwidth bound column materialization). Files nanolance writes are still read quickly by pylance (~1.2–1.5× native read in the table above).

Read throughput is the known remaining gap; write + on-disk size are the headline strengths for pcap-style and repetitive-column workloads.

### Python micro-benchmarks

```bash
pytest tests/test_benchmarks.py -m bench   # needs pylance
```

Soft gates compare `nanolance.write_table` vs `lance.write_dataset` on wide_int, pcap_ref+zstd, and scattered low-card strings. Informational only — not a CI regression gate.

---

## Tests

- **Parity**: pyarrow / pandas / polars round-trip
- **pylance interop**: nanolance write → `lance.dataset()` read
- **Structural dictionary**: cycling low-card strings size + round-trip
- **Full cycle**: Python → native → Python
- **Memory safety**: repeated write/read under RSS caps
- **Benchmarks**: optional `pytest -m bench`

## Build layout

`scikit-build-core` + `nanobind`, links the in-tree `nanolance` CMake target directly (no FetchContent pin).
