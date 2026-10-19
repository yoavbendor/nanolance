// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
#include "nanolance/byte_source.hpp"

#include "nanolance/object_store.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>

#if defined(NANO_LANCE_HAS_S3_BYTE_SOURCE)
#include "s3_byte_source.hpp"
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nano_lance::io {
namespace {

#if defined(_WIN32)
class LocalFile final : public ByteSource {
public:
    LocalFile(HANDLE h, std::uint64_t size, std::int64_t mtime) : h_(h), size_(size), mtime_(mtime) {}
    ~LocalFile() override { CloseHandle(h_); }
    std::uint64_t size() const override { return size_; }
    std::int64_t version_token() const override { return mtime_; }
    bool read_at(std::uint64_t offset, std::uint64_t n, std::uint8_t* dst, std::string& error) override {
        while (n > 0) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32U);
            const DWORD want = static_cast<DWORD>(n > (1ULL << 30U) ? (1ULL << 30U) : n);
            DWORD got = 0;
            if (!ReadFile(h_, dst, want, &got, &ov) || got == 0) {
                error = "failed to read data file byte range";
                return false;
            }
            offset += got;
            dst += got;
            n -= got;
        }
        return true;
    }

private:
    HANDLE h_;
    std::uint64_t size_;
    std::int64_t mtime_;
};
#else
class LocalFile final : public ByteSource {
public:
    LocalFile(int fd, std::uint64_t size, std::int64_t mtime) : fd_(fd), size_(size), mtime_(mtime) {}
    ~LocalFile() override { ::close(fd_); }
    std::uint64_t size() const override { return size_; }
    std::int64_t version_token() const override { return mtime_; }
    bool read_at(std::uint64_t offset, std::uint64_t n, std::uint8_t* dst, std::string& error) override {
        // pread keeps no file position, so no seek is ever needed and pages read in any order cost one
        // system call each; a large read may come back in pieces.
        while (n > 0) {
            const auto want = static_cast<std::size_t>(n > (1ULL << 30U) ? (1ULL << 30U) : n);
            const auto got = ::pread(fd_, dst, want, static_cast<off_t>(offset));
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got <= 0) {
                error = "failed to read data file byte range";
                return false;
            }
            offset += static_cast<std::uint64_t>(got);
            dst += got;
            n -= static_cast<std::uint64_t>(got);
        }
        return true;
    }

private:
    int fd_;
    std::uint64_t size_;
    std::int64_t mtime_;
};
#endif

std::mutex& factories_mutex() {
    static std::mutex m;
    return m;
}
// The built-in object stores are registered when the registry is first made. Doing it in the static
// initializer (not through register_*, which takes the lock this runs under) keeps that to one place.
std::map<std::string, SourceFactory>& factories() {
    static std::map<std::string, SourceFactory> f = [] {
        std::map<std::string, SourceFactory> built_in;
#if defined(NANO_LANCE_HAS_S3_BYTE_SOURCE)
        built_in.emplace("s3", &open_s3_source);
#endif
        return built_in;
    }();
    return f;
}

std::map<std::string, ListFactory>& listers() {
    static std::map<std::string, ListFactory> l = [] {
        std::map<std::string, ListFactory> built_in;
#if defined(NANO_LANCE_HAS_S3_BYTE_SOURCE)
        built_in.emplace("s3", &list_s3);
#endif
        return built_in;
    }();
    return l;
}

/// An s3:// URI in a build without S3 support: named, so the caller can tell it from a missing file.
bool is_unbuilt_s3(const std::string& path_or_uri) {
    return path_or_uri.rfind("s3://", 0) == 0 && !is_remote(path_or_uri);
}

}  // namespace

std::unique_ptr<ByteSource> open_local_file(const std::string& path, std::string& error) {
#if defined(_WIN32)
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wide_len > 0 ? wide_len : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wide_len);
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = "failed to open data file for read";
        return nullptr;
    }
    LARGE_INTEGER size{};
    FILETIME written{};
    if (!GetFileSizeEx(h, &size) || !GetFileTime(h, nullptr, nullptr, &written)) {
        CloseHandle(h);
        error = "failed to stat data file";
        return nullptr;
    }
    const auto mtime = static_cast<std::int64_t>((static_cast<std::uint64_t>(written.dwHighDateTime) << 32U) |
                                                 written.dwLowDateTime);
    return std::make_unique<LocalFile>(h, static_cast<std::uint64_t>(size.QuadPart), mtime);
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        error = "failed to open data file for read";
        return nullptr;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        error = "failed to stat data file";
        return nullptr;
    }
#if defined(__APPLE__)
    const std::int64_t mtime = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    const std::int64_t mtime = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    return std::make_unique<LocalFile>(fd, static_cast<std::uint64_t>(st.st_size), mtime);
#endif
}

bool stat_local(const std::string& path, std::uint64_t& size, std::int64_t& version) {
#if defined(_WIN32)
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wide_len > 0 ? wide_len : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wide_len);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &a)) {
        return false;
    }
    size = (static_cast<std::uint64_t>(a.nFileSizeHigh) << 32U) | a.nFileSizeLow;
    version = static_cast<std::int64_t>((static_cast<std::uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32U) |
                                        a.ftLastWriteTime.dwLowDateTime);
    return true;
#else
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
    version = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    version = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    return true;
#endif
}

bool is_remote(const std::string& path_or_uri) {
    const auto colon = path_or_uri.find("://");
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(factories_mutex());
    return factories().count(path_or_uri.substr(0, colon)) != 0U;
}

void register_source_factory(const std::string& scheme, SourceFactory factory) {
    std::lock_guard<std::mutex> lock(factories_mutex());
    if (factory) {
        factories()[scheme] = std::move(factory);
    } else {
        factories().erase(scheme);
    }
}

std::unique_ptr<ByteSource> open_source(const std::string& path_or_uri, std::string& error) {
    const auto colon = path_or_uri.find("://");
    if (colon != std::string::npos && colon > 0) {
        SourceFactory factory;
        {
            std::lock_guard<std::mutex> lock(factories_mutex());
            const auto it = factories().find(path_or_uri.substr(0, colon));
            if (it != factories().end()) {
                factory = it->second;
            }
        }
        if (factory) {
            return factory(path_or_uri, error);
        }
        if (is_unbuilt_s3(path_or_uri)) {
            error = "nanolance was built without S3 support: cannot open " + path_or_uri;
            return nullptr;
        }
    }
    return open_local_file(path_or_uri, error);
}

bool read_all(const std::string& path_or_uri, std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    const auto source = open_source(path_or_uri, error);
    if (!source) {
        return false;
    }
    out.resize(static_cast<std::size_t>(source->size()));
    if (!out.empty() && !source->read_at(0, out.size(), out.data(), error)) {
        out.clear();
        return false;
    }
    return true;
}

bool exists(const std::string& path_or_uri) {
    if (!is_remote(path_or_uri)) {
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::path(path_or_uri), ec);
    }
    std::string ignored;
    return open_source(path_or_uri, ignored) != nullptr;
}

void register_list_factory(const std::string& scheme, ListFactory factory) {
    std::lock_guard<std::mutex> lock(factories_mutex());
    if (factory) {
        listers()[scheme] = std::move(factory);
    } else {
        listers().erase(scheme);
    }
}

ObjectStoreOptions object_store_options_from(const std::vector<std::pair<std::string, std::string>>& pairs) {
    ObjectStoreOptions o;
    for (const auto& [key, value] : pairs) {
        if (key == "access_key_id" || key == "aws_access_key_id") {
            o.access_key_id = value;
        } else if (key == "secret_access_key" || key == "aws_secret_access_key") {
            o.secret_access_key = value;
        } else if (key == "session_token" || key == "aws_session_token") {
            o.session_token = value;
        } else if (key == "region" || key == "aws_region") {
            o.region = value;
        } else if (key == "endpoint" || key == "aws_endpoint") {
            o.endpoint = value;
        } else if (key == "allow_http") {
            o.allow_http = value == "true" || value == "True" || value == "1";
        }
    }
    return o;
}

#if !defined(NANO_LANCE_HAS_S3_BYTE_SOURCE)
bool set_object_store_options(const std::string& uri, const ObjectStoreOptions& /*options*/, std::string& error) {
    if (uri.rfind("s3://", 0) == 0) {
        error = "nanolance was built without S3 support: cannot set storage options for " + uri;
        return false;
    }
    return true;
}
#endif

bool list_dir(const std::string& path_or_uri, std::vector<DirEntry>& out, std::string& error, bool with_sizes) {
    out.clear();
    error.clear();
    const auto colon = path_or_uri.find("://");
    if (colon != std::string::npos && colon > 0) {
        const auto scheme = path_or_uri.substr(0, colon);
        ListFactory lister;
        bool has_source = false;
        {
            std::lock_guard<std::mutex> lock(factories_mutex());
            const auto it = listers().find(scheme);
            if (it != listers().end()) {
                lister = it->second;
            }
            has_source = factories().count(scheme) != 0U;
        }
        if (lister) {
            return lister(path_or_uri, out, error);
        }
        if (has_source) {
            error = "cannot list " + path_or_uri + ": no directory listing is registered for " + scheme + "://";
            return false;
        }
        if (is_unbuilt_s3(path_or_uri)) {
            error = "nanolance was built without S3 support: cannot list " + path_or_uri;
            return false;
        }
    }
    const std::filesystem::path dir(path_or_uri);
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return true;  // a directory that is not there lists as empty
        }
        error = "failed to list " + path_or_uri + ": " + ec.message();
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            error = "failed to list " + path_or_uri + ": " + ec.message();
            out.clear();
            return false;
        }
        DirEntry entry;
        entry.name = it->path().filename().string();
        std::error_code entry_ec;
        entry.is_dir = it->is_directory(entry_ec);
        if (with_sizes && !entry.is_dir) {
            entry.size = static_cast<std::uint64_t>(it->file_size(entry_ec));
        }
        out.push_back(std::move(entry));
    }
    return true;
}

}  // namespace nano_lance::io
