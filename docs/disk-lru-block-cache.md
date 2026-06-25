# Disk-resident LRU block cache — design & implementation plan

## Background

fuselance mounts a Lance blob.v2 table as a FUSE filesystem. Each file's
content is assembled from one or more `BlobSeg` entries, each of which is a
byte range inside a remote URI (NFS path or `s3://` object). Every FUSE
`read` call that misses the current 32 MiB fetch window triggers a full
remote round-trip:

- NFS warm cache: ~78 ms per fetch
- NFS cold / S3:  ~1 200 ms per fetch

For a typical file with 264 segments that means 264 × 78 ms ≈ 20 s for a
single sequential read. A second read of the same file repeats all 264
fetches — there is no persistence between FUSE sessions or between
`cat` invocations.

A **disk-resident LRU** stores each fetched 32 MiB aligned block as a flat
file on local disk. On a cache hit the block is served from local disk
(typically < 1 ms), bypassing the remote entirely.

---

## Where the implementation lives: `nanos3reader`

The fetch of remote bytes ultimately happens inside **nanos3reader**
(`yoavbendor/nanos3reader`), specifically in the `S3MinStreamFactory::open()`
and the seekable stream it returns. Placing the cache there means:

- Any consumer of nanos3reader (not just fuselance) gets the benefit.
- The cache is co-located with the S3 read-ahead logic (`kS3ReadAheadBytes =
  32 MiB`) — the same granularity becomes the natural block size.
- nanolance's `nano_lance_external_blob.cpp` stays unchanged; it just calls
  `nanos3reader` as today.

For `file://` URIs (served by a plain `std::ifstream`) the cache is
unnecessary — local disk reads are already fast and caching them to disk
would be disk-to-disk with no benefit. The cache **only applies to `s3://`
(and any future remote scheme nanos3reader adds)**.

---

## nanos3reader changes required

### 1. New configure function

```cpp
// Call once before the first open(). cache_dir is created if absent.
// max_blocks: LRU capacity 2–500; 0 = disabled (default).
// Returns true on success.
bool S3MinStreamFactory::configure_disk_cache(const std::string& cache_dir,
                                              int max_blocks);
```

Or, equivalently, as a free function / global if the factory is a singleton:

```cpp
void nanos3reader_configure_disk_cache(const char* cache_dir, int max_blocks);
```

### 2. Stats query

```cpp
void nanos3reader_disk_cache_stats(uint64_t* out_hits, uint64_t* out_misses);
```

### 3. Block file naming

Key: `(uri_hash, chunk_start)`.  
Filename: `<djb2_hex16(uri)>_<chunk_start_hex16>.blk`

```
/tmp/fuselance-cache-<pid>/
    a3f1b2c4d5e6f708_0000000000000000.blk   # uri=s3://bucket/foo, chunk 0
    a3f1b2c4d5e6f708_0000000002000000.blk   # uri=s3://bucket/foo, chunk 32 MiB
```

### 4. LRU discipline (mtime-based, O(N), N ≤ 500)

- **Hit**: `utime(path, nullptr)` to touch mtime → promotes to MRU.
- **Miss**: fetch full 32 MiB aligned block from S3, write to disk, then
  if file count > limit delete the file with the oldest mtime.

Full 32 MiB is always fetched on a miss (not capped to the requested slice)
so that any future read to any offset within the same chunk is a hit.

### 5. Integration point inside nanos3reader

In `S3MinStreamFactory::open()` (or the stream's `underflow()`), before
issuing a range GET:

```cpp
if (disk_cache_enabled()) {
    uint64_t chunk_start = (position / kReadAheadBytes) * kReadAheadBytes;
    std::string bpath    = blk_path(uri, chunk_start);
    if (block_exists_on_disk(bpath)) {
        promote(bpath);        // utime
        return stream_from_file(bpath, rel_off);
    }
    // miss: do the S3 GET, write bpath, evict if needed
}
```

### 6. Thread safety

A `std::mutex` guards the configure call (one-time init). Block I/O runs
on the caller's thread. fuselance uses single-threaded FUSE (`-s`), so
there is no concurrent disk access in that use case.

---

## nanolance changes required

### `nano_lance_external_blob.cpp`

Add a thin wrapper that forwards to nanos3reader's configure function when
S3 is enabled:

```cpp
// Only meaningful when built with NANOLANCE_ENABLE_S3.
int nano_lance_block_cache_configure(const char* cache_dir, int max_blocks,
                                     char* errmsg, size_t errmsg_cap);

void nano_lance_block_cache_stats(uint64_t* out_hits, uint64_t* out_misses);
```

For `file://`-only builds these are no-ops that return `NANO_LANCE_READER_OK`.

Declare both in `include/nanolance/nano_lance_reader.h`.

---

## fuselance changes required

### New CLI flag

```
--local-blocks-lru <N>
```

- N = 0 (or flag omitted): no disk cache (default, pure streaming)
- N = 2–500: enable disk LRU with N block slots (each ≈ 32 MiB on disk)

### Wiring

In `main()`, after CLI parsing, before `fuse_main`:

```cpp
if (g_lru_n > 0) {
    std::string cache_dir = "/tmp/fuselance-cache-" + std::to_string(::getpid());
    char cerr[256];
    if (nano_lance_block_cache_configure(cache_dir.c_str(), g_lru_n,
                                         cerr, sizeof(cerr)) != NANO_LANCE_READER_OK) {
        LOG_ERR("block cache init failed: %s", cerr);
        return 1;
    }
    g_lru_cache_dir = cache_dir;   // global, used in fl_destroy for cleanup
    LOG_INFO("disk LRU block cache: %d blocks (≤ %d MiB) at %s",
             g_lru_n, g_lru_n * 32, cache_dir.c_str());
}
```

### Cleanup in `fl_destroy`

```cpp
if (!g_lru_cache_dir.empty())
    std::filesystem::remove_all(g_lru_cache_dir);
```

### `perf_dump()` addition (when `--local-blocks-lru` is active)

```
  lru hits/misses: <hits> / <misses>   (via nano_lance_block_cache_stats)
```

### Usage string update

```
  --local-blocks-lru <N>    disk LRU block cache: N slots × 32 MiB (0=off, default)
                            range 2–500; stored in /tmp/fuselance-cache-<pid>/
```

---

## Implementation order

1. Add disk LRU to **nanos3reader** (`yoavbendor/nanos3reader`)
2. Add `nano_lance_block_cache_configure` / `_stats` wrapper to **nanolance**
   (`nano_lance_external_blob.cpp` + `nano_lance_reader.h`)
3. Bump the nanos3reader pin in **nanolance** `CMakeLists.txt` to the new tag
4. Add `--local-blocks-lru` to **fuselance** (`fuselance_main.cpp`)

---

## Verification

```bash
# Build with S3 enabled:
cmake -S . -B build -DNANOLANCE_ENABLE_S3=ON && cmake --build build -t fuselance

# First read (all misses — populates cache):
./build/examples/fuselance/fuselance my_table.lance \
    --filename-col src --local-blocks-lru 100 --perf &
time cat /tmp/fuse_my_table/somefile | wc -c

# Second read (all hits — served from disk):
time cat /tmp/fuse_my_table/somefile | wc -c

fusermount3 -u /tmp/fuse_my_table
# perf output shows lru hits/misses; /tmp/fuselance-cache-<pid>/ removed
```
