# Blob-fetch benchmark — nanolance vs pylance on external `lance.blob.v2` references

Compares two ways of materializing external blob payloads from a Lance dataset of `lance.blob.v2`
references:

- **nanolance** — `nano_lance_fetch_external_blob(uri, position, size)`, the nimble AWS-C++-SDK path on
  S3 (plain file IO on `file://`). Driver: `nlance_blobfetch`.
- **pylance** — `Dataset.take_blobs(...).read()`, the Rust core's on-demand object-store reads. Driver:
  `bench/fetch_pylance.py`.

Both fetch the **same** dataset and MD5 every payload; the harness asserts the digests agree per index
(so any timing comparison is apples-to-apples), then times them with a warm/cold-aware schedule.

## The dataset: realistic tiling

`nlance_blobgen` models the real workload — N external objects (e.g. one pcapng per hour), each sliced
front-to-back into consecutive **random packet-sized chunks** (`len = 64 + rand()%9000`, last chunk
clamped to EOF), one Lance row per chunk with a running global `packet_id`. So a single Lance file spans
many hours of externally-stored payload, and every `(position, size)` is valid by construction.

```
nlance_blobgen --out ds.lance --uris s3://b/h00,s3://b/h01 --sizes 12000000,11500000 [--seed N]
```

## Running

```
bench/blob_fetch_bench.sh --uris u1,u2,...  [--sizes s1,s2,...] [--rounds 3] \
    [--gen build/nlance_blobgen] [--nlfetch build/nlance_blobfetch] [--pyfetch bench/fetch_pylance.py]
```

The script: (1) reads each object's size — `stat` for `file://`, `aws s3api head-object` for `s3://`
(or pass `--sizes` to skip), (2) generates the dataset, (3) runs **pylance ×3 cold**, (4) checks
MD5(nanolance) == MD5(pylance) for every blob, (5) runs `--rounds` **rotated** rounds (alternating which
implementation goes first, to cancel ordering/warm-up bias), reporting total fetch ms per implementation.

### Local smoke (`file://`)

```bash
head -c 3000000 /dev/urandom > /tmp/h00.bin
head -c 5000000 /dev/urandom > /tmp/h01.bin
bench/blob_fetch_bench.sh --uris file:///tmp/h00.bin,file:///tmp/h01.bin
```

This proves correctness (MD5 agreement) and that the harness runs. **It is not the meaningful perf test:**
on local files each `nano_lance_fetch_external_blob` opens the file per blob, while the Rust object-store
reuses a handle — so local `file://` flatters pylance. The real comparison is **remote S3**, where the
AWS-C++-SDK path is latency- and connection-bound.

### Real S3

Build nanolance with S3 support, then point the same harness at `s3://` URIs (credentials/region via the
usual `AWS_*` env vars). To test in CI without a real bucket, run an S3 mock — **LocalStack** or **MinIO**
— and export `AWS_ENDPOINT_URL`, `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_REGION`,
`AWS_ALLOW_HTTP=true`, and path-style addressing so both pylance's object-store and nanolance's SDK resolve
the same endpoint.

## Output format

Each fetcher prints `index <TAB> md5 <TAB> fetch_nanos` (only the fetch call is timed). The script sums the
nanos per run for the totals and diffs the md5 columns for correctness.
