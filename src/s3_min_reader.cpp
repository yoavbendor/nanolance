// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/s3_min_reader.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <random>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace nanolance {
namespace s3v4 {

namespace {

const char* const kHexDigits = "0123456789abcdef";

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return "";
    }
    const auto e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::string hex_encode(const unsigned char* data, std::size_t len) {
    std::string out;
    out.resize(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out[2 * i] = kHexDigits[data[i] >> 4];
        out[2 * i + 1] = kHexDigits[data[i] & 0x0F];
    }
    return out;
}

std::string sha256_hex(const std::string& payload) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest);
    return hex_encode(digest, sizeof digest);
}

std::vector<unsigned char> hmac_sha256(const std::vector<unsigned char>& key, const std::string& msg) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), out, &out_len);
    return std::vector<unsigned char>(out, out + out_len);
}

std::vector<unsigned char> derive_signing_key(const std::string& secret, const std::string& date,
                                              const std::string& region, const std::string& service) {
    const std::string k0 = "AWS4" + secret;
    const std::vector<unsigned char> k_secret(k0.begin(), k0.end());
    const auto k_date = hmac_sha256(k_secret, date);
    const auto k_region = hmac_sha256(k_date, region);
    const auto k_service = hmac_sha256(k_region, service);
    return hmac_sha256(k_service, "aws4_request");
}

std::string build_authorization(const CanonicalRequest& req, const std::string& access_key,
                                const std::string& secret_key, const std::string& region,
                                const std::string& service, const std::string& amz_date,
                                std::string* out_signed_headers) {
    // Headers sorted by (lowercase) name for the canonical request and the signed-headers list.
    std::vector<Header> headers = req.headers;
    std::sort(headers.begin(), headers.end(),
              [](const Header& a, const Header& b) { return a.name < b.name; });

    std::string canonical_headers;
    std::string signed_headers;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        canonical_headers += headers[i].name + ":" + trim(headers[i].value) + "\n";
        signed_headers += headers[i].name;
        if (i + 1 < headers.size()) {
            signed_headers += ";";
        }
    }

    const std::string canonical_request = req.method + "\n" + req.uri + "\n" + req.query + "\n" +
                                          canonical_headers + "\n" + signed_headers + "\n" + req.payload_hash;

    const std::string scope_date = amz_date.substr(0, 8);
    const std::string credential_scope = scope_date + "/" + region + "/" + service + "/aws4_request";
    const std::string string_to_sign = std::string("AWS4-HMAC-SHA256\n") + amz_date + "\n" + credential_scope +
                                        "\n" + sha256_hex(canonical_request);

    const auto signing_key = derive_signing_key(secret_key, scope_date, region, service);
    const auto sig_bytes = hmac_sha256(signing_key, string_to_sign);
    const std::string signature = hex_encode(sig_bytes.data(), sig_bytes.size());

    if (out_signed_headers != nullptr) {
        *out_signed_headers = signed_headers;
    }
    return "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + credential_scope +
           ", SignedHeaders=" + signed_headers + ", Signature=" + signature;
}

}  // namespace s3v4

// --- factory + stream ------------------------------------------------------------------------------

namespace s3detail {
struct Config {
    std::string access_key;
    std::string secret_key;
    std::string session_token;
    std::string region;
    std::string endpoint;    // empty -> AWS virtual-hosted; non-empty -> path-style against this endpoint
    bool path_style = false;
    int max_attempts = 3;    // total tries per GET (AWS_MAX_ATTEMPTS), >= 1
};
}  // namespace s3detail

namespace {

// Per-thread last-error, surfaced by S3MinStreamFactory::error(). Thread-local because the external-blob
// handle cache (the sole caller) is itself thread-local and lock-free. Cleared when a window loads cleanly
// or a logical seek begins, set when a GET ultimately fails — so the caller can read it after a failed
// seek/read to get the real S3 reason instead of a generic I/O error.
thread_local std::string g_last_error;

// Network tuning for the range GETs.
constexpr long kConnectTimeoutSec = 10;   // fail fast if the endpoint is unreachable
constexpr long kLowSpeedBytes = 1;        // treat <1 B/s ...
constexpr long kLowSpeedTimeSec = 60;     // ... for 60s as a stall and abort (instead of hanging forever)
constexpr long kBackoffBaseMs = 100;      // exponential backoff base
constexpr long kBackoffCapMs = 2000;      // and ceiling, with full jitter

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? std::string(v) : fallback;
}

void ensure_curl_global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// RFC 3986 percent-encoding of a path, preserving '/' between segments (S3 does not double-encode).
std::string uri_encode_path(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back("0123456789ABCDEF"[c >> 4]);
            out.push_back("0123456789ABCDEF"[c & 0x0F]);
        }
    }
    return out;
}

std::string amz_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[20];
    std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm_utc);
    return std::string(buf);
}

// Resolved request target: the Host header value, the full request URL, and the canonical (encoded) path.
struct Endpoint {
    std::string host;
    std::string url;
    std::string path;
};

Endpoint build_endpoint(const s3detail::Config& cfg, const std::string& bucket, const std::string& key,
                        const std::string& region) {
    Endpoint e;
    if (cfg.path_style) {
        // endpoint like "http://localhost:9000" — strip scheme to get the host:port for the Host header.
        std::string ep = cfg.endpoint;
        std::string scheme = "https://";
        if (ep.rfind("http://", 0) == 0) {
            scheme = "http://";
            ep = ep.substr(7);
        } else if (ep.rfind("https://", 0) == 0) {
            ep = ep.substr(8);
        }
        while (!ep.empty() && ep.back() == '/') {
            ep.pop_back();
        }
        e.host = ep;
        e.path = uri_encode_path("/" + bucket + "/" + key);
        e.url = scheme + e.host + e.path;
    } else {
        e.host = bucket + ".s3." + region + ".amazonaws.com";
        e.path = uri_encode_path("/" + key);
        e.url = "https://" + e.host + e.path;
    }
    return e;
}

std::size_t write_to_vector(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t n = size * nmemb;
    auto* out = static_cast<std::vector<char>*>(userdata);
    out->insert(out->end(), ptr, ptr + n);
    return n;
}

// Capture the `x-amz-bucket-region` response header (S3 returns it on wrong-region redirects), so we can
// re-target and retry against the correct region.
std::size_t capture_region_header(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
    const std::size_t n = size * nitems;
    auto* region = static_cast<std::string*>(userdata);
    const std::string kKey = "x-amz-bucket-region:";
    std::string line(buffer, n);
    std::string prefix = line.substr(0, std::min(line.size(), kKey.size()));
    for (char& c : prefix) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (prefix == kKey) {
        std::string value = line.substr(kKey.size());
        const auto b = value.find_first_not_of(" \t");
        const auto e = value.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) {
            *region = value.substr(b, e - b + 1);
        }
    }
    return n;
}

bool is_retriable_curl(CURLcode rc) {
    switch (rc) {
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
        case CURLE_SSL_CONNECT_ERROR:
            return true;
        default:
            return false;
    }
}

// Full-jitter exponential backoff before retry `attempt` (1-based).
void sleep_backoff(int attempt) {
    long ceiling = kBackoffBaseMs;
    for (int i = 1; i < attempt && ceiling < kBackoffCapMs; ++i) {
        ceiling <<= 1;
    }
    ceiling = std::min(ceiling, kBackoffCapMs);
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<long> dist(0, ceiling);
    std::this_thread::sleep_for(std::chrono::milliseconds(dist(rng)));
}

// Seekable streambuf over S3 range GETs. Each underflow/refetch pulls one read-ahead window so consecutive
// slices within a window cost no extra GET (matching the access pattern in nano_lance_external_blob.cpp).
// The single curl handle is reused across windows so the connection stays keep-alive for the object.
class S3Streambuf : public std::streambuf {
public:
    S3Streambuf(std::shared_ptr<const s3detail::Config> cfg, std::string bucket, std::string key,
                std::size_t read_ahead)
        : cfg_(std::move(cfg)), bucket_(std::move(bucket)), key_(std::move(key)), region_(cfg_->region),
          read_ahead_(read_ahead == 0 ? 1 : read_ahead) {
        ensure_curl_global_init();
        curl_ = curl_easy_init();
        const Endpoint e = build_endpoint(*cfg_, bucket_, key_, region_);
        host_ = e.host;
        url_ = e.url;
        path_ = e.path;
    }

    ~S3Streambuf() override {
        if (curl_ != nullptr) {
            curl_easy_cleanup(curl_);
        }
    }

    bool ok() const { return curl_ != nullptr; }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (reached_eof_) {
            return traits_type::eof();
        }
        const std::uint64_t next = window_start_ + window_len_;
        if (!load_window(next)) {
            return traits_type::eof();
        }
        if (window_len_ == 0) {
            reached_eof_ = true;
            return traits_type::eof();
        }
        return traits_type::to_int_type(*gptr());
    }

    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override {
        if ((which & std::ios_base::in) == 0) {
            return pos_type(off_type(-1));
        }
        std::int64_t base = 0;
        if (dir == std::ios_base::beg) {
            base = 0;
        } else if (dir == std::ios_base::cur) {
            base = static_cast<std::int64_t>(cur_pos());
        } else {
            return pos_type(off_type(-1));  // SEEK_END: object size unknown; the caller only uses beg/cur
        }
        const std::int64_t target = base + static_cast<std::int64_t>(off);
        if (target < 0) {
            return pos_type(off_type(-1));
        }
        return seekpos(pos_type(static_cast<off_type>(target)), which);
    }

    pos_type seekpos(pos_type sp, std::ios_base::openmode which) override {
        if ((which & std::ios_base::in) == 0) {
            return pos_type(off_type(-1));
        }
        // The caller seeks before every read, so this is the start of a fresh logical fetch: drop any stale
        // error so a later error() reflects only this fetch.
        g_last_error.clear();
        const std::uint64_t target = static_cast<std::uint64_t>(static_cast<off_type>(sp));
        // Inside the live window? Just move the get pointer — no GET.
        if (window_len_ > 0 && target >= window_start_ && target <= window_start_ + window_len_) {
            char* base = window_.data();
            setg(base, base + (target - window_start_), base + window_len_);
            return sp;
        }
        if (!load_window(target)) {
            return pos_type(off_type(-1));
        }
        return sp;
    }

private:
    std::uint64_t cur_pos() const {
        return window_start_ + static_cast<std::uint64_t>(gptr() - eback());
    }

    // Position the get area after a successful body fetch. A 206 begins at `start`; a 200 means the server
    // ignored Range and returned the whole object from offset 0 — place the get pointer at the requested
    // logical offset within it so reads still land correctly.
    void install_window(std::uint64_t start, bool partial) {
        char* base = window_.data();
        if (partial) {  // 206 Partial Content
            window_start_ = start;
            reached_eof_ = window_.size() < read_ahead_;  // a short window means the object ended here
        } else {  // 200 OK: full object from offset 0
            window_start_ = 0;
            reached_eof_ = true;
        }
        window_len_ = window_.size();
        std::uint64_t goff = (start >= window_start_) ? (start - window_start_) : 0;
        if (goff > window_len_) {
            goff = window_len_;
        }
        setg(base, base + goff, base + window_len_);
    }

    void fail(const std::string& msg) {
        last_error_ = msg;
        g_last_error = msg;
        window_len_ = 0;
        setg(nullptr, nullptr, nullptr);
    }

    // Fetch [start, start+read_ahead-1] into the window, with retry/backoff and one-shot region redirect.
    // Returns false only on a real transport/HTTP error (sets the thread error); an empty/short read at
    // end-of-object is a success that flags reached_eof_.
    bool load_window(std::uint64_t start) {
        if (curl_ == nullptr) {
            fail("curl init failed");
            return false;
        }
        bool redirected = false;
        int attempt = 0;
        for (;;) {
            window_.clear();
            window_.reserve(read_ahead_);
            region_header_.clear();

            const std::uint64_t last = start + read_ahead_ - 1;
            const std::string range = "bytes=" + std::to_string(start) + "-" + std::to_string(last);
            const std::string amz_date = amz_now();  // fresh per attempt: backoff must not skew the signature

            std::vector<s3v4::Header> sign_headers = {
                {"host", host_},
                {"range", range},
                {"x-amz-content-sha256", "UNSIGNED-PAYLOAD"},
                {"x-amz-date", amz_date},
            };
            if (!cfg_->session_token.empty()) {
                sign_headers.push_back({"x-amz-security-token", cfg_->session_token});
            }
            s3v4::CanonicalRequest req;
            req.method = "GET";
            req.uri = path_;
            req.query = "";
            req.headers = sign_headers;
            req.payload_hash = "UNSIGNED-PAYLOAD";
            const std::string authz = s3v4::build_authorization(req, cfg_->access_key, cfg_->secret_key,
                                                                region_, "s3", amz_date);

            curl_slist* hdrs = nullptr;
            hdrs = curl_slist_append(hdrs, ("x-amz-date: " + amz_date).c_str());
            hdrs = curl_slist_append(hdrs, "x-amz-content-sha256: UNSIGNED-PAYLOAD");
            hdrs = curl_slist_append(hdrs, ("Range: " + range).c_str());
            if (!cfg_->session_token.empty()) {
                hdrs = curl_slist_append(hdrs, ("x-amz-security-token: " + cfg_->session_token).c_str());
            }
            hdrs = curl_slist_append(hdrs, ("Authorization: " + authz).c_str());

            curl_easy_reset(curl_);  // preserves the live connection + DNS/TLS caches (keep-alive)
            curl_easy_setopt(curl_, CURLOPT_URL, url_.c_str());
            curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
            curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hdrs);
            curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &write_to_vector);
            curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &window_);
            curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, &capture_region_header);
            curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &region_header_);
            curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
            curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytes);
            curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, kLowSpeedTimeSec);

            const CURLcode rc = curl_easy_perform(curl_);
            long code = 0;
            curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &code);
            curl_slist_free_all(hdrs);

            if (rc == CURLE_OK && (code == 200 || code == 206)) {
                install_window(start, code == 206);
                g_last_error.clear();
                return true;
            }
            if (rc == CURLE_OK && code == 416) {  // Range Not Satisfiable: at/over the end of the object.
                window_start_ = start;
                window_len_ = 0;
                reached_eof_ = true;
                setg(nullptr, nullptr, nullptr);
                g_last_error.clear();
                return true;
            }

            // Wrong-region redirect (virtual-hosted only): re-target to the region S3 reported and retry
            // immediately (once), without consuming a backoff attempt.
            if (rc == CURLE_OK && !cfg_->path_style && (code == 301 || code == 307 || code == 400) &&
                !region_header_.empty() && region_header_ != region_ && !redirected) {
                region_ = region_header_;
                const Endpoint e = build_endpoint(*cfg_, bucket_, key_, region_);
                host_ = e.host;
                url_ = e.url;
                path_ = e.path;
                redirected = true;
                continue;
            }

            const bool http_retriable =
                rc == CURLE_OK && (code == 500 || code == 502 || code == 503 || code == 504 || code == 429);
            const bool retriable = is_retriable_curl(rc) || http_retriable;
            ++attempt;
            if (retriable && attempt < cfg_->max_attempts) {
                sleep_backoff(attempt);
                continue;
            }

            if (rc != CURLE_OK) {
                fail(std::string("S3 GET transport error: ") + curl_easy_strerror(rc));
            } else {
                std::string body(window_.begin(), window_.end());
                if (body.size() > 512) {
                    body.resize(512);
                }
                fail("S3 GET HTTP " + std::to_string(code) + ": " + body);
            }
            return false;
        }
    }

    std::shared_ptr<const s3detail::Config> cfg_;
    std::string bucket_;
    std::string key_;
    std::string region_;  // effective region (may change once on a wrong-region redirect)
    std::string host_;
    std::string url_;
    std::string path_;  // percent-encoded canonical path
    std::size_t read_ahead_;
    CURL* curl_ = nullptr;
    std::string region_header_;  // x-amz-bucket-region captured from the last response

    std::vector<char> window_;
    std::uint64_t window_start_ = 0;
    std::size_t window_len_ = 0;
    bool reached_eof_ = false;
    std::string last_error_;
};

// istream that owns its streambuf so the unique_ptr<istream> the factory returns keeps the buffer alive.
class S3IStream : public std::istream {
public:
    explicit S3IStream(std::unique_ptr<S3Streambuf> buf) : std::istream(buf.get()), buf_(std::move(buf)) {}

private:
    std::unique_ptr<S3Streambuf> buf_;
};

}  // namespace

S3MinStreamFactory::S3MinStreamFactory() {
    auto cfg = std::make_shared<s3detail::Config>();
    cfg->access_key = env_or("AWS_ACCESS_KEY_ID", "");
    cfg->secret_key = env_or("AWS_SECRET_ACCESS_KEY", "");
    cfg->session_token = env_or("AWS_SESSION_TOKEN", "");
    cfg->region = env_or("AWS_REGION", env_or("AWS_DEFAULT_REGION", "us-east-1"));
    cfg->endpoint = env_or("AWS_ENDPOINT_URL", "");
    cfg->path_style = !cfg->endpoint.empty();
    const int attempts = std::atoi(env_or("AWS_MAX_ATTEMPTS", "3").c_str());
    cfg->max_attempts = attempts >= 1 ? attempts : 3;
    config_ = cfg;
}

std::string S3MinStreamFactory::error() const { return g_last_error; }

std::unique_ptr<std::istream> S3MinStreamFactory::open(const std::string& uri, std::size_t read_ahead_bytes) {
    g_last_error.clear();
    if (uri.rfind("s3://", 0) != 0) {
        g_last_error = "not an s3:// URI";
        return nullptr;
    }
    const std::string rest = uri.substr(5);  // strip "s3://"
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
        g_last_error = "malformed s3:// URI (expected s3://bucket/key)";
        return nullptr;
    }
    const std::string bucket = rest.substr(0, slash);
    const std::string key = rest.substr(slash + 1);

    if (config_->access_key.empty() || config_->secret_key.empty()) {
        g_last_error = "missing AWS credentials (set AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY)";
        return nullptr;
    }

    auto buf = std::make_unique<S3Streambuf>(config_, bucket, key, read_ahead_bytes);
    if (!buf->ok()) {
        g_last_error = "failed to initialize libcurl handle";
        return nullptr;
    }
    return std::make_unique<S3IStream>(std::move(buf));
}

}  // namespace nanolance
