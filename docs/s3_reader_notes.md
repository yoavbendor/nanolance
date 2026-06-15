# The built-in S3 reader: credentials, performance, and small static builds

Engineering notes for the in-tree S3 range reader (`src/s3_min_reader.cpp`,
`include/nanolance/s3_min_reader.h`). Written so we can page this back in weeks from now without
re-deriving it. Covers: what the reader is, how it resolves credentials, why it's ~40× faster than
pylance on the blob-fetch benchmark, the swappable SigV4 crypto backend, what a small static build
(mbedTLS) actually costs, and what it would take to promote the reader into a standalone library
(working name **`nanos3reader`**).

---

## 1. What it is

`nanolance` only needs to **read byte ranges** out of external blobs (see
`src/nano_lance_external_blob.cpp`). The built-in reader provides exactly that and nothing more:

- A `S3MinStreamFactory` that opens `s3://bucket/key` as a **seekable, read-ahead-buffered
  `std::istream`** backed by libcurl range `GET`s + AWS SigV4 signing.
- Dependencies: **libcurl** (HTTP/HTTPS) + a tiny bit of crypto for SigV4 (SHA-256/HMAC). No AWS SDK.
- Scope: **read-only, `GET` only.** No `PUT`/multipart/`LIST`. That's deliberate — it keeps the
  surface tiny and is all the engine needs.

It's a drop-in for the old `AwsSdkStreamFactory` seam: `nano_lance_external_blob.cpp` talks to it
through a typedef (`NanoLanceS3Factory`) over a stable `open()/error()` → `std::unique_ptr<std::istream>`
contract, and the CMake already supports built-in / external / no-S3 providers.

---

## 2. Credential resolution

The reader resolves credentials the way the AWS tools do, trying in order:

1. **Environment** — `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` / `AWS_SESSION_TOKEN`.
2. **Shared profile files** — `~/.aws/credentials` and `~/.aws/config`, honoring `AWS_PROFILE`
   (`AWS_SHARED_CREDENTIALS_FILE` / `AWS_CONFIG_FILE` override the paths). This includes a profile's
   **`credential_process`** helper: the reader runs the command and parses its JSON
   (`AccessKeyId` / `SecretAccessKey` / `SessionToken` / `Expiration` — note `SessionToken`, *not*
   the `Token` key the metadata endpoints use).
3. **ECS/EKS container credentials** (`AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` / `_FULL_URI`).
4. **EC2 instance role** via IMDSv2.

Temporary credentials (session tokens) are honored and refreshed shortly before they expire. The
resolved result is cached process-wide, so a single run resolves once and reuses it across every
fetch — this matters for performance (see §3).

### Not resolved (by design)

- **AWS SSO** profiles and **assume-role** profiles. The AWS CLI resolves these; we don't. For those,
  run your normal AWS login and export the resolved creds to the environment first.

### Self-explaining failures

When no credentials are found, the error names **each source it tried and why**, e.g.:

```
missing AWS credentials — env: AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY not set;
  profile: profile 'default' uses AWS SSO, which the built-in reader does not resolve — run
  'aws sso login', then export AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY/AWS_SESSION_TOKEN to env;
  IMDS: no EC2 instance role (metadata endpoint unreachable or empty)
```

It specifically detects profiles that use SSO / `credential_process` / assume-role, a profile that
isn't found, and absent `~/.aws` files. This is the fastest way to diagnose "the `aws` CLI works but
nanolance says no credentials" — it's almost always SSO or `credential_process`.

### Context: `credential_process` matters more than you'd think

Apache `arrow-rs object_store` (the storage layer behind pylance / Lance) **does not support
`credential_process`** — the feature request was closed "not planned"
([apache/arrow-rs-object-store#47](https://github.com/apache/arrow-rs-object-store/issues/47)). So our
native `credential_process` support is a real differentiator for corporate setups that use it.

---

## 3. Performance: ~40× faster than pylance on blob-fetch

Benchmark: `bench/blob_fetch_bench.sh` builds a `lance.blob.v2` dataset of external references and
fetches every blob two ways — **nanolance** (`nano_lance_fetch_external_blob`) vs **pylance**
(`take_blobs().read()`, the Rust core) — then MD5-compares the bytes per index.

Real run (epgd068, 690 blobs across 2 S3 objects, warm mean):

| | total | per blob |
|---|---|---|
| **nanolance** | ~2.6–3.0 s | **~4 ms** |
| **pylance** `take_blobs` | ~119–121 s | **~180 ms** |

→ **~40–47×**, bytes verified identical.

### Why — and what we ruled out

Both drivers fetch the same 690 ranges **serially, one at a time** (`tools/nlance_blobfetch.cpp` and
`bench/fetch_pylance.py`), so it's a clean per-blob latency comparison. We chased the gap to ground:

- ❌ **Credentials.** Exported static env creds (`key=ASIA`, 760-char token) so both sides skip
  `credential_process` → pylance still ~119 s. Not it.
- ❌ **Warmup.** pylance per-blob distribution: `min 151 / p50 179 / p90 242 / max 876` ms. The 876 ms
  is **only idx 0** (dataset + first-object open).
- ❌ **Per-object setup.** Only the object-boundary blob (idx 481) showed a bump (~382 ms); everything
  else sits at the steady ~180 ms.
- ✅ **Per-blob read cost with no reuse.** ~97% of pylance's time is a uniform ~180 ms/blob that does
  **not** amortize. nanolance reuses **one keep-alive HTTPS connection per object** and pays ~4 ms/blob.

The ~4 ms/blob also tells us the RTT to the bucket is tiny (same-region/Direct-Connect), so pylance's
~180 ms is ~45× a single round-trip — i.e. per-blob overhead inside `take_blobs` (each `BlobFile`
opening its own reader/connection), not the network.

### Honest caveats (don't over-quote the 40×)

- It's specific to **many small reads from few objects**. For a few large reads it shrinks toward parity.
- `take_blobs` is the lazy random-access API; pylance's *bulk* path (a scan / `to_table` over the blob
  column, which coalesces) was **not** measured and would likely be much faster. The fair framing is
  "nanolance is ~40× faster than pylance's lazy `take_blobs` for this fetch-by-reference pattern."
- 2 objects, one host/region/session. Worth re-running with 10–20 objects.

### Reproduce / the env-creds trick

If your profile uses `credential_process` and your AWS CLI is too old for
`aws configure export-credentials` (added in v2.7.5), materialize creds straight from the helper:

```bash
PYBIN=/path/to/.venv/bin/python   # any python3 (stdlib json only; no botocore needed)
eval "$(eval "$(aws configure get credential_process)" | "$PYBIN" -c '
import sys, json
d = json.load(sys.stdin)
print("export AWS_ACCESS_KEY_ID="     + d["AccessKeyId"])
print("export AWS_SECRET_ACCESS_KEY=" + d["SecretAccessKey"])
print("export AWS_SESSION_TOKEN="     + d.get("SessionToken", ""))
')"
echo "key=${AWS_ACCESS_KEY_ID:0:4}  token_len=${#AWS_SESSION_TOKEN}"   # sanity check
```

### Known bug spotted along the way

`ds.take_blobs(col, indices=[0,1,2,5,...])` with a **sparse/non-contiguous** index list panics in the
Lance Rust core (`Invalid size`, `lance-encoding/.../bytepack.rs`). The full contiguous range works,
which is why the bench uses it. This is upstream Lance, orthogonal to nanolance — file a minimal repro
if it ever bites.

---

## 4. SigV4 crypto backend (`-DNANOLANCE_S3_CRYPTO`)

SigV4 needs only SHA-256 (+ HMAC). HMAC is the standard ipad/opad construction, so it's implemented
generically over a single swappable `sha256` primitive. Two backends, chosen at build time:

| `-DNANOLANCE_S3_CRYPTO=` | SHA-256 from | Links |
|---|---|---|
| `openssl` (default) | OpenSSL `libcrypto` | `OpenSSL::Crypto` |
| `bundled` | compact in-tree SHA-256 | **no crypto library** |

Both produce **byte-identical signatures** — verified by the SigV4 known-answer test against AWS's
published vectors (`nano_lance_s3_min_sigv4`), which passes on both backends.

Why bother: it decouples our code from `libcrypto`. Proof (object-level):

```
nm -uC s3_min_reader.o  (openssl)  ->  U SHA256        # depends on libcrypto
nm -uC s3_min_reader.o  (bundled)  ->  (no crypto syms) # links nothing
```

Cost: **+1.1 KB** of code (`.text` 83,429 → 84,568 bytes). This is the prerequisite for a small static
build — otherwise `libcrypto` (~4.5 MB static) gets pulled in just for our SHA-256.

> Note: in a *normal dynamic* build, `libcrypto` still arrives via **libcurl's TLS backend** (system
> curl is OpenSSL-backed). `bundled` only pays off when curl itself uses a non-OpenSSL TLS — see §5.

---

## 5. Small static builds with mbedTLS (parked, but measured)

**Status: parked. Not wired into the build — this section is the recipe + the numbers.**

The ~4.5 MB of static OpenSSL `libcrypto` is the bloat, and it enters via **both** our SigV4 and
**libcurl's TLS**. To shed it you need both `bundled` crypto **and** a libcurl built against a small
TLS backend. We measured it end-to-end with a standalone `s3cat` (open a URI, read first bytes), fully
static for TLS+curl+reader (only glibc dynamic):

| Build | Stripped | Unstripped |
|---|---|---|
| **mbedTLS curl + bundled SHA-256** (zero OpenSSL) | **2.16 MB** | 2.68 MB |
| **OpenSSL curl + OpenSSL SHA-256** | **6.72 MB** | 7.84 MB |

→ **mbedTLS is ~4.6 MB smaller: 3.1× / ~68% off.** `ldd` confirms no dynamic curl/tls/crypto deps in
either. Component sizes: trimmed `libcurl.a` ≈ 1.0 MB; mbedTLS static stack ≈ 1.3 MB
(`libmbedcrypto.a` 856 KB + `libmbedtls.a` 318 KB + `libmbedx509.a` 162 KB) vs OpenSSL `libcrypto.a`
≈ 4.5 MB.

### TLS-backend reality

curl's well-supported **small** TLS backends are **mbedTLS** and **wolfSSL**. curl's BearSSL support
has been spotty/removed — don't count on it. mbedTLS (Apache-2.0) is the pragmatic small choice.

### Gotcha: curl ↔ mbedTLS version match

Ubuntu ships **mbedTLS 2.28 LTS**. **curl 8.8.0** calls `mbedtls_ssl_get_ciphersuite_id_from_ssl` (a
mbedTLS 3.x function) unguarded and fails to build against 2.28 (fixed by a version guard in 8.9). Use
**curl 8.7.1** (predates that call) or mbedTLS 3.x. We used curl 8.7.1 + mbedTLS 2.28.8.

### Recipe (what we ran)

```bash
apt-get install -y libmbedtls-dev zlib1g-dev
# curl.se is blocked here; fetch from GitHub:
curl -fsSL -o curl.tar.xz \
  https://github.com/curl/curl/releases/download/curl-8_7_1/curl-8.7.1.tar.xz
tar xf curl.tar.xz && cd curl-8.7.1

# HTTP/HTTPS only, everything else off:
./configure --disable-shared --enable-static --with-mbedtls=/usr --without-openssl \
  --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet \
  --disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher \
  --disable-mqtt --disable-smb --disable-ftp --disable-file --disable-ntlm \
  --disable-unix-sockets --disable-manual --disable-libcurl-option \
  --without-libpsl --without-nghttp2 --without-libidn2 --without-brotli \
  --without-zstd --without-librtmp --without-libssh2 --without-zlib
make -C lib -j"$(nproc)"          # -> lib/.libs/libcurl.a  (~1 MB)

# Link the standalone example with bundled crypto + the mbedTLS curl, zero OpenSSL:
M=/usr/lib/x86_64-linux-gnu
g++ -std=gnu++20 -O2 -I nanolance/include -DNANOLANCE_S3_CRYPTO_BUNDLED=1 \
  nanolance/src/s3_min_reader.cpp s3cat.cpp \
  -static-libstdc++ -static-libgcc \
  curl-8.7.1/lib/.libs/libcurl.a $M/libmbedtls.a $M/libmbedx509.a $M/libmbedcrypto.a \
  -lpthread -ldl -o s3cat_mbedtls
strip -s s3cat_mbedtls   # ~2.16 MB
```

---

## 6. Promoting it to a standalone library — `nanos3reader`

**The name fits** the `nano*` family (nanolance, nanotins, nanoarrow): **`nanos3reader`** (all lowercase —
the canonical name for repo, CMake target, and namespace) says exactly what it is: a read-only S3 range
reader.

### Why it has appeal

The pitch: *"Read byte-ranges from S3 (and MinIO/R2/any S3-compatible) as a seekable `std::istream`,
with SigV4 + the full AWS credential chain — in ~1k LOC and just libcurl + a TLS lib, no AWS SDK."* The
AWS SDK for C++ is the thing people flee (100s of MB, huge dep tree, slow builds). A focused, permissive,
static-friendly **reader** with the credential chain (incl. `credential_process`, which `arrow-rs`
lacks) is a real sweet spot for data engines, edge/embedded, and CLIs. Scope it honestly as a *reader*,
not an SDK.

### What it takes (most of it already exists)

Already have: SigV4 vector tests, a MinIO integration test, the external-blob test, a clean header, and
a CMake seam. Remaining:

- Own repo + CMake exporting `nanos3reader::nanos3reader`, install rules, `find_package` config +
  pkg-config, semver tags. (GitLab: `.gitlab-ci.yml` with a `minio` service for the integration test.)
- Rename namespace `nanolance::` → `nanos3reader::`; nanolance keeps working via the external-target
  hook + a 2-line typedef shim.
- README (quickstart, credential chain, MinIO/`AWS_ENDPOINT_URL`, build options, **limitations**), API
  reference, and the `s3cat` example (doubles as a smoke test).
- Optional, high-leverage: a thin **C API** (`s3_open/read/close`) so Python/Rust/Go can FFI to it.

Rough effort: ~2–4 focused days to a credible v0.1.

### Dependency strategy (the real decision)

**Make it a build option; don't hard-vendor.** Deciding factor: **who owns CVE updates.**

- **Default = `find_package(CURL)` + `find_package(OpenSSL)`** (dynamic, system-managed). Smallest
  source; the distro patches curl/TLS CVEs. This is what most C++ libs do.
- **Opt-in `-DNANOS3READER_BUNDLED_DEPS=ON`** = `FetchContent`/`ExternalProject` a **pinned** curl +
  mbedTLS built statically, plus the `bundled` SHA-256 → the ~2.2 MB self-contained artifact from §5.
  For users who want a single static binary (containers/distroless/edge) **and accept owning the patch
  treadmill** — that's the tradeoff: a static vendored networking stack means *you* rebuild and
  re-release for every curl/TLS CVE.

Ship both as documented presets; CI builds both.

### Does splitting hurt nanolance? No

The seam already exists: `nano_lance_external_blob.cpp` goes through the `NanoLanceS3Factory` typedef,
and CMake already supports an external S3 target (`NANOLANCE_S3_TARGET` / `NANOLANCE_S3_INCLUDE_DIR`) and
a no-S3 mode. nanolance can consume `nanos3reader` as an external target with **zero hot-path change**.
Costs to manage: keep the `open()/error()` + `istream` contract stable, pin a version (submodule or
package), and keep the in-tree copy as a fallback during migration.

### Licenses (all permissive — no copyleft, no GPL friction)

- Our code: **Apache-2.0**.
- **libcurl**: curl/MIT-style — static-link fine; ship the short license + copyright.
- **OpenSSL 3.x**: Apache-2.0 (compatible). Avoid pinning ≤1.1.1 (old OpenSSL/SSLeay BSD-with-advertising
  clause).
- **mbedTLS**: Apache-2.0. **zlib** (if ever enabled): zlib license.

Static linking is legally clean; the entire compliance burden is shipping a `THIRD-PARTY-LICENSES` /
`NOTICE` file with the curl + TLS attributions. The classic "OpenSSL + GPL" headache does not apply
because nothing here is GPL.

---

## 7. Quick reference

| Knob | Effect |
|---|---|
| `AWS_PROFILE` / `AWS_DEFAULT_PROFILE` | profile to read from `~/.aws/*` |
| `AWS_REGION` / `AWS_DEFAULT_REGION` | region for virtual-hosted addressing (else profile, else `us-east-1`) |
| `AWS_ENDPOINT_URL` | custom endpoint (MinIO etc.); switches to path-style addressing |
| `AWS_MAX_ATTEMPTS` | tries per range GET (default 3; full-jitter backoff on transient failures) |
| `-DNANOLANCE_S3_CRYPTO=openssl\|bundled` | SigV4 SHA-256 backend (default `openssl`) |
| `-DNANOLANCE_ENABLE_S3=ON` | enable the S3 provider |
| `NANOLANCE_S3_TARGET` / `_INCLUDE_DIR` | use an external S3 reader instead of the built-in one |

Source of truth: `src/s3_min_reader.cpp`, `include/nanolance/s3_min_reader.h`,
`src/nano_lance_external_blob.cpp`, `bench/blob_fetch_bench.sh`, `bench/fetch_pylance.py`.
