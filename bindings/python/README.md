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

## API

| Function | Description |
|----------|-------------|
| `write_table(table, path, **opts)` | Write an Arrow table to a Lance dataset |
| `read_table(path)` | Arrow-exportable Lance reader handle |
| `WriteOptions` | `compression`, `compression_level`, `append`, etc. |

## Benchmarks

```bash
pytest tests/test_benchmarks.py -m bench
```

See also nanolance `tools/bench.py` / `bench/linux-ci-results.md` for C++ baselines.

## Tests

- **Parity**: compares output against pyarrow / pandas / polars readers
- **pylance interop**: files written by nanolance read back via `lance.dataset()`
- **Full cycle**: Python → native writer → standard reader → Python
- **Memory safety**: repeated write/read under RSS caps
- **Benchmarks**: optional `pytest -m bench`

## Build layout

`scikit-build-core` + `nanobind`, links the in-tree `nanolance` CMake target directly (no FetchContent pin).
