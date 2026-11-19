// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "io/s3_byte_sink.hpp"

#include "nanolance/work_stats.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <random>
#include <streambuf>
#include <vector>

#include "nanos3reader/s3_range.h"
#include "nanos3reader/s3_reader.h"

namespace nano_lance::io {
namespace {

// Objects up to this size are uploaded from memory in one PUT; a larger one spills to a temporary file
// and goes up with put_file (multipart from 64 MiB), so memory stays bounded whatever is written.
constexpr std::size_t kMemoryLimit = 8U * 1024U * 1024U;
// Parts of one multipart upload in flight (16 MiB each). Measured on moto against pylance writing 512 MiB:
// 4 in flight 0.71x its speed, 8 0.91x, 16 0.98x; 8 keeps the memory in flight at 128 MiB (Lance uploads
// about 10 parts at once).
constexpr unsigned kPartsInFlight = 8U;

std::filesystem::path temp_dir() {
    if (const char* dir = std::getenv("NANOLANCE_TMPDIR"); dir != nullptr && *dir != '\0') {
        return dir;
    }
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    return ec ? std::filesystem::path("/tmp") : dir;
}

// The sink's output: nanos3reader's SpillingObjectWriter (memory up to kMemoryLimit, then a temporary file; the
// object is published by one PUT or a multipart upload of the file, on commit()).
class S3Output final : public OutputFile {
public:
    S3Output(std::unique_ptr<nanos3reader::SpillingObjectWriter> writer, std::string uri)
        : OutputFile(writer->rdbuf()), writer_(std::move(writer)), uri_(std::move(uri)) {}
    bool commit(std::string& error) override {
        flush();
        if (!*this) {
            error = "failed to write " + uri_;
            return false;  // the writer's destructor publishes nothing
        }
        const std::uint64_t bytes = writer_->bytes_written();
        if (!writer_->commit(error)) {
            return false;
        }
        work_stats::count_write(bytes);  // the local tally stats a file, which a store has not
        return true;
    }

private:
    std::unique_ptr<nanos3reader::SpillingObjectWriter> writer_;
    std::string uri_;
};

/// Whether `bucket` honours create-only PUTs (If-None-Match: *), probed once per process: a probe object
/// next to the manifest is created, created again (which must fail with 412), and removed. A store that
/// accepts the second PUT would let two writers both publish one version, so every commit to it is
/// refused. (A read-back of the published manifest cannot tell: a later overwrite lands after it.)
bool conditional_puts_honoured(nanos3reader::S3Client& client, const std::string& manifest_uri, std::string& error) {
    static std::mutex mutex;
    static std::map<std::string, std::string> verdicts;  // bucket -> "" (honoured) or the refusal
    const auto bucket_end = manifest_uri.find('/', 5);
    const std::string bucket = manifest_uri.substr(0, bucket_end);
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = verdicts.find(bucket);
        if (it != verdicts.end()) {
            error = it->second;
            return error.empty();
        }
    }
    const auto slash = manifest_uri.rfind('/');
    static std::atomic<std::uint64_t> counter{0};
    const std::string probe = manifest_uri.substr(0, slash + 1) + ".nanolance-conditional-put-probe-" +
                              std::to_string(std::random_device{}()) + "-" + std::to_string(counter.fetch_add(1U));
    nanos3reader::PutOptions create_only;
    create_only.if_none_match = true;
    const std::uint8_t byte = 0;
    std::string put_error;
    bool first_existed = false;  // a lost reply, retried: the probe's random name can only be ours
    if (!client.put(probe, &byte, 1, create_only, nullptr, put_error, &first_existed) && !first_existed) {
        error = "cannot write to " + probe + ": " + put_error;
        return false;  // not a verdict on the store: the next commit probes again
    }
    bool precondition_failed = false;
    const bool second = client.put(probe, &byte, 1, create_only, nullptr, put_error, &precondition_failed);
    std::string ignored;
    client.remove(probe, ignored);
    if (!second && !precondition_failed) {
        error = "cannot probe conditional writes at " + probe + ": " + put_error;
        return false;
    }
    const std::string verdict =
        precondition_failed ? std::string()
                            : "the object store at " + bucket +
                                  " does not honour conditional writes (a create-only PUT with If-None-Match "
                                  "replaced an existing object): concurrent commits are unsafe on it, so nanolance "
                                  "does not commit there";
    std::lock_guard<std::mutex> lock(mutex);
    verdicts[bucket] = verdict;
    error = verdict;
    return verdict.empty();
}

}  // namespace

SinkBackend s3_sink_backend() {
    SinkBackend b;
    b.open = [](const std::string& uri, std::string& error) -> std::unique_ptr<OutputFile> {
        auto client = s3_client_for_uri(uri, error);
        if (!client) {
            return nullptr;
        }
        nanos3reader::S3WriterOptions options;
        options.upload.part_concurrency = kPartsInFlight;
        options.spill.memory_limit = kMemoryLimit;
        options.spill.temp_dir = temp_dir();
        auto writer = nanos3reader::open_s3_writer(client, uri, options);
        if (!writer) {
            error = "not an s3:// URI with a bucket: " + uri;
            return nullptr;
        }
        return std::make_unique<S3Output>(std::move(writer), uri);
    };
    b.put_if_absent = [](const std::string& uri, const std::vector<std::uint8_t>& bytes, bool& already_exists,
                         std::string& error) {
        already_exists = false;
        auto client = s3_client_for_uri(uri, error);
        if (!client || !conditional_puts_honoured(*client, uri, error)) {
            return false;
        }
        nanos3reader::PutOptions options;
        options.if_none_match = true;
        bool precondition_failed = false;
        const bool ok = client->put(uri, bytes.data(), bytes.size(), options, nullptr, error, &precondition_failed);
        already_exists = !ok && precondition_failed;
        if (ok) {
            work_stats::count_write(bytes.size());
        }
        return ok;
    };
    b.remove = [](const std::string& uri, std::string& error) {
        auto client = s3_client_for_uri(uri, error);
        return client && client->remove(uri, error);
    };
    return b;
}

}  // namespace nano_lance::io
