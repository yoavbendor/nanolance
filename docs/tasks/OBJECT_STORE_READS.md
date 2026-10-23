# Task: read datasets on S3-compatible object stores (`s3://`), as pylance does

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP "Index plan
for AI users", Wave 2, item 5. Three sessions: **O1** (an S3 client in nanos3reader), **O2** (nanolance
reads `s3://` datasets), **O3** (CI against MinIO, lance-c). Do only the phase you are given. Writes to
object stores are out of scope (Wave 3, item 8): refuse them by name.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'`
windows of at most ~120 lines (`/home/user/nanos3reader/src/s3_reader.cpp` is ~1.3k lines;
`bindings/python/nanolance/lance/dataset.py` 3.5k+). Cut output: `| tail -15` for pytest (add
`-o addopts=`), `ninja 2>&1 | grep -E "error|warning:" | head`.

## 1. Goal

`lance.dataset("s3://bucket/path.lance", storage_options={...})` opens a dataset in an object store;
scans, filters, takes, index searches and `versions()` work on it. nanolance reads external blobs from
`s3://` today (nanos3reader's `S3MinStreamFactory`, an `std::istream` per object), but cannot open a
dataset there: its read layer (`include/nanolance/byte_source.hpp`) takes registered factories per URI
scheme (`register_source_factory`, `register_list_factory`), and nothing registers `s3`.

## 2. Facts (measured here, 2026-10-19 -- do not re-derive)

- A local S3 endpoint: moto's server, installed in
  `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/motoenv` (moto 5.2.4). Start it
  with `setsid .../motoenv/bin/moto_server -p 5055 > moto.log 2>&1 &` (record the PID; kill by PID), then
  create a bucket: `curl -X PUT http://127.0.0.1:5055/nl-test`. Requests to it must bypass this sandbox's
  proxy: set `NO_PROXY=127.0.0.1,localhost` and `no_proxy=...` for every process (pylance, curl, tests).
  There is no Docker daemon here; CI has Docker and runs MinIO (see nanos3reader's `.github/workflows/ci.yml`,
  job `minio-integration`).
- pylance writes and reads such a dataset with
  `storage_options={"access_key_id": "testing", "secret_access_key": "testing", "region": "us-east-1",
  "endpoint": "http://127.0.0.1:5055", "allow_http": "true"}` (measured: 1,000 rows in 3 fragments,
  `filter="id = 777"` returns the row). Lance's keys also include `session_token`; the `aws_`-prefixed
  forms (`aws_access_key_id`, `aws_secret_access_key`, `aws_session_token`, `aws_region`, `aws_endpoint`)
  are accepted by pylance as well. Without storage_options pylance uses the AWS environment chain.
- moto's ListObjectsV2 (`GET /bucket?list-type=2&prefix=...`) returns XML: `<ListBucketResult>` with
  `<IsTruncated>`, `<Contents><Key>..</Key><Size>..</Size></Contents>`, `<CommonPrefixes><Prefix>` when a
  `delimiter=/` is given, and `<NextContinuationToken>` when truncated (send it back as
  `continuation-token`). Keys of the measured dataset: `ds.lance/_transactions/0-<uuid>.txn`,
  `ds.lance/_versions/18446744073709551614.manifest`, `ds.lance/data/<name>.lance`.
- nanos3reader today (`include/nanos3reader/s3_reader.h`): SigV4 helpers; `S3MinStreamFactory::open(uri,
  read_ahead_bytes)` -> a seekable `std::istream` of range GETs; credentials and endpoint only from the
  environment chain (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN`, `AWS_REGION`,
  `AWS_ENDPOINT_URL` -> path-style addressing); a disk LRU block cache. No HEAD, no listing, no per-call
  options. nanolance pins it by commit in `CMakeLists.txt` (`FetchContent_Declare(nanos3reader ... GIT_TAG
  2155864...)`, about line 286).

## 3. Phases

### O1 -- nanos3reader: an S3 client (work in `/home/user/nanos3reader`, branch `claude/nanolance-optimization-review-h3rxi7`)

Add, beside `S3MinStreamFactory` (keep it unchanged):
```cpp
struct ClientOptions {           // empty fields fall back to the environment chain, as today
    std::string access_key_id, secret_access_key, session_token, region, endpoint;
    bool allow_http = false;     // an http:// endpoint is refused unless set
};
struct ObjectInfo { std::string key; std::uint64_t size = 0; };
class S3Client {
public:
    explicit S3Client(ClientOptions options = {});
    bool head(const std::string& uri, std::uint64_t& size, std::string& error);
    // Exactly n bytes at offset (one range GET; a short body is an error).
    bool read_range(const std::string& uri, std::uint64_t offset, std::uint64_t n, std::uint8_t* dst,
                    std::string& error);
    // ListObjectsV2 under prefix_uri ("s3://bucket/prefix/"), every page. With delimiter "/", the
    // immediate children only, sub-"directories" in common_prefixes.
    bool list(const std::string& prefix_uri, const std::string& delimiter, std::vector<ObjectInfo>& objects,
              std::vector<std::string>& common_prefixes, std::string& error);
};
```
Thread-safe: nanolance calls one client from many threads at once (one curl easy handle per thread,
reused, so connections are kept alive). Retry 5xx and 503 SlowDown / throttling up to 3 times with
backoff; never retry 4xx. Errors name the URI and the HTTP status. Bump the version to 0.3.0.
Tests: extend `tests/minio_integration_test.cpp` (CI runs it against MinIO) to cover head, read_range at
the start, middle and end, a short read, a missing object (404), list with and without a delimiter, and a
listing longer than one page (create 1,100 objects, or pass `max-keys`); run it locally against moto.
Python bindings (`tests/python`): expose head / read_range / list if the existing binding style makes it
a few lines; otherwise leave Python alone.

### O2 -- nanolance reads `s3://` datasets

- The nanos3reader pin is bumped already (0.4.0, `S3Client`; commit 7158595, in `CMakeLists.txt`).
- S3 is OFF in every build today: `NANOLANCE_ENABLE_S3` defaults OFF (`CMakeLists.txt` line 68) and the
  Python binding forces it OFF (`bindings/python/CMakeLists.txt` about line 71). Reviewer's decision: the
  binding builds with S3 when libcurl is found (`find_package(CURL)`), without it otherwise; a build
  without S3 refuses `s3://` with `nanolance was built without S3 support`. For your local builds
  configure the main build with `-DNANOLANCE_ENABLE_S3=ON` (libcurl headers are installed here). Wheels
  (cibuildwheel, `bindings/python/pyproject.toml` `[tool.cibuildwheel.linux]`, manylinux_2_28) get
  libcurl in O3.
- `src/io/s3_byte_source.cpp` (built when `NANOLANCE_S3_NANOS3READER_ACTIVE`): an `io::ByteSource` over
  `S3Client` (size from `head` at open; `read_at` = `read_range`; `small_read_block` 1 MiB and
  `footer_read_size` 64 KiB, so small reads become one request) and a lister over `list`, registered for
  `s3`. One `S3Client` per distinct `ClientOptions`.
- Storage options from Python: `lance.dataset(uri, storage_options=...)` (and `LanceDataset` methods that
  re-open by URI) pass the options for that URI's bucket to C++ before the open. Map pylance's keys above.
- Every read path works: `to_table`, filters, `take`, `count_rows`, `versions()`, `checkout_version`,
  scalar / vector / full-text index searches on indexes pylance built there. Writes to `s3://`
  (`write_dataset`, `delete`, `create_index`, ...) raise `NotSupportedError` naming object-store writes.
- Tests: `bindings/python/tests/test_object_store.py`, skipped unless `NANOLANCE_S3_TEST_ENDPOINT` is set
  (the one allowed skip reason: the endpoint is an optional dependency). Each test writes a dataset with
  pylance to the endpoint, then compares nanolance's results with pylance's: whole table, projection,
  filter, take, a BTREE and an IVF_FLAT search, a dataset with deletions, two versions. Also count the
  requests (`nanolance._work_stats()` bytes read and largest read) for a filtered scan and assert a bound.

### O3 -- CI and lance-c

- `.github/workflows/bindings-python.yml` (or a new small workflow): start MinIO with Docker as
  nanos3reader's CI does, set `NANOLANCE_S3_TEST_ENDPOINT` and the AWS variables, run
  `test_object_store.py`; upload MinIO's log on failure. It must fail when the tests fail; it must not skip.
- lance-c: `lance_dataset_open` with storage options (see `compat/lance-c/include/lance/lance.h` for the
  options parameter) reads `s3://` datasets; a C test against the same endpoint.

## 4. Rules

`AGENTS.md` sections 5, 6a (threads) and 8 (reads go through `io::ByteSource`). No skipped or weakened
tests except the endpoint skip above; no `except Exception` around failures. C++ without warnings (CI
uses `-Werror`); portable (no `__int128`, no unguarded builtins); no new dependency beyond libcurl and
the crypto backend nanos3reader already uses. Local reads must not change or slow down. Do not commit,
push, change branches, or run `--update`. Scratch files only in
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/os/`. Network timings in this
sandbox are not representative: compare request counts and bytes, not milliseconds. Run each full suite
once, at the end, one at a time; never two at once; no sleep loops (wait for moto with a bounded curl
retry); kill by PID.

## 5. Stop and ask (question first in the report)

- moto and pylance disagree on something MinIO or S3 would do differently (an XML field, an error code);
- a read path opens files with something other than `io::ByteSource` (do not add a second path);
- the client needs a credential source or signing case the environment chain does not cover.

## 6. Done and report

O1: the nanos3reader tests' tails (unit, and the integration test against moto), the files changed, the
API as built. O2: `cd build && ctest -j8 | tail -3`; `NANOLANCE_S3_TEST_ENDPOINT=... python3 -m pytest
-o addopts= -q bindings/python/tests/test_object_store.py | tail -2`; last, once: the full pytest. O3:
the workflow file and a local dry run of its commands. Report under 40 lines: questions first, then files,
tails, decisions.

**O1 outcome (2026-10-19):** nanos3reader 0.4.0 (`S3Client`; commits 3e83de0, 7158595); its CI runs the
integration tests against moto, since `minio/minio` can no longer be pulled from Docker Hub.

**O2 outcome (2026-10-19):** done in one Haiku session; reviewed. `s3://` datasets open through an
`io::ByteSource` / lister over `S3Client` (`src/io/s3_byte_source.cpp`); storage_options from Python per
bucket; whole table, projection, filters, take, count, BTREE, IVF_FLAT and INVERTED searches,
deletions and versions match pylance on the same dataset (moto). The binding builds with S3 when
libcurl is found. Review fix: writes to a URI were refused only in `nanolance.lance`; the low-level
`nanolance.write_table` / `LanceWriter` went through `pathlib.Path`, which collapses `s3://b/x` into a
local `s3:/b/x` and wrote there. Every writer now refuses a URI (Python before `Path`, C++ at writer
open and at `publish_manifest`); `tests/test_object_store_writes.py`. Open: moto does not check SigV4
signatures, so no server here or in CI verifies them (nanos3reader's SigV4 known-answer tests do);
options are per bucket and process-wide.

**O3 outcome (2026-10-19):** done by the reviewer (small enough not to delegate). CI
(`bindings-python.yml`) installs libcurl, starts moto (MinIO's image cannot be pulled), builds lance-c's
S3 reader with `-Werror` and runs `test_object_store.py` / `test_object_store_writes.py`; the step fails on
any skip and uploads moto's log on failure. lance-c: `lance_dataset_open(uri, storage_opts, version)`
opens `s3://` datasets with Lance's option keys (one key mapping, `io::object_store_options_from`, shared
with the Python binding); writes through lance-c still refuse URIs. `tests/lance_c_s3_read.cpp`, driven
by `test_lance_c_reads_s3_dataset` (version, count and take after a delete, against pylance). Not done:
wheels still build without S3 (bundling libcurl and OpenSSL into manylinux wheels is its own decision);
a wheel refuses `s3://` by name.
