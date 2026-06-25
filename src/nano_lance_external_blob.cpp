// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/nano_lance_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <utime.h>
#include <utility>
#include <vector>

#ifdef NANO_LANCE_READER_HAS_S3
#ifdef NANO_LANCE_S3_AWS_SDK
#include <s3_aws_sdk_stream.h>
using NanoLanceS3Factory = AwsSdkStreamFactory;
#else
#include "nanos3reader/s3_reader.h"
using NanoLanceS3Factory = nanos3reader::S3MinStreamFactory;
#endif
#endif

// External-blob fetch. The workload reads millions of small ranged slices out of each external object
// (one pcapng/hour), often several times (L1/L2/L3 augmentation passes), so reopening the object per
// fetch is fatal: for file:// it is an open()/close() per row; for s3:// it was a fresh S3 GET per row,
// each pulling a 32 MiB read-ahead that was then discarded. Instead we keep a small per-thread LRU of
// OPEN handles keyed by URI and reuse them: file:// seeks within one fd, and s3:// reuses one seekable
// stream so its read-ahead buffer serves the consecutive slices (≈1 GET per buffer, not per row). The
// cache is thread_local, so parallel bulk fetch needs no locking and each thread keeps its own position.

namespace {

void set_error(char* error_message, size_t error_message_capacity, const std::string& msg) {
    if (error_message != nullptr && error_message_capacity > 0U) {
        std::strncpy(error_message, msg.c_str(), error_message_capacity - 1U);
        error_message[error_message_capacity - 1U] = '\0';
    }
}

// "file:///C:/x" -> "C:/x" (drop the slash before a Windows drive letter); "file:///path" -> "/path".
std::string file_uri_to_path(const std::string& suri) {
    std::string path = suri.substr(7);  // strip "file://"
    if (path.size() >= 3 && path[0] == '/' &&
        ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z')) && path[2] == ':') {
        path.erase(0, 1);
    }
    return path;
}

#ifdef NANO_LANCE_READER_HAS_S3
// Process-lifetime S3 stream factory. Intentionally leaked (never destroyed) so cached S3 streams can
// always destruct without an SDK-shutdown ordering hazard at program exit; the OS reclaims at exit.
NanoLanceS3Factory& s3_factory() {
    static NanoLanceS3Factory* factory = new NanoLanceS3Factory();
    return *factory;
}
constexpr std::size_t kS3ReadAheadBytes = 32U * 1024U * 1024U;
#endif

// One open object handle: a file stream or a (seekable, range-GET-backed) S3 stream.
struct BlobHandle {
    bool is_s3 = false;
    std::ifstream file;
    std::unique_ptr<std::istream> s3stream;
    std::istream* stream() noexcept { return is_s3 ? s3stream.get() : static_cast<std::istream*>(&file); }
};

// Per-thread LRU keyed by URI. Small cap: the access pattern is "drain one object's slices, then the
// next", so even a few entries cover light interleaving across augmentation passes.
constexpr std::size_t kMaxHandles = 8;
thread_local std::list<std::pair<std::string, std::unique_ptr<BlobHandle>>> g_handles;

BlobHandle* find_handle(const std::string& uri) {
    for (auto it = g_handles.begin(); it != g_handles.end(); ++it) {
        if (it->first == uri) {
            g_handles.splice(g_handles.begin(), g_handles, it);  // promote to most-recently-used
            return g_handles.front().second.get();
        }
    }
    return nullptr;
}

void drop_front_handle() {
    if (!g_handles.empty()) {
        g_handles.pop_front();
    }
}

// ---------------------------------------------------------------------------
// Disk-resident LRU block cache
// ---------------------------------------------------------------------------
// When enabled via nano_lance_block_cache_configure(), each 32 MiB aligned
// block fetched from remote storage is persisted as a flat file in g_blk_dir.
// On subsequent requests for the same (uri, chunk_start) pair the block is
// served from disk, skipping the remote round-trip entirely.
//
// Block file naming: <djb2_hex16(uri)>_<chunk_start_hex16>.blk
// LRU discipline: mtime-based. On hit: utime() promotes the file. On miss
// after write: if file count > limit, delete the oldest-mtime file.
//
// Thread safety: g_blk_mu guards the one-time configure call only. All I/O
// runs on the caller's thread. fuselance runs single-threaded (-s), so there
// is no concurrent disk access.

static constexpr size_t kBlkSize = 32U * 1024U * 1024U;  // 32 MiB per cache block

static int         g_blk_limit  = 0;   // 0 = disabled
static std::string g_blk_dir;
static std::mutex  g_blk_mu;           // guards configure only
static uint64_t    g_blk_hits   = 0;
static uint64_t    g_blk_misses = 0;

static std::string blk_uri_key(const std::string& uri) {
    uint64_t h = 5381;
    for (unsigned char c : uri) h = ((h << 5) + h) ^ static_cast<uint64_t>(c);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

static std::string blk_path(const std::string& uri, uint64_t chunk_start) {
    char tail[34];
    std::snprintf(tail, sizeof(tail), "_%016llx.blk", static_cast<unsigned long long>(chunk_start));
    return g_blk_dir + "/" + blk_uri_key(uri) + tail;
}

static void blk_evict_if_needed() {
    std::vector<std::pair<time_t, std::string>> entries;
    DIR* d = ::opendir(g_blk_dir.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = ::readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        std::string p = g_blk_dir + "/" + de->d_name;
        struct stat st{};
        if (::stat(p.c_str(), &st) == 0) entries.emplace_back(st.st_mtime, p);
    }
    ::closedir(d);
    int excess = static_cast<int>(entries.size()) - g_blk_limit;
    if (excess <= 0) return;
    std::sort(entries.begin(), entries.end());  // oldest mtime first
    for (int i = 0; i < excess; ++i) ::unlink(entries[i].second.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal: raw remote fetch (no disk cache layer). Used by both the
// public API (cache-disabled path) and the cache-miss path (to populate disk).
// ---------------------------------------------------------------------------
static int fetch_from_remote(const char* uri, uint64_t position, uint64_t size,
                              uint8_t* out_buf, size_t out_cap, size_t* bytes_read,
                              char* error_message, size_t error_message_capacity) {
    *bytes_read = 0U;
    const std::string suri(uri);
    const bool is_file = suri.rfind("file://", 0) == 0;
#ifdef NANO_LANCE_READER_HAS_S3
    const bool is_s3 = suri.rfind("s3://", 0) == 0;
#else
    const bool is_s3 = false;
#endif
    if (!is_file && !is_s3) {
        set_error(error_message, error_message_capacity,
                  "unsupported URI scheme (expected file://"
#ifdef NANO_LANCE_READER_HAS_S3
                  " or s3://"
#endif
                  ")");
        return NANO_LANCE_READER_IO_ERROR;
    }

    BlobHandle* handle = find_handle(suri);
    if (handle == nullptr) {
        auto fresh = std::make_unique<BlobHandle>();
        if (is_file) {
            fresh->is_s3 = false;
            fresh->file.open(file_uri_to_path(suri), std::ios::binary);
            if (!fresh->file) {
                set_error(error_message, error_message_capacity, "failed to open file:// path");
                return NANO_LANCE_READER_IO_ERROR;
            }
        } else {
#ifdef NANO_LANCE_READER_HAS_S3
            fresh->is_s3 = true;
            fresh->s3stream = s3_factory().open(suri, kS3ReadAheadBytes);
            if (!fresh->s3stream) {
                set_error(error_message, error_message_capacity,
                          s3_factory().error().empty() ? "failed to open s3:// stream" : s3_factory().error());
                return NANO_LANCE_READER_IO_ERROR;
            }
#endif
        }
        g_handles.emplace_front(suri, std::move(fresh));
        if (g_handles.size() > kMaxHandles) {
            g_handles.pop_back();
        }
        handle = g_handles.front().second.get();
    }

    std::istream* in = handle->stream();
    in->clear();
    in->seekg(static_cast<std::streamoff>(position), std::ios::beg);
    if (!*in) {
        const bool was_s3 = handle->is_s3;
        drop_front_handle();
#ifdef NANO_LANCE_READER_HAS_S3
        if (was_s3 && !s3_factory().error().empty()) {
            set_error(error_message, error_message_capacity, s3_factory().error());
        } else
#else
        (void)was_s3;
#endif
        set_error(error_message, error_message_capacity, "failed to seek external blob");
        return NANO_LANCE_READER_IO_ERROR;
    }
    std::streamsize to_read = static_cast<std::streamsize>(out_cap);
    if (size != std::numeric_limits<uint64_t>::max()) {
        to_read = static_cast<std::streamsize>(std::min<uint64_t>(size, static_cast<uint64_t>(out_cap)));
    }
    in->read(reinterpret_cast<char*>(out_buf), to_read);
    *bytes_read = static_cast<size_t>(std::max<std::streamsize>(0, in->gcount()));
#ifdef NANO_LANCE_READER_HAS_S3
    if (handle->is_s3 && !s3_factory().error().empty()) {
        drop_front_handle();
        set_error(error_message, error_message_capacity, s3_factory().error());
        return NANO_LANCE_READER_IO_ERROR;
    }
#endif
    return NANO_LANCE_READER_OK;
}

extern "C" {

int nano_lance_block_cache_configure(const char* cache_dir, int max_blocks,
                                     char* error_message, size_t error_message_capacity) {
    if (max_blocks != 0 && (max_blocks < 2 || max_blocks > 500)) {
        set_error(error_message, error_message_capacity,
                  "nano_lance_block_cache_configure: max_blocks must be 0 or in range 2–500");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    if (max_blocks > 0 && (cache_dir == nullptr || cache_dir[0] == '\0')) {
        set_error(error_message, error_message_capacity,
                  "nano_lance_block_cache_configure: cache_dir must not be empty when max_blocks > 0");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(g_blk_mu);
    g_blk_limit  = max_blocks;
    g_blk_dir    = (max_blocks > 0 && cache_dir) ? cache_dir : "";
    g_blk_hits   = 0;
    g_blk_misses = 0;
    if (max_blocks > 0) {
        std::error_code ec;
        std::filesystem::create_directories(g_blk_dir, ec);
        if (ec) {
            set_error(error_message, error_message_capacity,
                      ("nano_lance_block_cache_configure: cannot create cache_dir: " + ec.message()).c_str());
            g_blk_limit = 0;
            return NANO_LANCE_READER_IO_ERROR;
        }
    }
    return NANO_LANCE_READER_OK;
}

void nano_lance_block_cache_stats(uint64_t* out_hits, uint64_t* out_misses) {
    if (out_hits)   *out_hits   = g_blk_hits;
    if (out_misses) *out_misses = g_blk_misses;
}

int nano_lance_fetch_external_blob(const char* uri, uint64_t position, uint64_t size, uint8_t* out_buf, size_t out_cap,
                                   size_t* bytes_read, char* error_message, size_t error_message_capacity) {
    if (uri == nullptr || uri[0] == '\0' || out_buf == nullptr || out_cap == 0U || bytes_read == nullptr) {
        set_error(error_message, error_message_capacity, "invalid argument");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    *bytes_read = 0U;

    // ---- Disk block cache (when enabled) ------------------------------------
    if (g_blk_limit > 0) {
        const std::string suri(uri);
        const uint64_t chunk_start = (position / kBlkSize) * kBlkSize;
        const size_t   rel_off     = static_cast<size_t>(position - chunk_start);
        const std::string bpath    = blk_path(suri, chunk_start);

        struct stat bst{};
        if (::stat(bpath.c_str(), &bst) == 0 && bst.st_size > 0) {
            // Cache hit: promote mtime and serve from disk.
            ::utime(bpath.c_str(), nullptr);
            ++g_blk_hits;
            const size_t block_sz = static_cast<size_t>(bst.st_size);
            if (rel_off >= block_sz) return NANO_LANCE_READER_OK;
            size_t avail = block_sz - rel_off;
            size_t req   = (size == std::numeric_limits<uint64_t>::max())
                           ? out_cap
                           : static_cast<size_t>(std::min<uint64_t>(size, static_cast<uint64_t>(out_cap)));
            size_t want  = std::min(avail, req);
            FILE* f = std::fopen(bpath.c_str(), "rb");
            if (f) {
                std::fseek(f, static_cast<long>(rel_off), SEEK_SET);
                *bytes_read = std::fread(out_buf, 1, want, f);
                std::fclose(f);
                return NANO_LANCE_READER_OK;
            }
            // fopen failed — fall through to remote fetch (don't count as miss).
            --g_blk_hits;
        }

        // Cache miss: fetch the full aligned block from remote storage.
        ++g_blk_misses;
        std::vector<uint8_t> blk_buf(kBlkSize);
        size_t blk_fetched = 0;
        int rc = fetch_from_remote(uri, chunk_start, static_cast<uint64_t>(kBlkSize),
                                   blk_buf.data(), kBlkSize,
                                   &blk_fetched, error_message, error_message_capacity);
        if (rc != NANO_LANCE_READER_OK && blk_fetched == 0) return rc;

        // Write block to disk.
        if (blk_fetched > 0) {
            FILE* f = std::fopen(bpath.c_str(), "wb");
            if (f) {
                std::fwrite(blk_buf.data(), 1, blk_fetched, f);
                std::fclose(f);
                blk_evict_if_needed();
            }
        }

        if (rel_off >= blk_fetched) return NANO_LANCE_READER_OK;
        size_t avail = blk_fetched - rel_off;
        size_t req   = (size == std::numeric_limits<uint64_t>::max())
                       ? out_cap
                       : static_cast<size_t>(std::min<uint64_t>(size, static_cast<uint64_t>(out_cap)));
        *bytes_read = std::min(avail, req);
        std::memcpy(out_buf, blk_buf.data() + rel_off, *bytes_read);
        return NANO_LANCE_READER_OK;
    }
    // ---- End disk cache -----------------------------------------------------

    return fetch_from_remote(uri, position, size, out_buf, out_cap, bytes_read,
                             error_message, error_message_capacity);
}

}  // extern "C"
