// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer io: where bytes live. Local files and the object-store schemes nanolance has registered (s3, when it is
// built with it) sit behind one interface. This example writes bytes through the local file backend, reads them
// back in three ways, lists the directory and removes the file.
//
//   - io::open_output gives a stream. Nothing becomes visible until commit() succeeds.
//   - io::open_source gives a ByteSource. read_at reads an exact range; a source keeps no position.
//   - io::read_all, io::list_dir and io::remove_output cover the rest.
//
// Run it with: ctest --test-dir build -R nano_lance_example_io --output-on-failure

#include "nanolance/byte_sink.hpp"
#include "nanolance/byte_source.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

// A fresh directory under the temp directory. It is removed when the example ends, also on an early return.
class ScratchDir {
public:
    explicit ScratchDir(const std::string& name) : path_(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Prints the failed check and returns the exit code.
int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

}  // namespace

int main() {
    const ScratchDir scratch("nano_lance_example_io");
    const std::string path = (scratch.path() / "greeting.txt").string();
    const std::string text = "hello, lance\n";
    std::string error;

    // Step 1: write the bytes. The file appears only when commit() succeeds.
    {
        auto out = nano_lance::io::open_output(path, error);
        if (!out) {
            return fail("open_output: " + error);
        }
        out->write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out->commit(error)) {
            return fail("commit: " + error);
        }
    }
    std::cout << "wrote " << text.size() << " bytes\n";

    // Step 2: read an exact range back through a ByteSource. Bytes 7 to 11 are "lance".
    {
        auto source = nano_lance::io::open_source(path, error);
        if (!source) {
            return fail("open_source: " + error);
        }
        if (source->size() != text.size()) {
            return fail("size is " + std::to_string(source->size()) + ", want " + std::to_string(text.size()));
        }
        std::vector<std::uint8_t> range(5);
        if (!source->read_at(7, range.size(), range.data(), error)) {
            return fail("read_at: " + error);
        }
        const std::string word(range.begin(), range.end());
        std::cout << "read_at(7, 5) = \"" << word << "\"\n";
        if (word != "lance") {
            return fail("read_at returned \"" + word + "\", want \"lance\"");
        }
    }

    // Step 3: read the whole object at once and compare it with what was written.
    std::vector<std::uint8_t> all;
    if (!nano_lance::io::read_all(path, all, error)) {
        return fail("read_all: " + error);
    }
    if (std::string(all.begin(), all.end()) != text) {
        return fail("read_all returned different bytes");
    }

    // Step 4: list the directory. Local entries carry a size only when asked for.
    std::vector<nano_lance::io::DirEntry> entries;
    if (!nano_lance::io::list_dir(scratch.path().string(), entries, error, true)) {
        return fail("list_dir: " + error);
    }
    if (entries.size() != 1 || entries[0].name != "greeting.txt" || entries[0].size != text.size()) {
        return fail("list_dir did not show greeting.txt with the right size");
    }
    std::cout << "listed " << entries.size() << " entry: " << entries[0].name << "\n";

    // Step 5: remove it. Removing a missing file is not an error, but this one exists.
    if (!nano_lance::io::remove_output(path, error)) {
        return fail("remove_output: " + error);
    }
    if (nano_lance::io::exists(path)) {
        return fail("the file still exists after remove_output");
    }
    std::cout << "removed it; exists() is false\n";
    return 0;
}
