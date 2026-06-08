# nanolance

Standalone C++ library for writing **Lance v2.2** datasets (minimal protobuf scaffold, no Rust `lance` core). Public headers remain under `include/nano_lance_writer/` for compatibility.

## Layout

- **Libraries (CMake targets):** `nanolance_proto`, `nanolance_reader`, `nanolance` (aliases: `nano_lance_proto_minimal`, `nano_lance_reader`, `nano_lance_writer`)
- **Tool:** `arrowipc2lance` — Arrow IPC stream → Lance dataset (`--version` prints nanolance version)

## Standalone build

Requires CMake 3.22+, C++20, network for first-time FetchContent (nanoarrow, zstd, CLI11).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# Full smoke needs Python with pyarrow for several arrowipc2lance scripts; otherwise use a venv:
#   cmake -S . -B build -DNANO_LANCE_WRITER_PYTHON=/path/to/venv/bin/python
ctest --test-dir build -L smoke --output-on-failure
```

Optional S3 external blobs (you must supply a CMake target and include dir, e.g. from a parent project):

```bash
cmake -S . -B build -DNANOLANCE_ENABLE_S3=ON \
  -DNANOLANCE_S3_TARGET=your_s3_helper_target \
  -DNANOLANCE_S3_INCLUDE_DIR=/path/to/headers
```

## Embedded in streamingtestapps

The parent project sets `NANOLANCE_SOURCE_DIR` and calls `add_subdirectory` with `NANOLANCE_ENABLE_S3`, `NANOLANCE_S3_TARGET`, and `NANOLANCE_BUILD_TESTS` so it reuses nanoarrow, zstd, and `stream_helper_s3` from the main tree.

## Version

Version **0.2.0** — macros are generated into `build/include/nanolance/version.h`. Runtime string: `nanolance::library_version()` (declared in `include/nanolance/version.hpp`). `arrowipc2lance --version` includes this string.

Standalone FetchContent pins **nanoarrow** to commit `ffe61d3cd2d02da9e60cfc405cd6c50c512f64b9` for reproducibility (align with a known-good streamingtestapps fetch).
