# Third-party licenses

nanolance is licensed under Apache-2.0 (see [LICENSE](LICENSE)). It builds on the
following third-party software. All are permissive and Apache-2.0-compatible.

## Format definitions (derived source)

| Component | How it's used | License | Project |
|---|---|---|---|
| **Lance** file format `.proto` schemas | nanolance's protobuf message types (manifest, data file, column/page metadata, encodings) are derived from Lance's `.proto` files, pinned at `v7.0.0-rc.1`. Generated into `generated/lance_minimal.pb.*`; see `third_party/lance_proto/`. nanolance does **not** link the Rust `lance` crate. | Apache-2.0 | https://github.com/lance-format/lance |

## Ported code and embedded data (redistributed)

| Component | How it's used | License | Project |
|---|---|---|---|
| **jieba-rs** 0.10.0 and **jieba-macros** 0.10.0 | `src/fts_jieba.cpp` ports jieba-rs's segmentation (dictionary, DAG and route, HMM Viterbi, search mode) to C++, and `src/fts_jieba_hmm_data.inc` embeds jieba-macros's built-in HMM model (`hmm.model`, zstd-compressed by `tools/fts_jieba_hmm.py`). Built unless `-DNANOLANCE_WITH_JIEBA=OFF` (off for wasm). Dictionaries are not embedded: a jieba model is read from the language model directory. | MIT | https://github.com/messense/jieba-rs (crates at commit 7818f91a) |
| **Lindera** 3.0.7 (lindera, lindera-dictionary) | `src/fts_lindera.cpp` ports Lindera's tokenizer config, segmenter, lattice and Viterbi (normal and decompose modes), user-dictionary building and the mapping character filter to C++, and reads Lindera 3 dictionary files; `bindings/python/tests/lindera_dict.py` writes them for the tests. No dictionary is embedded: a lindera model is read from the language model directory. | MIT | https://github.com/lindera/lindera (tag v3.0.7) |
| **daachorse** 2.1.0 (format) | `src/fts_lindera.cpp` walks the serialized double-array automaton Lindera's dict.da holds, and the test writer builds one, from the layout in daachorse's source. | MIT OR Apache-2.0 (used under Apache-2.0) | https://github.com/daac-tools/daachorse |

jieba-rs's license (from its repository at the commit the 0.10.0 crates were published from):

```
MIT License

Copyright (c) 2018 - 2019 messense
Copyright (c) 2019 Paul Meng

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Lindera's license (from its repository at tag v3.0.7; the crates carry no license file):

```
MIT License

Copyright (c) 2019 by the project authors.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

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
