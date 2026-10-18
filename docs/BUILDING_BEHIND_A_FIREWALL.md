# Building behind a firewall (a GitLab mirror instead of GitHub)

nanolance's CMake build clones a few pinned dependencies with git. By default they come from GitHub;
one setting points all of them at an internal mirror, and nothing else in the build needs the
internet. Checked 2026-10-18: with every `github.com` URL made to fail, a fresh configure, build,
`ctest` (64 / 64) and Python wheel build succeeded from a mirror, and without the setting the
configure failed at the first clone.

## 1. What to mirror

Mirror each repository **with its full history and tags** (GitLab "pull mirroring", or
`git clone --mirror` + `git push --mirror`): the build checks out pinned commits and tags, and a
shallow mirror may not have them.

| GitHub repository | pinned at | needed for |
|---|---|---|
| `apache/arrow-nanoarrow` | commit `ffe61d3cd2d02da9e60cfc405cd6c50c512f64b9` | always (C++ and Python) |
| `facebook/zstd` | tag `v1.5.6` | always, unless zstd is installed (`find_package(zstd)`) |
| `yoavbendor/nanom` | commit `ffb03d35c3ce16d8ee5fd1702a6921989061383a` | always |
| `CLIUtils/CLI11` | tag `v2.4.2` | the command-line tools (`NANOLANCE_BUILD_TOOLS`, on by default) |
| `yoavbendor/nanos3reader` | commit `21558644771b37b6fefa440ee42ff7e8281c4748` | only `-DNANOLANCE_ENABLE_S3=ON` (it needs only system curl and OpenSSL) |
| `libfuse/libfuse` | tag `fuse-3.16.2` | only the fuselance example, and only when `libfuse3-dev` is not installed |
| `lance-format/lance` | tag `v<pylance version>` | only `tools/pylance_suite.py` (pylance's test suite as an oracle) |
| `lance-format/lance-c` | the commit in `tools/lance_c_suite.py` | only `tools/lance_c_suite.py` |

The pins live in the `FetchContent_Declare` calls of `CMakeLists.txt` and
`bindings/python/CMakeLists.txt` (see section 5 for checking them after an update).

Not needed: the two git submodules (`examples/pcapng2lance/extern/nanotins`,
`examples/pcapng2lance_nanom/extern/nanom`). They belong to two examples, and CMake skips those
examples when the submodules are absent. To build them anyway, see the `insteadOf` note in section 6.

## 2. Point the build at the mirror

If the mirror keeps GitHub's `owner/repo` paths under one group (for example
`https://gitlab.example.com/mirrors/apache/arrow-nanoarrow.git`), one setting does it:

```sh
export NANOLANCE_GIT_BASE=https://gitlab.example.com/mirrors
```

or `-DNANOLANCE_GIT_BASE=...` on the cmake command line. Each dependency is then
`$NANOLANCE_GIT_BASE/<owner>/<repo>.git`. The environment variable also reaches the Python build
and the two test-suite scripts.

If some repositories sit elsewhere, set those one by one (a `-D` option or an environment variable
of the same name); the rest still follow the base:

| variable | default |
|---|---|
| `NANOLANCE_GIT_URL_NANOARROW` | `$NANOLANCE_GIT_BASE/apache/arrow-nanoarrow.git` |
| `NANOLANCE_GIT_URL_ZSTD` | `$NANOLANCE_GIT_BASE/facebook/zstd.git` |
| `NANOLANCE_GIT_URL_NANOM` | `$NANOLANCE_GIT_BASE/yoavbendor/nanom.git` |
| `NANOLANCE_GIT_URL_CLI11` | `$NANOLANCE_GIT_BASE/CLIUtils/CLI11.git` |
| `NANOLANCE_GIT_URL_NANOS3READER` | `$NANOLANCE_GIT_BASE/yoavbendor/nanos3reader.git` |
| `NANOLANCE_GIT_URL_LIBFUSE` | `$NANOLANCE_GIT_BASE/libfuse/libfuse.git` |
| `NANOLANCE_GIT_URL_LANCE` | `$NANOLANCE_GIT_BASE/lance-format/lance.git` (pylance_suite.py) |
| `NANOLANCE_GIT_URL_LANCE_C` | `$NANOLANCE_GIT_BASE/lance-format/lance-c.git` (lance_c_suite.py) |

Example: `-DNANOLANCE_GIT_URL_ZSTD=https://gitlab.example.com/thirdparty/zstd.git`.

If git needs credentials for the mirror, give them to git as usual (a credential helper, or an
SSH base such as `NANOLANCE_GIT_BASE=git@gitlab.example.com:mirrors` — then the URLs become
`git@gitlab.example.com:mirrors/apache/arrow-nanoarrow.git`). If the server's certificate is
signed by a corporate CA: `git config --global http.sslCAInfo /path/to/corporate-ca.pem`.

## 3. Build and test (C++)

Needs CMake 3.22+, a C++20 compiler (GCC 11+ or Clang 14+), Ninja or Make, git, and Python 3 (some
tests generate fixtures with it).

```sh
export NANOLANCE_GIT_BASE=https://gitlab.example.com/mirrors
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -j4 --output-on-failure        # 64 tests
ctest --test-dir build -L smoke --output-on-failure    # the quick subset
```

The first configure clones the dependencies into `.deps-<generator>/` next to the source (it
survives `rm -rf build`). After that, no network is needed; to make sure the build never tries,
add `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` (it then uses the checkouts already in `.deps-*`).

To use a local checkout of one dependency instead of cloning it:
`-DFETCHCONTENT_SOURCE_DIR_NANOARROW=/path/to/arrow-nanoarrow` (likewise `_NANOM`, `_NANOS3READER`,
`__NANOLANCE_ZSTD`, `__NANOLANCE_CLI11` -- the dependency names upper-cased).

## 4. Python package

The Python build fetches nanoarrow and zstd again, through the same settings. pip itself needs a
package index for the build tools (`scikit-build-core`, `nanobind`) and, for the tests, `pytest`,
`pyarrow`, `pandas`, `polars`, `psutil` and `pylance`. Point pip at the corporate index:

```sh
export NANOLANCE_GIT_BASE=https://gitlab.example.com/mirrors
export PIP_INDEX_URL=https://artifactory.example.com/api/pypi/pypi/simple   # your PyPI mirror
pip install -e "bindings/python[test]"
cd bindings/python && pytest -q                       # ~3,400 tests, several minutes
```

A wheel to copy to other machines: `pip wheel ./bindings/python --no-deps -w dist/`.

The pylance-suite check (optional; clones `lance-format/lance` from the mirror):
`python3 tools/pylance_suite.py` (1,200 expected passes).

## 5. After you update nanolance

1. Update the mirrors first (GitLab pull mirrors do it on their schedule; or push the new
   commits yourself), then pull the new nanolance into your checkout.
2. See whether a dependency pin moved:

   ```sh
   git diff <old>..<new> -- CMakeLists.txt bindings/python/CMakeLists.txt cmake/ tools/*_suite.py | grep -E "GIT_TAG|GIT_REPOSITORY|^[-+]REPO|COMMIT"
   ```

   Nothing printed: the mirrors already have everything. A new commit or tag: make sure the mirror
   has it (`git ls-remote $NANOLANCE_GIT_BASE/apache/arrow-nanoarrow.git | grep <sha or tag>`; for a
   bare commit, a fresh configure is the real test).
3. Build and test as in sections 3 and 4. If you configure with `FETCHCONTENT_FULLY_DISCONNECTED=ON`,
   turn it off once after a pin moves (or delete `.deps-*`), so the new commit is fetched.

## 6. A git-only alternative

Without touching the build, git can rewrite GitHub URLs for everything on the machine, submodules
included, as long as the mirror keeps GitHub's `owner/repo` paths:

```sh
git config --global url."https://gitlab.example.com/mirrors/".insteadOf "https://github.com/"
git submodule update --init --recursive    # now from the mirror too
```

This is the simplest way to also get the example submodules. `NANOLANCE_GIT_BASE` is the better
default for builds: it lives in the build's own configuration, does not change git for every other
project on the machine, and can point single repositories elsewhere.
