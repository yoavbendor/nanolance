// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Local files through the io layer (include/nanolance/byte_sink.hpp and byte_source.hpp): an output written
// through open_output reads back through open_source and read_all at the same bytes; put_if_absent publishes
// once and never overwrites (the commit step of a write); remove_output removes a file, is not an error for a
// missing one, and calls the removed-file hook with the path; an unsupported scheme is refused on the write
// side. Every file lives in a directory this test creates and removes.

#include "nanolance/byte_sink.hpp"
#include "nanolance/byte_source.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::vector<std::uint8_t> bytes_of(const std::string& s) { return {s.begin(), s.end()}; }

int g_hook_calls = 0;
std::string g_hook_path;

void count_removed(const std::string& path) {
    ++g_hook_calls;
    g_hook_path = path;
}

}  // namespace

int main() {
    namespace io = nano_lance::io;
    int checks = 0;

    const auto dir = std::filesystem::temp_directory_path() /
                     ("nano_lance_byte_sink_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);

    // Write then read back: open_output creates the parent directory, commit makes the file exist, and the
    // source reads the same bytes at any offset.
    const auto file = dir / "sub" / "hello.bin";
    {
        std::string error;
        auto out = io::open_output(file.string(), error);
        require(out != nullptr, "open_output creates the file (" + error + ")");
        *out << "hello world";
        require(out->commit(error), "commit succeeds (" + error + ")");
        require(std::filesystem::file_size(file) == 11, "the file holds the 11 bytes written");
        checks += 3;
    }
    {
        std::string error;
        auto source = io::open_source(file.string(), error);
        require(source != nullptr, "open_source opens the written file (" + error + ")");
        require(source->size() == 11, "the source reports the file's size");

        std::vector<std::uint8_t> got(5);
        require(source->read_at(6, 5, got.data(), error), "a ranged read succeeds");
        require(got == bytes_of("world"), "the ranged read returns bytes 6..10");

        std::vector<std::uint8_t> whole(11);
        require(source->read_at(0, 11, whole.data(), error), "a whole-file read succeeds");
        require(whole == bytes_of("hello world"), "the whole read returns the written bytes");

        std::vector<std::uint8_t> past(4);
        require(!source->read_at(9, 4, past.data(), error), "a read past the end is refused");
        require(!error.empty(), "a refused read says why");
        checks += 5;

        std::vector<std::uint8_t> all;
        require(io::read_all(file.string(), all, error), "read_all reads the file");
        require(all == bytes_of("hello world"), "read_all returns the whole file");
        checks += 2;
    }

    // Opening a missing file is an error with a message, and exists() says no.
    {
        std::string error;
        const auto missing = (dir / "missing.bin").string();
        require(io::open_source(missing, error) == nullptr, "a missing file does not open");
        require(!error.empty(), "a missing file says why");
        require(!io::exists(missing), "exists() is false for a missing file");
        require(io::exists(file.string()), "exists() is true for a written file");
        std::uint64_t size = 0;
        std::int64_t version = 0;
        require(!io::stat_local(missing, size, version), "stat_local fails for a missing file");
        require(io::stat_local(file.string(), size, version) && size == 11, "stat_local reports the size");
        checks += 6;
    }

    // put_if_absent: the first call publishes the bytes, the second reports already_exists and leaves the first
    // bytes in place.
    const auto target = dir / "once" / "value.bin";
    {
        std::string error;
        bool already = true;
        require(io::put_if_absent(target.string(), bytes_of("first"), already, error),
                "put_if_absent publishes a new file (" + error + ")");
        require(!already, "a new file is not already_exists");
        std::vector<std::uint8_t> published;
        require(io::read_all(target.string(), published, error) && published == bytes_of("first"),
                "the published file holds the bytes given");
        checks += 3;
    }
    {
        std::string error;
        bool already = false;
        require(!io::put_if_absent(target.string(), bytes_of("second"), already, error),
                "put_if_absent refuses to replace an existing file");
        require(already, "the refusal is reported as already_exists");
        std::vector<std::uint8_t> kept;
        require(io::read_all(target.string(), kept, error) && kept == bytes_of("first"),
                "the first writer's bytes are kept");
        checks += 3;
    }

    // remove_output removes the file; a second removal of the missing file is not an error. The removed-file
    // hook is called with each path.
    {
        std::string error;
        io::set_output_removed_hook(&count_removed);
        g_hook_calls = 0;
        require(io::remove_output(target.string(), error), "remove_output removes the file (" + error + ")");
        require(!io::exists(target.string()), "the file is gone after remove_output");
        require(g_hook_calls == 1 && g_hook_path == target.string(), "the removed-file hook is called with the path");

        require(io::remove_output(target.string(), error), "removing a missing file is not an error");
        require(g_hook_calls == 2, "the hook is called for the missing file too");
        io::set_output_removed_hook(nullptr);
        checks += 4;
    }

    // Writes to an unregistered scheme are refused with a message; a local path is not an object store.
    {
        std::string error;
        require(io::open_output("nosuchscheme://bucket/key", error) == nullptr,
                "an unregistered scheme is refused for writing");
        require(error.find("not supported") != std::string::npos, "the refusal says the scheme is not supported");
        require(!io::is_object_store_output(file.string()), "a local path is not an object store output");
        checks += 3;
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "byte_sink: " << checks << " checks passed\n";
    return 0;
}
