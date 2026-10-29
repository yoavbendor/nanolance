// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// nano_lance_external_blob_size, and the argument and end-of-object rules of nano_lance_fetch_external_blob,
// on file:// objects this test writes into a fresh directory. test_nano_lance_external_blob covers the
// basic fetch; this covers the size call, which nothing else calls, and the cases the fetch's own
// comments promise: a size of UINT64_MAX reads to the end (capped by the buffer), and NULL or empty
// arguments fail by status with a message.

#include "nanolance/nano_lance_reader.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {

void require(bool ok, const std::string& what) {
    if (!ok) {
        throw std::runtime_error(what);
    }
}

/// A fresh directory, removed when the test ends however it ends.
class ScratchDir {
  public:
    ScratchDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("nano_lance_external_blob_size_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        require(std::filesystem::create_directory(path_), "scratch directory already exists");
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    [[nodiscard]] std::filesystem::path path() const { return path_; }

  private:
    std::filesystem::path path_;
};

std::string file_uri(const std::filesystem::path& p) { return "file://" + p.string(); }

void write_object(const std::filesystem::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
    require(static_cast<bool>(out), "could not write " + p.string());
}

void test_size() {
    const ScratchDir scratch;
    const auto six = scratch.path() / "six.bin";
    write_object(six, "abcdef");
    const auto empty = scratch.path() / "empty.bin";
    write_object(empty, "");

    char err[256]{};
    std::uint64_t size = 12345;
    require(nano_lance_external_blob_size(file_uri(six).c_str(), &size, err, sizeof err) == NANO_LANCE_READER_OK,
            std::string("size of a 6-byte object: ") + err);
    require(size == 6, "size must be the object's length in bytes");

    require(nano_lance_external_blob_size(file_uri(empty).c_str(), &size, err, sizeof err) == NANO_LANCE_READER_OK,
            std::string("size of an empty object: ") + err);
    require(size == 0, "an empty object has size 0");

    // The size of an object is what a fetch of the whole object reads.
    std::uint8_t buf[16]{};
    size_t n = 0;
    require(nano_lance_fetch_external_blob(file_uri(six).c_str(), 0, UINT64_MAX, buf, sizeof buf, &n, err,
                                           sizeof err) == NANO_LANCE_READER_OK,
            std::string("fetch whole object: ") + err);
    require(n == 6 && std::string(reinterpret_cast<char*>(buf), n) == "abcdef",
            "a fetch to the end must return the whole object, and its size");

    // A missing object is an I/O failure with a message, and the size is not written.
    size = 777;
    const auto missing = scratch.path() / "missing.bin";
    const int rc = nano_lance_external_blob_size(file_uri(missing).c_str(), &size, err, sizeof err);
    require(rc != NANO_LANCE_READER_OK, "size of a missing object must fail");
    require(std::string(err).size() > 0, "a missing object's size failure must say why");
}

void test_size_argument_errors() {
    std::uint64_t size = 0;
    char err[128]{};
    require(nano_lance_external_blob_size(nullptr, &size, err, sizeof err) == NANO_LANCE_READER_INVALID_ARGUMENT,
            "NULL uri must be INVALID_ARGUMENT");
    require(nano_lance_external_blob_size("", &size, err, sizeof err) == NANO_LANCE_READER_INVALID_ARGUMENT,
            "empty uri must be INVALID_ARGUMENT");
    require(nano_lance_external_blob_size("file:///nonexistent", nullptr, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "NULL out_size must be INVALID_ARGUMENT");
    require(std::string(err).find("invalid argument") != std::string::npos,
            "an argument failure must say so in the message");

    // A NULL message buffer is allowed: the status still comes back.
    require(nano_lance_external_blob_size(nullptr, &size, nullptr, 0) == NANO_LANCE_READER_INVALID_ARGUMENT,
            "NULL message buffer must not change the status");
}

void test_fetch_rules() {
    const ScratchDir scratch;
    const auto object = scratch.path() / "object.bin";
    write_object(object, "abcdef");
    const std::string uri = file_uri(object);
    char err[128]{};
    std::uint8_t buf[8]{};
    size_t n = 99;

    // UINT64_MAX reads to the end, and the buffer caps it.
    require(nano_lance_fetch_external_blob(uri.c_str(), 4, UINT64_MAX, buf, 10, &n, err, sizeof err) ==
                NANO_LANCE_READER_OK,
            std::string("fetch to end: ") + err);
    require(n == 2 && buf[0] == 'e' && buf[1] == 'f', "a fetch to the end must stop at the end of the object");

    require(nano_lance_fetch_external_blob(uri.c_str(), 1, UINT64_MAX, buf, 3, &n, err, sizeof err) ==
                NANO_LANCE_READER_OK,
            std::string("fetch capped: ") + err);
    require(n == 3 && buf[0] == 'b' && buf[2] == 'd', "a fetch to the end must stop at the buffer's capacity");

    // Argument failures, each by status and without touching the outputs.
    n = 99;
    require(nano_lance_fetch_external_blob(nullptr, 0, 1, buf, sizeof buf, &n, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "NULL uri must be INVALID_ARGUMENT");
    require(nano_lance_fetch_external_blob("", 0, 1, buf, sizeof buf, &n, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "empty uri must be INVALID_ARGUMENT");
    require(nano_lance_fetch_external_blob(uri.c_str(), 0, 1, nullptr, sizeof buf, &n, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "NULL out_buf must be INVALID_ARGUMENT");
    require(nano_lance_fetch_external_blob(uri.c_str(), 0, 1, buf, 0, &n, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "a zero-capacity buffer must be INVALID_ARGUMENT");
    require(nano_lance_fetch_external_blob(uri.c_str(), 0, 1, buf, sizeof buf, nullptr, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "NULL bytes_read must be INVALID_ARGUMENT");
    require(n == 99, "a refused fetch must not write bytes_read");

    // A missing object fails by I/O status with a message.
    const auto missing = scratch.path() / "missing.bin";
    require(nano_lance_fetch_external_blob(file_uri(missing).c_str(), 0, 1, buf, sizeof buf, &n, err, sizeof err) !=
                NANO_LANCE_READER_OK,
            "a fetch of a missing object must fail");
    require(std::string(err).size() > 0, "a failed fetch must say why");
}

void run() {
    test_size();
    test_size_argument_errors();
    test_fetch_rules();
}

}  // namespace

int main() {
    try {
        run();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "external blob size tests passed\n";
    return 0;
}
