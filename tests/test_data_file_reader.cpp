// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/data_file_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset() {
    auto p = std::filesystem::temp_directory_path() / "nano_lance_data_file_reader_ds";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

/// Replace the file at `p`, unlinking first so the new contents get a NEW inode.
///
/// Truncating in place would not test anything: a cached file handle follows the inode, so a
/// rewritten-in-place file reads correctly even from a stale handle. Reusing a *name* for a different
/// file is the case the cache exists to catch, and it is what a rewritten dataset directory does.
void write_file(const std::filesystem::path& p, const std::string& bytes) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "write scratch file");
}

bool read_fails(const std::filesystem::path& p, std::uint64_t offset, std::uint64_t size) {
    std::vector<std::uint8_t> got;
    std::string err;
    return !nano_lance::read_lance_data_file_bytes(p, offset, size, got, err);
}

std::string read_at(const std::filesystem::path& p, std::uint64_t offset, std::uint64_t size) {
    std::vector<std::uint8_t> got;
    std::string err;
    require(nano_lance::read_lance_data_file_bytes(p, offset, size, got, err), err.c_str());
    return std::string(got.begin(), got.end());
}

/// The reader keeps a small LRU of open data files, and a DataFileReadScope lets one read operation
/// validate a file once instead of once per page buffer. Two things have to stay true either way.
///
/// 1. A path can be reused by a DIFFERENT file: fragment names come from the lowest unused suffix, so
///    rewriting a dataset directory reproduces "fragment-0.lance" with other bytes. Between read
///    operations that must be noticed -- otherwise a read returns the previous dataset's contents with
///    no error at all, which is the worst shape a bug can take here.
/// 2. The cached stream position, which exists so a page read that continues where the last one
///    stopped can skip its seek, must never let a read return the wrong range -- including after a
///    read that failed partway.
void test_open_file_cache() {
    const auto dir = std::filesystem::temp_directory_path() / "nano_lance_data_file_cache";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const auto path = dir / "fragment-0.lance";

    write_file(path, "AAAABBBBCCCCDDDD");
    require(read_at(path, 0, 4) == "AAAA", "first read");
    // Sequential continuation: this is the read whose seek the position cache elides.
    require(read_at(path, 4, 4) == "BBBB", "sequential read skips the seek but not the bytes");
    require(read_at(path, 12, 4) == "DDDD", "forward jump still seeks");
    require(read_at(path, 0, 4) == "AAAA", "backward jump still seeks");

    // Same path, different and SHORTER file. A stale entry shows up twice over: the old handle still
    // reads the unlinked inode's bytes, and the old file_size lets a now-out-of-bounds read through.
    write_file(path, "ZZZZ");
    require(read_at(path, 0, 4) == "ZZZZ", "a replaced file of a different size must not read stale bytes");
    require(read_fails(path, 0, 8), "a read past the replacement's end must be refused, not served stale");

    // ...and the other direction: a replacement that GREW must not still be bounded by the old size.
    write_file(path, "EEEEFFFFGGGGHHHH");
    require(read_at(path, 8, 4) == "GGGG", "a read inside the replacement's new length must be allowed");

    // Same path, different file, SAME size -- caught by mtime, not by size.
    write_file(path, "QQQQQQQQQQQQQQQQ");
    require(read_at(path, 0, 4) == "QQQQ", "a same-size replacement must not read stale bytes");
    write_file(path, "QQQQ");
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2),
                                     ec);
    require(!ec, "bump mtime");
    require(read_at(path, 0, 4) == "QQQQ", "a replaced file of the same size must not read stale bytes");

    // A refused read must not leave a cached position that misdirects the next one. This one is
    // refused by the bounds check before the stream is touched at all, so the position is simply
    // unchanged; the other path -- the stream itself failing after the bounds check passed -- needs
    // the file to shrink between the stat and the read, which is a race this test cannot stage. The
    // reader forgets the position there rather than guessing, and that line is defensive.
    {
        write_file(path, "0123456789");
        require(read_at(path, 0, 4) == "0123", "prime the cached position");
        require(read_fails(path, 6, 99), "a read past the end must be refused");
        require(read_at(path, 4, 4) == "4567", "a read after a refused one lands where it was asked to");
        require(read_at(path, 0, 4) == "0123", "...including when it has to seek backwards");
    }

    // Inside one read operation the file is validated once, and the reads themselves are unaffected.
    {
        const nano_lance::DataFileReadScope scope;
        require(read_at(path, 0, 4) == "0123", "scoped read");
        require(read_at(path, 4, 4) == "4567", "scoped sequential read");
        require(read_at(path, 2, 4) == "2345", "scoped backward read");
    }
    // ...and a replacement after that operation is still noticed by the next one.
    write_file(path, "WXYZ");
    require(read_at(path, 0, 4) == "WXYZ", "a replacement between operations must be noticed");

    std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main() {
    test_open_file_cache();

    const auto ds = temp_dataset();

    {
        NanoLanceWriter writer{};
        require(nano_lance_writer_init(&writer, ds.string().c_str(), 0) == NANO_LANCE_OK, "init");
        ArrowArray batch{};
        batch.length = 7;
        ArrowSchema field{};
        field.format = "l";
        field.name = "x";
        field.flags = 0;
        const std::int64_t values[] = {10, 11, 12, 13, 14, 15, 16};
        const void* buffers[] = {nullptr, values};
        batch.n_buffers = 2;
        batch.buffers = buffers;
        require(nano_lance_write_batch(&writer, &batch, &field) == NANO_LANCE_OK, "write batch");
        require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    }

    const auto data_path = ds / "data";
    std::filesystem::path fragment;
    for (const auto& e : std::filesystem::directory_iterator(data_path)) {
        if (e.is_regular_file() && e.path().extension() == ".lance") {
            fragment = e.path();
            break;
        }
    }
    require(!fragment.empty(), "expected fragment .lance file");

    nano_lance::pb::FileDescriptor descriptor{};
    nano_lance::LanceDataFileFooterLayout layout{};
    std::string err;
    require(nano_lance::read_lance_data_file_footer_and_descriptor(fragment, descriptor, layout, err), err.c_str());
    require(descriptor.length == 7ULL, "descriptor row length");
    require(layout.num_columns == 1U, "one physical column");
    require(!descriptor.fields.empty(), "descriptor has fields");

    std::error_code ec;
    std::filesystem::remove_all(ds, ec);
    return 0;
}
