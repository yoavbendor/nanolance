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

/// Bytes written so far, in memory until kMemoryLimit, then in a temporary file.
class SpillBuffer final : public std::streambuf {
public:
    ~SpillBuffer() override { discard(); }
    SpillBuffer() = default;
    SpillBuffer(const SpillBuffer&) = delete;
    SpillBuffer& operator=(const SpillBuffer&) = delete;

    std::uint64_t written() const { return written_; }
    bool spilled() const { return !file_path_.empty(); }  // also after finish() closed the file
    const std::vector<char>& memory() const { return memory_; }
    const std::filesystem::path& file_path() const { return file_path_; }
    bool failed() const { return failed_; }
    const std::string& failure() const { return failure_; }

    /// Closes the temporary file (if any) so it can be uploaded.
    bool finish() {
        if (file_ != nullptr && std::fclose(file_) != 0) {
            fail("failed to write the temporary file " + file_path_.string());
        }
        file_ = nullptr;
        return !failed_;
    }
    void discard() {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
        if (!file_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(file_path_, ec);
            file_path_.clear();
        }
    }

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (failed_) {
            return 0;
        }
        const auto size = static_cast<std::size_t>(n);
        if (file_ == nullptr && memory_.size() + size > kMemoryLimit && !spill()) {
            return 0;
        }
        if (file_ != nullptr) {
            if (std::fwrite(s, 1, size, file_) != size) {
                fail("failed to write the temporary file " + file_path_.string());
                return 0;
            }
        } else {
            memory_.insert(memory_.end(), s, s + size);
        }
        written_ += size;
        return n;
    }
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }
        const char c = traits_type::to_char_type(ch);
        return xsputn(&c, 1) == 1 ? ch : traits_type::eof();
    }
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override {
        // tellp() only: the writers never seek back.
        if (off == 0 && dir == std::ios_base::cur && (which & std::ios_base::out) != 0) {
            return pos_type(static_cast<off_type>(written_));
        }
        return pos_type(off_type(-1));
    }

private:
    bool spill() {
        static std::atomic<std::uint64_t> counter{0};
        file_path_ = temp_dir() / ("nanolance-upload-" + std::to_string(std::random_device{}()) + "-" +
                                   std::to_string(counter.fetch_add(1U)) + ".tmp");
        file_ = std::fopen(file_path_.string().c_str(), "wb");
        if (file_ == nullptr) {
            fail("failed to create a temporary file for the upload: " + file_path_.string());
            return false;
        }
        if (!memory_.empty() && std::fwrite(memory_.data(), 1, memory_.size(), file_) != memory_.size()) {
            fail("failed to write the temporary file " + file_path_.string());
            return false;
        }
        std::vector<char>().swap(memory_);
        return true;
    }
    void fail(std::string why) {
        if (!failed_) {
            failed_ = true;
            failure_ = std::move(why);
        }
    }

    std::vector<char> memory_;
    std::FILE* file_ = nullptr;
    std::filesystem::path file_path_;
    std::uint64_t written_ = 0;
    bool failed_ = false;
    std::string failure_;
};

class S3Output final : public OutputFile {
public:
    S3Output(std::shared_ptr<nanos3reader::S3Client> client, std::string uri)
        : OutputFile(&buf_), client_(std::move(client)), uri_(std::move(uri)) {}
    bool commit(std::string& error) override {
        flush();
        if (!*this || buf_.failed() || !buf_.finish()) {
            error = buf_.failed() ? buf_.failure() : "failed to write " + uri_;
            buf_.discard();
            return false;
        }
        bool ok = false;
        if (buf_.spilled()) {
            nanos3reader::FileUploadOptions upload;
            upload.part_concurrency = kPartsInFlight;
            ok = client_->put_file(uri_, buf_.file_path().string(), upload, nullptr, error);
        } else {
            const auto& m = buf_.memory();
            ok = client_->put(uri_, reinterpret_cast<const std::uint8_t*>(m.data()), m.size(),
                              nanos3reader::PutOptions{}, nullptr, error);
        }
        if (ok) {
            work_stats::count_write(buf_.written());  // the local tally stats a file, which a store has not
        }
        buf_.discard();
        return ok;
    }

private:
    SpillBuffer buf_;
    std::shared_ptr<nanos3reader::S3Client> client_;
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
        return std::make_unique<S3Output>(std::move(client), uri);
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
