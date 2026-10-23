// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/byte_sink.hpp"
#include "nanolance/data_file_reader.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <utility>

#if defined(NANO_LANCE_HAS_S3_BYTE_SOURCE)
#include "s3_byte_sink.hpp"
#endif

namespace nano_lance::io {
namespace {

std::mutex& backends_mutex() {
    static std::mutex m;
    return m;
}

std::map<std::string, SinkBackend>& backends() {
    static std::map<std::string, SinkBackend> b = [] {
        std::map<std::string, SinkBackend> built_in;
#if defined(NANO_LANCE_HAS_S3_BYTE_SOURCE)
        built_in.emplace("s3", s3_sink_backend());
#endif
        return built_in;
    }();
    return b;
}

/// The backend for `path_or_uri`'s scheme, or none (a local path).
bool backend_for(const std::string& path_or_uri, SinkBackend& out) {
    const auto colon = path_or_uri.find("://");
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(backends_mutex());
    const auto it = backends().find(path_or_uri.substr(0, colon));
    if (it == backends().end()) {
        return false;
    }
    out = it->second;
    return true;
}

/// A local file, written in place (as nanolance always wrote them).
class LocalOutput final : public OutputFile {
public:
    LocalOutput() : OutputFile(&buf_) {}
    bool open(const std::filesystem::path& path) {
        path_ = path;
        return buf_.open(path, std::ios::binary | std::ios::out | std::ios::trunc) != nullptr;
    }
    bool commit(std::string& error) override {
        flush();
        const bool ok = static_cast<bool>(*this) && buf_.close() != nullptr;
        if (!ok) {
            error = "failed to write " + path_.string();
        }
        return ok;
    }

private:
    std::filebuf buf_;
    std::filesystem::path path_;
};

}  // namespace

std::unique_ptr<OutputFile> open_output(const std::string& path_or_uri, std::string& error) {
    SinkBackend backend;
    if (backend_for(path_or_uri, backend)) {
        return backend.open(path_or_uri, error);
    }
    if (path_or_uri.find("://") != std::string::npos) {
        // Written as a local path, a URI would become a directory named after its scheme.
        error = "writes to " + path_or_uri.substr(0, path_or_uri.find("://")) +
                ":// are not supported (nanolance writes local paths and s3:// URIs): " + path_or_uri;
        return nullptr;
    }
    const std::filesystem::path path(path_or_uri);
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "failed to create " + path.parent_path().string() + ": " + ec.message();
            return nullptr;
        }
    }
    auto out = std::make_unique<LocalOutput>();
    if (!out->open(path)) {
        error = "failed to open " + path.string() + " for writing";
        return nullptr;
    }
    return out;
}

bool put_if_absent(const std::string& path_or_uri, const std::vector<std::uint8_t>& bytes, bool& already_exists,
                   std::string& error) {
    already_exists = false;
    error.clear();
    SinkBackend backend;
    if (backend_for(path_or_uri, backend)) {
        return backend.put_if_absent(path_or_uri, bytes, already_exists, error);
    }
    const std::filesystem::path final_path(path_or_uri);
    std::error_code ec;
    std::filesystem::create_directories(final_path.parent_path(), ec);
    if (ec) {
        error = "failed to create " + final_path.parent_path().string() + ": " + ec.message();
        return false;
    }
    // The temp name is the writer's own: two writers racing for one name must not share it, or one's
    // link could publish the other's bytes (and report success for a change it lost).
    static std::atomic<std::uint64_t> counter{0};
    const auto unique = std::to_string(std::random_device{}()) + "-" + std::to_string(counter.fetch_add(1U));
    const auto temp_path = final_path.parent_path() / (final_path.filename().string() + "." + unique + ".tmp");
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "failed to open " + temp_path.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(temp_path, ec);
            error = "failed to write " + temp_path.string();
            return false;
        }
    }
    // Publish without replacing: a hard link fails if the name exists, so what another writer published
    // meanwhile is never overwritten (rename would silently replace it). File systems without hard links
    // fall back to rename after an existence check.
    std::filesystem::create_hard_link(temp_path, final_path, ec);
    if (!ec) {
        std::filesystem::remove(temp_path, ec);
        return true;
    }
    if (std::filesystem::exists(final_path)) {
        std::filesystem::remove(temp_path, ec);
        already_exists = true;
        return false;
    }
    ec.clear();
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        error = "failed to publish " + final_path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool remove_output(const std::string& path_or_uri, std::string& error) {
    error.clear();
    SinkBackend backend;
    if (backend_for(path_or_uri, backend)) {
        return backend.remove(path_or_uri, error);
    }
    std::error_code ec;
    std::filesystem::remove(path_or_uri, ec);
    close_data_files_under(path_or_uri);
    if (ec) {
        error = "failed to remove " + path_or_uri + ": " + ec.message();
        return false;
    }
    return true;
}

bool is_object_store_output(const std::string& path_or_uri) {
    SinkBackend backend;
    return backend_for(path_or_uri, backend);
}

void register_sink_backend(const std::string& scheme, SinkBackend backend) {
    std::lock_guard<std::mutex> lock(backends_mutex());
    if (backend.open) {
        backends()[scheme] = std::move(backend);
    } else {
        backends().erase(scheme);
    }
}

}  // namespace nano_lance::io
