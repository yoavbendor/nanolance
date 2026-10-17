// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// weblance: nanolance's Lance reader in the browser. A Web Worker opens a Lance dataset served over
// HTTP(S) and reads only the byte ranges a take needs. Remote reads are synchronous XMLHttpRequests
// through EM_JS, which browsers allow only in a Web Worker, so this module runs in a worker.
//
// Sources: https:// (any host, Range requests) and http://localhost or http://127.0.0.1 (the test
// server). Listings: huggingface.co dataset trees (/api/datasets/.../tree), and the test server's
// /__tree/<dir>. Every listing records the sizes of the files it lists, so opening a file needs no
// probe request.

#include <emscripten/emscripten.h>

#include <nanoarrow/nanoarrow.h>
#include <nanolance/byte_source.hpp>
#include <nanolance/lance_table_reader.hpp>
#include <nanolance/manifest_reader.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------------
// JavaScript side. Synchronous XHR: allowed in a worker only. Every call returns an HTTP status, or a
// negative code for a failure before one (-1 network error, -2 short body, -3 unparsable listing).
// ---------------------------------------------------------------------------------------------------

// Bytes [offset, offset + len) of `url` into dst. Writes the URL the response came from (after any
// redirect) into final_url, and the Content-Range total into *total_out (-1 when not exposed).
EM_JS(int, wl_js_range, (const char* url, double offset, double len, uint8_t* dst, char* final_url,
                         int final_cap, double* total_out), {
  const target = UTF8ToString(url);
  const xhr = new XMLHttpRequest();
  xhr.open('GET', target, false);
  xhr.responseType = 'arraybuffer';
  xhr.setRequestHeader('Range', 'bytes=' + offset + '-' + (offset + len - 1));
  try {
    xhr.send();
  } catch (e) {
    return -1;
  }
  if (final_cap > 0) stringToUTF8(xhr.responseURL || target, final_url, final_cap);
  const cr = xhr.getResponseHeader('Content-Range');
  const m = cr ? /\\/(\\d+)\\s*$/.exec(cr) : null;
  HEAPF64[total_out >> 3] = m ? Number(m[1]) : -1;
  if (xhr.status !== 206) return xhr.status;
  const body = xhr.response;
  if (!body || body.byteLength !== len) return -2;
  HEAPU8.set(new Uint8Array(body), dst);
  return 206;
});

// A directory listing: one page, its entries kept in globalThis.wlList (see wl_js_list_copy), and
// the Link header's next-page URL (empty on the last page).
EM_JS(int, wl_js_list, (const char* url), {
  globalThis.wlList = { body: "", next: "" };
  const xhr = new XMLHttpRequest();
  xhr.open('GET', UTF8ToString(url), false);
  try {
    xhr.send();
  } catch (e) {
    return -1;
  }
  if (xhr.status !== 200) return xhr.status;
  try {
    const entries = JSON.parse(xhr.responseText);
    globalThis.wlList.body = entries
        .map(e => (e.type === 'directory' ? 'd' : 'f') + '\t' + e.path + '\t' + (e.size || 0))
        .join('\\n');
  } catch (e) {
    return -3;
  }
  const link = xhr.getResponseHeader('Link') || "";
  const m = /<([^>]+)>\\s*;\\s*rel="?next"?/.exec(link);
  globalThis.wlList.next = m ? m[1] : "";
  return 200;
});

EM_JS(int, wl_js_list_len, (int which), {
  return lengthBytesUTF8(which ? globalThis.wlList.next : globalThis.wlList.body);
});

EM_JS(void, wl_js_list_copy, (int which, char* dst, int cap), {
  stringToUTF8(which ? globalThis.wlList.next : globalThis.wlList.body, dst, cap);
});

namespace {

namespace io = nano_lance::io;
using io::DirEntry;

// ---------------------------------------------------------------------------------------------------
// Counters and state
// ---------------------------------------------------------------------------------------------------

struct Stats {
    std::uint64_t requests = 0;   // every XHR: range reads, probes and listing pages
    std::uint64_t bytes = 0;      // body bytes of the range reads
    std::uint64_t listings = 0;   // listing pages
    std::uint64_t fallbacks = 0;  // reads retried at the original URL after the cached one failed
    double ms = 0;                // wall time spent inside XHR calls
};

Stats g_stats;
std::string g_error;
// File URL -> size, from a directory listing or a one-byte probe.
std::map<std::string, std::uint64_t> g_sizes;
// File URL -> the URL it redirected to (a signed CDN URL). Reads go there first.
std::map<std::string, std::string> g_final_urls;
// Directory URL -> its complete listing, for the snapshot one wl_open reads. A file that a complete
// listing of its directory does not name does not exist: no request is made for it.
std::map<std::string, std::vector<DirEntry>> g_listings;

struct Dataset {
    std::string url;
    std::uint64_t version = 0;   // the latest manifest at open; takes read this version
    std::uint64_t rows = 0;
    std::string info_json;
};
std::vector<std::unique_ptr<Dataset>> g_datasets;

double now_ms() { return emscripten_get_now(); }

bool is_local_http(const std::string& uri) {
    static const std::string prefixes[] = {"http://localhost", "http://127.0.0.1"};
    for (const auto& p : prefixes) {
        if (uri.rfind(p, 0) == 0) {
            const auto next = uri.size() > p.size() ? uri[p.size()] : '\0';
            if (next == '\0' || next == ':' || next == '/') {
                return true;
            }
        }
    }
    return false;
}

bool allowed_uri(const std::string& uri) { return uri.rfind("https://", 0) == 0 || is_local_http(uri); }

// ---------------------------------------------------------------------------------------------------
// Range reads
// ---------------------------------------------------------------------------------------------------

// One range request, counted. Returns the HTTP status (206 on success) or a negative code.
int timed_range(const std::string& url, std::uint64_t offset, std::uint64_t len, std::uint8_t* dst,
                std::string& final_url, double& total) {
    std::vector<char> final_buf(8192, '\0');
    const double t0 = now_ms();
    const int status = wl_js_range(url.c_str(), static_cast<double>(offset), static_cast<double>(len), dst,
                                   final_buf.data(), static_cast<int>(final_buf.size()), &total);
    g_stats.ms += now_ms() - t0;
    g_stats.requests += 1;
    if (status == 206) {
        g_stats.bytes += len;
    }
    final_url.assign(final_buf.data());
    if (final_url.size() + 1 >= final_buf.size()) {
        final_url.clear();  // truncated: do not cache a URL that is not the whole one
    }
    return status;
}

// Bytes [offset, offset + len) of the file at `url`. Served from the signed URL the file redirected to
// before when there is one; a failure there is retried once at the original URL.
bool range_read(const std::string& url, std::uint64_t offset, std::uint64_t len, std::uint8_t* dst,
                std::string& error, double* total_out = nullptr) {
    double total = -1;
    int status = 0;
    int network_retries = 6;  // a request that never got a status (a network error) is retried
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto cached = g_final_urls.find(url);
        const bool use_cached = cached != g_final_urls.end();
        const std::string request_url = use_cached ? cached->second : url;
        std::string final_url;
        status = timed_range(request_url, offset, len, dst, final_url, total);
        if (status == 206) {
            if (!final_url.empty() && final_url != url) {
                g_final_urls[url] = final_url;
            }
            if (total_out != nullptr) {
                *total_out = total;
            }
            return true;
        }
        if (use_cached) {
            g_final_urls.erase(url);
            g_stats.fallbacks += 1;
            continue;
        }
        if (status <= 0 && network_retries-- > 0) {
            continue;
        }
        break;
    }
    error = "range read of " + std::to_string(len) + " bytes at " + std::to_string(offset) + " from " + url +
            " failed (HTTP " + std::to_string(status) + ")";
    return false;
}

// A ByteSource over one HTTP file. Lance files never change in place, so version_token stays 0.
class HttpSource final : public io::ByteSource {
public:
    HttpSource(std::string url, std::uint64_t size) : url_(std::move(url)), size_(size) {}

    std::uint64_t size() const override { return size_; }

    bool read_at(std::uint64_t offset, std::uint64_t n, std::uint8_t* dst, std::string& error) override {
        if (n == 0) {
            return true;
        }
        if (offset > size_ || n > size_ - offset) {
            error = "read past the end of " + url_;
            return false;
        }
        return range_read(url_, offset, n, dst, error);
    }

    // Small reads (the footer, column metadata, a short page) become one 256 KiB request, not many.
    std::uint64_t small_read_block() const override { return kSmallBlockBytes; }
    std::uint64_t footer_read_size() const override { return kFooterBytes; }

private:
    // A read below the small-read block is one request of this size (the block is cached, so the next
    // small read nearby costs nothing). Overridable at build time to measure the trade-off.
#ifndef WL_SMALL_BLOCK_KIB
#define WL_SMALL_BLOCK_KIB 16
#endif
    static constexpr std::uint64_t kSmallBlockBytes = WL_SMALL_BLOCK_KIB * 1024U;
#ifndef WL_FOOTER_KIB
#define WL_FOOTER_KIB 64
#endif
    static constexpr std::uint64_t kFooterBytes = WL_FOOTER_KIB * 1024U;
    std::string url_;
    std::uint64_t size_;
};

std::unique_ptr<io::ByteSource> open_remote(const std::string& uri, std::string& error) {
    if (!allowed_uri(uri)) {
        error = "not an https or localhost http URI: " + uri;
        return nullptr;
    }
    auto it = g_sizes.find(uri);
    if (it == g_sizes.end()) {
        const auto slash = uri.rfind('/');
        if (slash != std::string::npos && g_listings.count(uri.substr(0, slash)) != 0U) {
            error = uri + " is not in its directory's listing";
            return nullptr;
        }
        std::uint8_t probe = 0;
        double total = -1;
        if (!range_read(uri, 0, 1, &probe, error, &total)) {
            return nullptr;
        }
        if (total < 0) {
            error = "cannot size " + uri + ": no Content-Range in the response";
            return nullptr;
        }
        it = g_sizes.emplace(uri, static_cast<std::uint64_t>(total)).first;
    }
    return std::make_unique<HttpSource>(uri, it->second);
}

// ---------------------------------------------------------------------------------------------------
// Directory listings
// ---------------------------------------------------------------------------------------------------

struct Remote {
    std::string list_url;  // the first listing page
    std::string base;      // a file's URL is base + its path from the root (repository or origin)
    std::string dir_rel;   // the directory's path from that root, without a trailing '/'
};

std::string trim_slashes(std::string s) {
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

bool remote_for(const std::string& dir, Remote& r, std::string& error) {
    static const std::string hf = "https://huggingface.co/datasets/";
    if (dir.rfind(hf, 0) == 0) {
        // datasets/<owner>/<repo>/resolve/<rev>[/<dir>]
        const std::string rest = dir.substr(hf.size());
        const auto resolve = rest.find("/resolve/");
        if (resolve == std::string::npos) {
            error = "not a Hugging Face resolve URL: " + dir;
            return false;
        }
        const std::string repo = rest.substr(0, resolve);
        const std::string after = rest.substr(resolve + 9);
        const auto slash = after.find('/');
        const std::string rev = after.substr(0, slash);
        const std::string dir_rel = trim_slashes(slash == std::string::npos ? "" : after.substr(slash + 1));
        r.base = hf + repo + "/resolve/" + rev + "/";
        r.dir_rel = dir_rel;
        r.list_url = "https://huggingface.co/api/datasets/" + repo + "/tree/" + rev +
                     (dir_rel.empty() ? "" : "/" + dir_rel) + "?recursive=false";
        return true;
    }
    if (is_local_http(dir)) {
        // The test server: <origin>/__tree/<dir>, entries relative to the origin.
        const auto slash = dir.find('/', 7);
        const std::string origin = slash == std::string::npos ? dir : dir.substr(0, slash);
        const std::string dir_rel = trim_slashes(slash == std::string::npos ? "" : dir.substr(slash + 1));
        r.base = origin + "/";
        r.dir_rel = dir_rel;
        r.list_url = origin + "/__tree/" + dir_rel;
        return true;
    }
    error = "no directory listing for " + dir;
    return false;
}

std::string js_listing_text(int which) {
    const int n = wl_js_list_len(which);
    std::string s(static_cast<std::size_t>(n) + 1, '\0');
    wl_js_list_copy(which, s.data(), n + 1);
    s.resize(static_cast<std::size_t>(n));
    return s;
}

// Entries directly under `dir`, each file's size recorded for its URL. A missing directory lists as
// empty (a 404), as the library expects for an absent prefix.
bool list_remote(const std::string& dir, std::vector<DirEntry>& out, std::string& error) {
    Remote r;
    if (!remote_for(dir, r, error)) {
        return false;
    }
    const std::string prefix = r.dir_rel.empty() ? "" : r.dir_rel + "/";
    std::string next = r.list_url;
    for (int page = 0; !next.empty() && page < 10000; ++page) {
        int status = 0;
        for (int tries = 0; tries < 6; ++tries) {  // a network error (no status) is retried
            const double t0 = now_ms();
            status = wl_js_list(next.c_str());
            g_stats.ms += now_ms() - t0;
            g_stats.requests += 1;
            g_stats.listings += 1;
            if (status > 0) {
                break;
            }
        }
        if (status == 404) {
            break;
        }
        if (status != 200) {
            error = "listing " + next + " failed (HTTP " + std::to_string(status) + ")";
            out.clear();
            return false;
        }
        const std::string body = js_listing_text(0);
        std::size_t pos = 0;
        while (pos < body.size()) {
            auto end = body.find('\n', pos);
            if (end == std::string::npos) {
                end = body.size();
            }
            const std::string line = body.substr(pos, end - pos);
            pos = end + 1;
            const auto t1 = line.find('\t');
            const auto t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
            if (t2 == std::string::npos) {
                continue;
            }
            const bool is_dir = line[0] == 'd';
            const std::string path = line.substr(t1 + 1, t2 - t1 - 1);
            if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            const std::string name = path.substr(prefix.size());
            if (name.find('/') != std::string::npos) {
                continue;  // below the directory, not in it
            }
            DirEntry entry;
            entry.name = name;
            entry.is_dir = is_dir;
            entry.size = is_dir ? 0 : std::strtoull(line.c_str() + t2 + 1, nullptr, 10);
            if (!is_dir) {
                g_sizes[r.base + path] = entry.size;
            }
            out.push_back(std::move(entry));
        }
        next = js_listing_text(1);
    }
    return true;
}

// list_remote, memoized for the current open (see g_listings). Failures are not stored.
bool list_cached(const std::string& dir, std::vector<DirEntry>& out, std::string& error) {
    const auto cached = g_listings.find(dir);
    if (cached != g_listings.end()) {
        out = cached->second;
        return true;
    }
    if (!list_remote(dir, out, error)) {
        return false;
    }
    g_listings[dir] = out;
    return true;
}

void register_sources() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    io::register_source_factory("https", open_remote);
    io::register_source_factory("http", open_remote);
    io::register_list_factory("https", list_cached);
    io::register_list_factory("http", list_cached);
}

// ---------------------------------------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------------------------------------

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

char* dup_c(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p != nullptr) {
        std::memcpy(p, s.c_str(), s.size() + 1);
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------
// Types and values
// ---------------------------------------------------------------------------------------------------

enum class Kind { Int, UInt, Float32, Float64, Bool, Str, Binary, Unsupported };

Kind kind_of(const ArrowSchema* schema) {
    ArrowSchemaView view;
    struct ArrowError err {};
    if (ArrowSchemaViewInit(&view, schema, &err) != NANOARROW_OK) {
        return Kind::Unsupported;
    }
    switch (view.type) {
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_DATE32:
        case NANOARROW_TYPE_TIMESTAMP:
            return Kind::Int;
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64:
            return Kind::UInt;
        case NANOARROW_TYPE_FLOAT:
            return Kind::Float32;
        case NANOARROW_TYPE_DOUBLE:
            return Kind::Float64;
        case NANOARROW_TYPE_BOOL:
            return Kind::Bool;
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
            return Kind::Str;
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY:
            return Kind::Binary;
        default:
            return Kind::Unsupported;
    }
}

std::string type_name(const ArrowSchema* schema) {
    ArrowSchemaView view;
    struct ArrowError err {};
    if (ArrowSchemaViewInit(&view, schema, &err) != NANOARROW_OK) {
        return schema->format != nullptr ? schema->format : "?";
    }
    switch (view.type) {
        case NANOARROW_TYPE_FIXED_SIZE_LIST:
            return "fixed_size_list<" + type_name(schema->children[0]) + ">[" + std::to_string(view.fixed_size) + "]";
        case NANOARROW_TYPE_LIST:
            return "list<" + type_name(schema->children[0]) + ">";
        case NANOARROW_TYPE_STRUCT:
            return "struct";
        case NANOARROW_TYPE_BOOL: return "bool";
        case NANOARROW_TYPE_INT8: return "int8";
        case NANOARROW_TYPE_INT16: return "int16";
        case NANOARROW_TYPE_INT32: return "int32";
        case NANOARROW_TYPE_INT64: return "int64";
        case NANOARROW_TYPE_UINT8: return "uint8";
        case NANOARROW_TYPE_UINT16: return "uint16";
        case NANOARROW_TYPE_UINT32: return "uint32";
        case NANOARROW_TYPE_UINT64: return "uint64";
        case NANOARROW_TYPE_FLOAT: return "float32";
        case NANOARROW_TYPE_DOUBLE: return "float64";
        case NANOARROW_TYPE_STRING: return "string";
        case NANOARROW_TYPE_LARGE_STRING: return "large_string";
        case NANOARROW_TYPE_BINARY: return "binary";
        case NANOARROW_TYPE_LARGE_BINARY: return "large_binary";
        case NANOARROW_TYPE_DATE32: return "date32";
        case NANOARROW_TYPE_TIMESTAMP: return "timestamp";
        default:
            return schema->format != nullptr ? schema->format : "?";
    }
}

void append_value(std::string& out, const ArrowArrayView* a, std::int64_t i, Kind kind) {
    if (ArrowArrayViewIsNull(a, i) != 0) {
        out += "null";
        return;
    }
    char buf[40];
    switch (kind) {
        case Kind::Int:
            out += std::to_string(static_cast<long long>(ArrowArrayViewGetIntUnsafe(a, i)));
            break;
        case Kind::UInt:
            out += std::to_string(static_cast<unsigned long long>(ArrowArrayViewGetUIntUnsafe(a, i)));
            break;
        case Kind::Float32:
        case Kind::Float64: {
            const double d = ArrowArrayViewGetDoubleUnsafe(a, i);
            if (!std::isfinite(d)) {
                out += "null";
            } else {
                std::snprintf(buf, sizeof buf, kind == Kind::Float32 ? "%.9g" : "%.17g", d);
                out += buf;
            }
            break;
        }
        case Kind::Bool:
            out += ArrowArrayViewGetIntUnsafe(a, i) != 0 ? "true" : "false";
            break;
        case Kind::Str: {
            const ArrowStringView sv = ArrowArrayViewGetStringUnsafe(a, i);
            out += json_string(std::string(sv.data != nullptr ? sv.data : "", static_cast<std::size_t>(sv.size_bytes)));
            break;
        }
        default:
            out += "null";
    }
}

// ---------------------------------------------------------------------------------------------------
// Takes
// ---------------------------------------------------------------------------------------------------

// The rows at `rows` (any order, duplicates allowed) of the columns `columns`, read through
// lance_dataset_take at the dataset's pinned version. The take returns the rows sorted and unique,
// one batch per fragment; locate() maps a sorted position to its batch and offset.
class Take {
public:
    Take() = default;
    Take(const Take&) = delete;
    Take& operator=(const Take&) = delete;
    ~Take() {
        for (auto& v : views_) {
            ArrowArrayViewReset(&v);
        }
        for (auto& b : batches_) {
            if (b.release != nullptr) {
                b.release(&b);
            }
        }
        if (schema_.release != nullptr) {
            schema_.release(&schema_);
        }
    }

    bool run(const Dataset& ds, const std::vector<std::string>& columns, std::vector<std::uint64_t> rows,
             std::string& error) {
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        sorted_ = rows;
        nano_lance::LanceScanRequest request;
        request.columns = &columns;
        request.has_version = true;
        request.version = ds.version;
        if (!nano_lance::lance_dataset_take(ds.url, request, sorted_, schema_, batches_, error)) {
            return false;
        }
        std::uint64_t total = 0;
        for (const auto& b : batches_) {
            starts_.push_back(total);
            total += static_cast<std::uint64_t>(b.length);
        }
        starts_.push_back(total);
        if (total != sorted_.size()) {
            error = "take returned " + std::to_string(total) + " rows for " + std::to_string(sorted_.size()) + " indices";
            return false;
        }
        views_.reserve(batches_.size());
        for (auto& batch : batches_) {
            ArrowArrayView view;
            struct ArrowError err {};
            if (ArrowArrayViewInitFromSchema(&view, &schema_, &err) != NANOARROW_OK ||
                ArrowArrayViewSetArray(&view, &batch, &err) != NANOARROW_OK) {
                error = std::string("cannot read the take's arrays: ") + err.message;
                ArrowArrayViewReset(&view);
                return false;
            }
            views_.push_back(view);
        }
        return true;
    }

    // Position of requested row `row` in the sorted, unique list.
    std::uint64_t sorted_position(std::uint64_t row) const {
        return static_cast<std::uint64_t>(std::lower_bound(sorted_.begin(), sorted_.end(), row) - sorted_.begin());
    }

    // Batch index and offset of sorted position `k`.
    void locate(std::uint64_t k, std::size_t& batch, std::int64_t& offset) const {
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), k) - 1;
        batch = static_cast<std::size_t>(it - starts_.begin());
        offset = static_cast<std::int64_t>(k - *it);
    }

    ArrowSchema schema_{};
    std::vector<ArrowArray> batches_;
    std::vector<ArrowArrayView> views_;  // one top-level struct view per batch
    std::vector<std::uint64_t> starts_;  // the first sorted position of each batch, then the total
    std::vector<std::uint64_t> sorted_;
};

Dataset* dataset(int handle) {
    if (handle < 1 || static_cast<std::size_t>(handle) > g_datasets.size()) {
        g_error = "no open dataset with handle " + std::to_string(handle);
        return nullptr;
    }
    return g_datasets[static_cast<std::size_t>(handle - 1)].get();
}

std::vector<std::string> split_csv(const char* csv) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&out](std::string s) {
        const auto b = s.find_first_not_of(" \t");
        const auto e = s.find_last_not_of(" \t");
        if (b != std::string::npos) {
            out.push_back(s.substr(b, e - b + 1));
        }
    };
    for (const char* p = csv; *p != '\0'; ++p) {
        if (*p == ',') {
            flush(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    flush(cur);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// The C API the page calls (the exported names are listed in wasm/build_weblance.sh)
// ---------------------------------------------------------------------------------------------------

extern "C" {

// Opens a dataset URL (https://huggingface.co/datasets/<o>/<r>/resolve/<rev>/<path>.lance, or a
// localhost test URL). Returns a handle (>= 1), or 0 with wl_error() set.
EMSCRIPTEN_KEEPALIVE int wl_open(const char* dataset_url) {
    register_sources();
    g_error.clear();
    g_listings.clear();  // each open reads a fresh snapshot of the directories
    std::string url = trim_slashes(dataset_url != nullptr ? dataset_url : "");
    if (!allowed_uri(url)) {
        g_error = "not an https or localhost http URL: " + url;
        return 0;
    }
    std::string error;
    // The data directory's listing records every data file's size: the reader needs no probe per file.
    // Best effort: a failure here only costs a probe later.
    std::vector<DirEntry> data_files;
    io::list_dir(url + "/data", data_files, error, true);
    error.clear();

    const std::uint64_t version = nano_lance::highest_manifest_version(url, error);
    if (version == 0) {
        g_error = error.empty() ? "no Lance manifest under " + url : error;
        return 0;
    }
    auto ds = std::make_unique<Dataset>();
    ds->url = url;
    ds->version = version;
    if (!nano_lance::lance_table_count_rows(url, ds->rows, error)) {
        g_error = error;
        return 0;
    }
    ArrowSchema schema{};
    if (!nano_lance::lance_table_read_schema(url, schema, error)) {
        g_error = error;
        return 0;
    }
    std::string info = "{\"rows\":" + std::to_string(ds->rows) + ",\"version\":" + std::to_string(version) +
                       ",\"fields\":[";
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (i > 0) {
            info += ',';
        }
        info += "{\"name\":" + json_string(schema.children[i]->name) +
                ",\"type\":" + json_string(type_name(schema.children[i])) + "}";
    }
    info += "]}";
    schema.release(&schema);
    ds->info_json = std::move(info);
    g_datasets.push_back(std::move(ds));
    return static_cast<int>(g_datasets.size());
}

EMSCRIPTEN_KEEPALIVE const char* wl_error() { return g_error.c_str(); }

// {"rows":N,"version":V,"fields":[{"name":...,"type":...}, ...]}. Caller frees with wl_free.
EMSCRIPTEN_KEEPALIVE char* wl_info_json(int handle) {
    const Dataset* ds = dataset(handle);
    return ds != nullptr ? dup_c(ds->info_json) : nullptr;
}

// The scalar and string columns of `rows` (in the given order) as a JSON array of objects. A column
// that is binary or nested is an error: take those with wl_take_binary. Caller frees with wl_free.
EMSCRIPTEN_KEEPALIVE char* wl_take_json(int handle, const std::int64_t* rows, int n, const char* columns_csv) {
    Dataset* ds = dataset(handle);
    if (ds == nullptr) {
        return nullptr;
    }
    if (rows == nullptr || n <= 0 || columns_csv == nullptr) {
        g_error = "wl_take_json needs rows and columns";
        return nullptr;
    }
    const auto columns = split_csv(columns_csv);
    if (columns.empty()) {
        g_error = "wl_take_json needs at least one column";
        return nullptr;
    }
    std::vector<std::uint64_t> wanted;
    wanted.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (rows[i] < 0) {
            g_error = "negative row " + std::to_string(rows[i]);
            return nullptr;
        }
        wanted.push_back(static_cast<std::uint64_t>(rows[i]));
    }
    Take take;
    std::string error;
    if (!take.run(*ds, columns, wanted, error)) {
        g_error = error;
        return nullptr;
    }
    std::vector<Kind> kinds;
    std::vector<std::string> keys;
    for (std::int64_t c = 0; c < take.schema_.n_children; ++c) {
        const Kind kind = kind_of(take.schema_.children[c]);
        if (kind == Kind::Binary || kind == Kind::Unsupported) {
            g_error = std::string("column ") + take.schema_.children[c]->name +
                      " cannot be taken as JSON: use wl_take_binary or a scalar column";
            return nullptr;
        }
        kinds.push_back(kind);
        keys.push_back(take.schema_.children[c]->name);
    }
    std::string out = "[";
    for (int i = 0; i < n; ++i) {
        if (i > 0) {
            out += ',';
        }
        std::size_t batch = 0;
        std::int64_t offset = 0;
        take.locate(take.sorted_position(wanted[static_cast<std::size_t>(i)]), batch, offset);
        out += '{';
        for (std::size_t c = 0; c < keys.size(); ++c) {
            if (c > 0) {
                out += ',';
            }
            out += json_string(keys[c]) + ':';
            append_value(out, take.views_[batch].children[c], offset, kinds[c]);
        }
        out += '}';
    }
    out += ']';
    return dup_c(out);
}

// One binary (or string) value of one row, as a malloc'ed copy: caller frees with wl_free. *out_len
// is its length, or -1 with NULL on failure (wl_error()).
EMSCRIPTEN_KEEPALIVE void* wl_take_binary(int handle, std::int64_t row, const char* column, int* out_len) {
    if (out_len != nullptr) {
        *out_len = -1;
    }
    Dataset* ds = dataset(handle);
    if (ds == nullptr) {
        return nullptr;
    }
    if (row < 0 || column == nullptr) {
        g_error = "wl_take_binary needs a row >= 0 and a column";
        return nullptr;
    }
    const std::vector<std::string> columns{column};
    Take take;
    std::string error;
    if (!take.run(*ds, columns, {static_cast<std::uint64_t>(row)}, error)) {
        g_error = error;
        return nullptr;
    }
    if (take.schema_.n_children != 1) {
        g_error = "wl_take_binary: expected one column";
        return nullptr;
    }
    const Kind kind = kind_of(take.schema_.children[0]);
    if (kind != Kind::Binary && kind != Kind::Str) {
        g_error = std::string("column ") + column + " is not binary";
        return nullptr;
    }
    std::size_t batch = 0;
    std::int64_t offset = 0;
    take.locate(0, batch, offset);
    const ArrowArrayView* a = take.views_[batch].children[0];
    if (ArrowArrayViewIsNull(a, offset) != 0) {
        g_error = std::string("row ") + std::to_string(row) + " of " + column + " is null";
        return nullptr;
    }
    const std::uint8_t* data = nullptr;
    std::int64_t size = 0;
    if (kind == Kind::Binary) {
        const ArrowBufferView bv = ArrowArrayViewGetBytesUnsafe(a, offset);
        data = bv.data.as_uint8;
        size = bv.size_bytes;
    } else {
        const ArrowStringView sv = ArrowArrayViewGetStringUnsafe(a, offset);
        data = reinterpret_cast<const std::uint8_t*>(sv.data);
        size = sv.size_bytes;
    }
    void* copy = std::malloc(size > 0 ? static_cast<std::size_t>(size) : 1U);
    if (copy == nullptr) {
        g_error = "out of memory";
        return nullptr;
    }
    if (size > 0) {
        std::memcpy(copy, data, static_cast<std::size_t>(size));
    }
    if (out_len != nullptr) {
        *out_len = static_cast<int>(size);
    }
    return copy;
}

// {"requests":R,"bytes":B,"ms":T,"listings":L,"fallbacks":F}, since the last wl_reset_stats.
EMSCRIPTEN_KEEPALIVE char* wl_stats_json() {
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.1f", g_stats.ms);
    const std::string out = "{\"requests\":" + std::to_string(g_stats.requests) +
                            ",\"bytes\":" + std::to_string(g_stats.bytes) + ",\"ms\":" + ms +
                            ",\"listings\":" + std::to_string(g_stats.listings) +
                            ",\"fallbacks\":" + std::to_string(g_stats.fallbacks) + "}";
    return dup_c(out);
}

EMSCRIPTEN_KEEPALIVE void wl_reset_stats() { g_stats = Stats{}; }

EMSCRIPTEN_KEEPALIVE void wl_free(void* p) { std::free(p); }

}  // extern "C"
