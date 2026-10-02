# Third-party licenses

nanolance is licensed under Apache-2.0 (see [LICENSE](LICENSE)). It builds on the
following third-party software. All are permissive and Apache-2.0-compatible.

## Format definitions (derived source)

| Component | How it's used | License | Project |
|---|---|---|---|
| **Lance** file format `.proto` schemas | nanolance's protobuf message types (manifest, data file, column/page metadata, encodings) are derived from Lance's `.proto` files, pinned at `v7.0.0-rc.1`. Generated into `generated/lance_minimal.pb.*`; see `third_party/lance_proto/`. nanolance does **not** link the Rust `lance` crate. | Apache-2.0 | https://github.com/lance-format/lance |

## lance-c headers (vendored, unmodified)

`compat/lance-c/include/lance/lance.h` and `lance.hpp`: from
[lance-format/lance-c](https://github.com/lance-format/lance-c) at the commit in
`compat/lance-c/UPSTREAM`. Apache License 2.0, Copyright The Lance Authors
(`compat/lance-c/LICENSE`). They declare the API nanolance's `liblance_c` implements.

## Test suites run in CI (fetched, not redistributed)

pylance's `python/python/tests` (tools/pylance_suite.py) and lance-c's `tests/cpp`
(tools/lance_c_suite.py) are fetched with git at pinned revisions into `.deps/` and run
against nanolance; neither is copied into this repository. Both Apache License 2.0,
Copyright The Lance Authors.

## Build-time dependencies (FetchContent, not redistributed)

| Component | Used for | License | Project |
|---|---|---|---|
| Apache Arrow nanoarrow | Arrow `ArrowSchema`/`ArrowArray` + Arrow IPC reading | Apache-2.0 | https://github.com/apache/arrow-nanoarrow |
| Zstandard (zstd) | column compression | BSD-3-Clause | https://github.com/facebook/zstd |
| CLI11 | command-line parsing in the tools | BSD-3-Clause | https://github.com/CLIUtils/CLI11 |

The `pcapng2lance` example additionally depends on the sister **nanotins** stack
(soatins + nanotins), which is Apache-2.0; see its own `NOTICE` / `THIRD-PARTY-LICENSES.md`.

## Optional system libraries (built-in S3 reader)

Linked from the system (via `find_package`, not fetched or redistributed) only when the built-in S3 reader
is enabled with `-DNANOLANCE_ENABLE_S3=ON` (off by default). Both are dynamically linked.

| Component | Used for | License | Project |
|---|---|---|---|
| libcurl | HTTP(S) range GETs to S3 | curl license (MIT/X11-style) | https://curl.se |
| OpenSSL (libcrypto) | SHA-256 / HMAC for AWS SigV4 signing | Apache-2.0 (OpenSSL 3.x) | https://www.openssl.org |

Note: OpenSSL 3.x is Apache-2.0. OpenSSL ≤ 1.1.1 used the legacy OpenSSL/SSLeay dual license (with an
advertising clause); that is still permissive and Apache-2.0-compatible (it is incompatible only with GPLv2,
which does not apply here). libcurl's TLS backend is its own transitive dependency; all common backends
(OpenSSL, GnuTLS) are compatible with Apache-2.0 under dynamic linking.

License compatibility: Apache-2.0 and BSD-3-Clause are permissive and mutually
compatible. (zstd is also offered under GPLv2; this project uses it under the
BSD-3-Clause option.)
