# nanolance Python bindings

Fast, dependency-light Python bindings for [nanolance](https://github.com/yoavbendor/nanolance) — zero-copy Arrow ↔ Lance read/write with no Rust core.

Data moves through the [Arrow PyCapsule interface](https://arrow.apache.org/docs/format/CDataInterface/PyCapsuleInterface.html) — no buffer copies on the Python ↔ native boundary. Output files are validated against pyarrow, pandas, polars, and the official Lance format SDK (`pylance`).

## Naming: we do not shadow `import lance`

The official Lance columnar format SDK is published on PyPI as **`pylance`** but imported as **`import lance`**. This package is **`import nanolance`** — a separate, complementary fast C++ path.

```python
import lance       # official SDK (pip install pylance)
import nanolance   # fast C++ nanolance bindings (this package)
```

## Install

Wheels are built for manylinux (x86_64) and macOS (arm64, x86_64) on CPython 3.9–3.13 and published
to PyPI on a version tag; each one links nanoarrow and zstd statically, so there is nothing to
install alongside it. Until the first tag, install from source:

```bash
cd bindings/python
python -m pip install -e ".[test]"
pytest
```

That needs CMake, a C++20 toolchain, and network access (the build fetches nanoarrow and zstd).

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

### Coming from `pyarrow.parquet`

The API is deliberately `pyarrow.parquet`-shaped, so the migration is a one-line diff:

| `pyarrow.parquet` | nanolance |
|---|---|
| `pq.write_table(table, "out.parquet")` | `nanolance.write_table(table, "out.lance")` |
| `pq.write_table(table, p, compression="zstd")` | `nanolance.write_table(table, p, compression=True)` |
| `pq.read_table("in.parquet")` | `pa.table(nanolance.read_table("in.lance"))` |
| `pq.read_table(p, columns=["ts", "level"])` | `nanolance.read_table(p, columns=["ts", "level"])` |
| `pq.ParquetFile(p).iter_batches()` | `nanolance.open_stream(p)` |
| `pq.ParquetWriter(p, schema)` + `write_table` | `nanolance.LanceWriter(p)` + `write_batch` |
| `pq.ParquetFile(p).schema_arrow` | `nanolance.read_schema(p)` |
| `pq.ParquetFile(p).metadata.num_rows` | `nanolance.count_rows(p)` |
| `pq.read_table(p).slice(off, n)` | `nanolance.read_table(p, offset=off, length=n)` (skips whole fragments) |
| row group | **fragment** (`max_rows_per_fragment=`) |

Differences worth knowing before you switch:

- `read_table` returns an Arrow-exportable **handle**, not a `pa.Table`. Wrap it: `pa.table(...)`.
  That is what keeps the read zero-copy; nothing forces pyarrow on a caller who does not want it.
- A projected read comes back in the **dataset's** column order, not the order you listed.
- `compression=True` is a boolean, not a codec name: the codec per column type is chosen for you
  (zstd on strings, byte-stream-split + zstd on floats). Structural encodings -- bitpacking,
  constant, RLE, dictionary -- are on by default and independent of it.
- Types nanolance will not round-trip are **refused at write time**, by name, rather than written in
  a shape it cannot read back: `list`, `large_utf8`/`large_binary`, Arrow `dictionary` columns and
  Arrow's `null` type.

Already have parquet files? Convert one and compare:

```console
$ nanolance convert events.parquet events.lance --compress
200,000 rows  events.parquet -> events.lance
  parquet :    2.0 MiB
  lance   :  530.3 KiB   (3.92x smaller)
```

(Also `python -m nanolance convert ...`. Conversion reads and writes a batch at a time, so a file
larger than memory converts fine; `nanolance inspect events.lance` prints rows, size and schema.)

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
reader = pa.RecordBatchReader.from_stream(nanolance.open_stream("big.lance"))
for batch in reader:
    ...  # process one fragment's rows at a time
```

`open_stream` decodes a fragment per pull, so peak memory is one fragment rather than the whole
dataset. Measured on a 61 MiB / 16-fragment dataset: **5.6 MiB peak against 61.4 MiB**, and **3.2 ms
to the first batch against 66.5 ms**. `read_table` also exports one batch per fragment, but it has
already decoded them all by the time it returns.

## API

| Function | Description |
|----------|-------------|
| `write_table(table, path, **opts)` | Write an Arrow table to a Lance dataset (one fragment per input batch) |
| `LanceWriter(path, *, max_rows_per_fragment=0, **opts)` | Streaming context-manager writer: `write_batch(batch)`, `flush()`, `close()` |
| `read_table(path, columns=None, *, offset=0, length=None)` | Read eagerly; returns an Arrow-exportable handle (an Arrow C stream, one batch per fragment). `columns` projects |
| `open_stream(path, columns=None, *, offset=0, length=None)` | Same, but decoded one fragment per pull: bounded peak memory, larger-than-memory datasets, fast first batch. Single-shot |
| `read_schema(path)` | The dataset's Arrow schema, from the manifest alone -- no data file opened |
| `count_rows(path)` | The dataset's row count, from the manifest's fragments. O(fragments), not O(rows) |
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

### Read (`read_table`, `open_stream`)

```python
nanolance.read_table("events.lance")                          # every column
nanolance.read_table("events.lance", columns=["ts", "level"])  # just these two
```

`columns=` is a real projection, shaped like `pyarrow.parquet.read_table`: the columns you do not
ask for are **skipped during decode** rather than decoded and thrown away, which is what makes it
worth using — column materialization is where a read spends its time. Naming a column that does not
exist is an error, not a silently empty result. One difference from pyarrow: the result comes back in
the dataset's column order, not the order you listed.

For a dataset larger than memory, or when you want the first rows quickly, use `open_stream` instead:

```python
reader = pa.RecordBatchReader.from_stream(nanolance.open_stream("events.lance", columns=["ts"]))
for batch in reader:
    ...
```

It takes the same `columns=`. Two differences from `read_table`, both inherent to streaming and
stated here because they are easy to trip over:

- **Errors surface late.** A corrupt or unreadable dataset raises from the first pull, not from
  `open_stream(...)`, and pyarrow reports it as `OSError` rather than `RuntimeError`. `read_table`
  decodes up front, so it still raises at call time.
- **The handle is single-shot.** Exporting it twice raises; call `open_stream` again for a second
  pass.

That is why it is a separate function rather than a flag on `read_table` -- `read_table` keeps its
eager, raise-at-call-time contract.

### Peeking without reading

```python
nanolance.count_rows("events.lance")            # -> 200000
pa.schema(nanolance.read_schema("events.lance"))  # -> the Arrow schema
```

Both answer from the manifest and never open a data file, so they cost the same on a 200k-row
dataset as on a 200M-row one (0.18 ms against 16 ms for the full read, measured on the same 200k-row
dataset). The schema handle is re-exportable, unlike the single-shot stream handle.

### Reading a row range

```python
nanolance.read_table("events.lance", offset=2_000_000, length=1_000)
nanolance.open_stream("events.lance", offset=2_000_000, length=1_000)  # same, streamed
```

Spelled like `pyarrow.Table.slice`, and the rows you get back are exactly the rows you asked for.
What it *saves* is more specific, and worth understanding before you rely on it: **fragments the
range does not touch are never opened**, so the I/O avoided is proportional to the fragments
skipped, not to the rows dropped. A range inside a single fragment still decodes that whole
fragment, and a one-fragment dataset saves nothing -- write with `max_rows_per_fragment` if you
intend to read ranges.

Measured on a 61 MiB dataset in 16 fragments of 250k rows:

| | time | vs full read |
|---|---|---|
| full read (4M rows) | 72.1 ms | — |
| 1,000 rows mid-dataset | 2.5 ms | **29x faster** |
| 500,000 rows (2 fragments) | 7.3 ms | 9.9x |
| half the dataset (8 fragments) | 35.1 ms | 2.1x |

Peak memory for the 1,000-row range was 1.8 MiB, 0.03x the dataset.

`length=None` reads to the end, and a length running past the end is clamped. An `offset` past the
end is an **error**, not an empty table -- it nearly always means the caller's arithmetic is wrong.
Negative values are refused rather than given `[-10:]` semantics, which would need the row count and
would otherwise read the wrong rows silently.

**Scope.** nanolance reads back everything it writes, and now every non-nested column type stock
Lance writes:

| Readable | Notes |
|----------|-------|
| Datasets written by `nanolance.write_table` | Round-trip tested |
| Fixed-width, temporal and decimal columns from stock Lance | int/uint 8–64, float, bool, timestamp/date/time, decimal128/256, `fixed_size_binary` |
| `utf8` / `large_utf8` / `binary` from stock Lance | Including FSST-compressed |
| Nullable columns from stock Lance | Bit-packed *and* run-length-encoded definition levels |
| Categorical columns from stock Lance | Including their LZ4-compressed dictionary |
| Struct columns written by nanolance, append fragments | Yes (reads latest manifest) |

| Not supported | Use instead |
|----------------|-------------|
| `list` and `struct` columns written by **stock Lance** | `pylance` — these are unmapped at the manifest layer, and refused by name |
| `blob_uri_dictionary` blob columns | nanolance reader only (not stock `lance`) |

**Interop rule of thumb:** everything nanolance writes with default Lance-compatible options is
readable by **both** `nanolance.read_table` and `lance.dataset()` (verified against `pylance` 12.0.0).
The reverse now holds for every non-nested column shape; anything still missing is **refused by
name**, never misread.

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
