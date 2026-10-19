// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
#include "s3_byte_source.hpp"

#include "nanolance/object_store.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include "nanos3reader/s3_reader.h"

namespace nano_lance::io {
namespace {

// Small reads share one block of this size, so a Lance page's small reads become one request.
constexpr std::uint64_t kSmallReadBlock = 1024U * 1024U;
// The footer reader fetches the file's tail (footer, descriptor, column metadata) in one request.
constexpr std::uint64_t kFooterReadSize = 64U * 1024U;

constexpr const char* kScheme = "s3://";

std::mutex& store_mutex() {
    static std::mutex m;
    return m;
}

/// Per-bucket options, as the last open set them.
std::map<std::string, ObjectStoreOptions>& bucket_options() {
    static std::map<std::string, ObjectStoreOptions> m;
    return m;
}

/// One S3Client per distinct set of options (the clients are thread-safe and keep their connections).
std::map<std::string, std::shared_ptr<nanos3reader::S3Client>>& clients() {
    static std::map<std::string, std::shared_ptr<nanos3reader::S3Client>> m;
    return m;
}

/// The bucket of `s3://bucket/key`; false when `uri` is not an s3:// URI or has no bucket.
bool bucket_of(const std::string& uri, std::string& bucket) {
    if (uri.rfind(kScheme, 0) != 0) {
        return false;
    }
    const auto start = std::string(kScheme).size();
    const auto slash = uri.find('/', start);
    bucket = uri.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    return !bucket.empty();
}

bool is_default(const ObjectStoreOptions& o) {
    return o.access_key_id.empty() && o.secret_access_key.empty() && o.session_token.empty() && o.region.empty() &&
           o.endpoint.empty() && !o.allow_http;
}

/// A key that tells distinct option sets apart (unit separators keep the fields from running together).
std::string client_key(const ObjectStoreOptions& o) {
    std::string key;
    for (const std::string* field : {&o.access_key_id, &o.secret_access_key, &o.session_token, &o.region, &o.endpoint}) {
        key += *field;
        key += '\x1f';
    }
    key += o.allow_http ? '1' : '0';
    return key;
}

/// The client for `bucket`'s current options, created on first use.
std::shared_ptr<nanos3reader::S3Client> client_for_bucket(const std::string& bucket) {
    std::lock_guard<std::mutex> lock(store_mutex());
    ObjectStoreOptions options;
    const auto it = bucket_options().find(bucket);
    if (it != bucket_options().end()) {
        options = it->second;
    }
    auto& slot = clients()[client_key(options)];
    if (!slot) {
        nanos3reader::ClientOptions client_options;
        client_options.access_key_id = options.access_key_id;
        client_options.secret_access_key = options.secret_access_key;
        client_options.session_token = options.session_token;
        client_options.region = options.region;
        client_options.endpoint = options.endpoint;
        client_options.allow_http = options.allow_http;
        slot = std::make_shared<nanos3reader::S3Client>(std::move(client_options));
    }
    return slot;
}

class S3ByteSource final : public ByteSource {
public:
    S3ByteSource(std::shared_ptr<nanos3reader::S3Client> client, std::string uri, std::uint64_t size)
        : client_(std::move(client)), uri_(std::move(uri)), size_(size) {}
    std::uint64_t size() const override { return size_; }
    bool read_at(std::uint64_t offset, std::uint64_t n, std::uint8_t* dst, std::string& error) override {
        return client_->read_range(uri_, offset, n, dst, error);
    }
    std::uint64_t small_read_block() const override { return kSmallReadBlock; }
    std::uint64_t footer_read_size() const override { return kFooterReadSize; }

private:
    std::shared_ptr<nanos3reader::S3Client> client_;
    std::string uri_;
    std::uint64_t size_;
};

}  // namespace

std::unique_ptr<ByteSource> open_s3_source(const std::string& uri, std::string& error) {
    std::string bucket;
    if (!bucket_of(uri, bucket)) {
        error = "not an s3:// URI with a bucket: " + uri;
        return nullptr;
    }
    const auto client = client_for_bucket(bucket);
    std::uint64_t size = 0;
    if (!client->head(uri, size, error)) {
        return nullptr;
    }
    return std::make_unique<S3ByteSource>(client, uri, size);
}

bool list_s3(const std::string& dir_uri, std::vector<DirEntry>& out, std::string& error) {
    out.clear();
    std::string bucket;
    if (!bucket_of(dir_uri, bucket)) {
        error = "not an s3:// URI with a bucket: " + dir_uri;
        return false;
    }
    // ListObjectsV2 wants the prefix to end in "/" (an empty key part lists the bucket).
    std::string prefix_uri = dir_uri;
    if (prefix_uri.back() != '/') {
        prefix_uri += '/';
    }
    const std::string key_prefix = prefix_uri.substr(std::string(kScheme).size() + bucket.size() + 1);

    std::vector<nanos3reader::ObjectInfo> objects;
    std::vector<std::string> common_prefixes;
    if (!client_for_bucket(bucket)->list(prefix_uri, "/", objects, common_prefixes, error)) {
        return false;
    }
    for (const auto& object : objects) {
        if (object.key.size() <= key_prefix.size()) {
            continue;  // the "directory" object itself, if the store has one
        }
        out.push_back(DirEntry{object.key.substr(key_prefix.size()), object.size, false});
    }
    for (const auto& sub : common_prefixes) {
        auto name = sub.substr(key_prefix.size());
        while (!name.empty() && name.back() == '/') {
            name.pop_back();
        }
        if (!name.empty()) {
            out.push_back(DirEntry{name, 0, true});
        }
    }
    return true;
}

bool set_object_store_options(const std::string& uri, const ObjectStoreOptions& options, std::string& error) {
    std::string bucket;
    if (!bucket_of(uri, bucket)) {
        error = "not an s3:// URI with a bucket: " + uri;
        return false;
    }
    std::lock_guard<std::mutex> lock(store_mutex());
    if (is_default(options)) {
        bucket_options().erase(bucket);
    } else {
        bucket_options()[bucket] = options;
    }
    return true;
}

}  // namespace nano_lance::io
