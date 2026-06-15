// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/nano_lance_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <utility>

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

}  // namespace

extern "C" {

int nano_lance_fetch_external_blob(const char* uri, uint64_t position, uint64_t size, uint8_t* out_buf, size_t out_cap,
                                   size_t* bytes_read, char* error_message, size_t error_message_capacity) {
    if (uri == nullptr || uri[0] == '\0' || out_buf == nullptr || out_cap == 0U || bytes_read == nullptr) {
        set_error(error_message, error_message_capacity, "invalid argument");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
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

    // Reuse an open handle for this URI, or open one and cache it.
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
    in->clear();  // a previous read may have set eof/fail; clear before repositioning
    in->seekg(static_cast<std::streamoff>(position), std::ios::beg);
    if (!*in) {
        const bool was_s3 = handle->is_s3;
        drop_front_handle();  // stale/unsuitable handle — evict so the next call reopens
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
    // A range GET that failed mid-read (transport/HTTP error, not a clean EOF) leaves a thread error set.
    // Surface it instead of silently returning a short read.
    if (handle->is_s3 && !s3_factory().error().empty()) {
        drop_front_handle();
        set_error(error_message, error_message_capacity, s3_factory().error());
        return NANO_LANCE_READER_IO_ERROR;
    }
#endif
    return NANO_LANCE_READER_OK;
}

}  // extern "C"
