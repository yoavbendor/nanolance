# Third-party licenses

nanolance is licensed under Apache-2.0 (see [LICENSE](LICENSE)). It builds on the
following third-party software. All are permissive and Apache-2.0-compatible.

## Format definitions (derived source)

| Component | How it's used | License | Project |
|---|---|---|---|
| **Lance** file format `.proto` schemas | nanolance's protobuf message types (manifest, data file, column/page metadata, encodings) are derived from Lance's `.proto` files, pinned at `v7.0.0-rc.1`. Generated into `generated/lance_minimal.pb.*`; see `third_party/lance_proto/`. nanolance does **not** link the Rust `lance` crate. | Apache-2.0 | https://github.com/lance-format/lance |

## Build-time dependencies (FetchContent, not redistributed)

| Component | Used for | License | Project |
|---|---|---|---|
| Apache Arrow nanoarrow | Arrow `ArrowSchema`/`ArrowArray` + Arrow IPC reading | Apache-2.0 | https://github.com/apache/arrow-nanoarrow |
| Zstandard (zstd) | column compression | BSD-3-Clause | https://github.com/facebook/zstd |
| CLI11 | command-line parsing in the tools | BSD-3-Clause | https://github.com/CLIUtils/CLI11 |

The `pcapng2lance` example additionally depends on the sister **nanotins** stack
(soatins + nanotins), which is Apache-2.0; see its own `NOTICE` / `THIRD-PARTY-LICENSES.md`.

License compatibility: Apache-2.0 and BSD-3-Clause are permissive and mutually
compatible. (zstd is also offered under GPLv2; this project uses it under the
BSD-3-Clause option.)
