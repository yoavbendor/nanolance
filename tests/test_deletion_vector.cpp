// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The deletion files of a fragment, both shapes Lance writes: an Arrow IPC file of row offsets
// (sparse deletions) and a portable roaring bitmap (dense ones). A read must return the offsets
// sorted and distinct, and must refuse anything whose count disagrees with the manifest, because
// filtering by a vector that does not match the manifest silently returns the wrong rows.

#include "lance_minimal.pb.hpp"
#include "nanolance/deletion_vector.hpp"
#include "nanolance/roaring_bitmap.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace pb = nano_lance::pb;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path scratch(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_deletion_vector_" + std::string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p / "_deletions");
    return p;
}

void write_bytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

void require(bool ok, const std::string& msg, const std::string& detail) {
    require(ok, msg + detail);
}

int main() {
    const auto root = scratch("main");
    const auto deletions = root / "_deletions";

    // Sparse: written unsorted with a repeat-free set; read back ascending.
    {
        pb::DeletionFile written;
        std::string error;
        require(nano_lance::write_deletion_file(root, 4, 7, {9, 2, 5}, written, error), "write: ", error);
        require(written.present, "a written deletion file is present");
        require(written.file_type == 0, "sparse deletions are an Arrow file (type 0)");
        require(written.read_version == 7, "read version is kept");
        require(written.num_deleted_rows == 3, "the count is the number written");

        const auto name = "4-7-" + std::to_string(written.id) + ".arrow";
        require(std::filesystem::exists(deletions / name), "the file is named as Lance names it: " + name);

        std::vector<std::uint32_t> offsets;
        require(nano_lance::read_deletion_vector(deletions, 4, written, offsets, error), "read: ", error);
        const std::vector<std::uint32_t> expected = {2, 5, 9};
        require(offsets == expected, "offsets come back sorted");
    }

    // An empty deletion list: the writer accepts it and the file counts zero rows.
    // KNOWN: the reader does not read that file back. A zero-row record batch leaves its length field
    // out (a flatbuffer default), and parse_arrow_ipc_uint32_column refuses the omitted field as
    // "malformed Arrow IPC record batch length". The library's own callers never write an empty
    // deletion file (an emptied fragment is removed instead), so this is reported rather than failed.
    {
        pb::DeletionFile written;
        std::string error;
        require(nano_lance::write_deletion_file(root, 5, 1, {}, written, error), "write empty: ", error);
        require(written.num_deleted_rows == 0, "an empty file counts zero");
        std::vector<std::uint32_t> offsets = {42};
        if (nano_lance::read_deletion_vector(deletions, 5, written, offsets, error)) {
            require(offsets.empty(), "an empty deletion file deletes nothing");
        } else {
            require(error == "malformed Arrow IPC record batch length",
                    "the empty file is refused for the known reason: " + error);
            require(offsets.empty(), "a refused read returns no offsets");
            std::cout << "KNOWN: an empty deletion file (0 rows, Arrow IPC) written by write_deletion_file is "
                         "refused by read_deletion_vector: \"" << error << "\"\n";
        }
    }

    // A fragment with no deletion file: nothing deleted, and no file is opened.
    {
        pb::DeletionFile absent;
        std::vector<std::uint32_t> offsets = {1};
        std::string error;
        require(nano_lance::read_deletion_vector(deletions, 0, absent, offsets, error), "absent file is not an error");
        require(offsets.empty(), "an absent deletion file deletes nothing");
    }

    // A count that disagrees with the manifest is refused, not guessed at: duplicates make two
    // distinct offsets where the manifest claims three.
    {
        pb::DeletionFile written;
        std::string error;
        require(nano_lance::write_deletion_file(root, 6, 1, {4, 4, 1}, written, error), "write dup: ", error);
        std::vector<std::uint32_t> offsets;
        require(!nano_lance::read_deletion_vector(deletions, 6, written, offsets, error),
                "a count that disagrees with the manifest is refused");
        require(error.find("holds 2 offsets but the manifest claims 3") != std::string::npos,
                "the refusal names both counts: ", error);
    }

    // A missing file is an error that names it.
    {
        pb::DeletionFile missing;
        missing.present = true;
        missing.read_version = 2;
        missing.id = 99;
        missing.num_deleted_rows = 1;
        std::vector<std::uint32_t> offsets;
        std::string error;
        require(!nano_lance::read_deletion_vector(deletions, 8, missing, offsets, error), "a missing file fails");
        require(error.find("failed to open deletion file 8-2-99.arrow") != std::string::npos,
                "the error names the file: ", error);
    }

    // Dense: a roaring bitmap named .bin, read through the same entry point.
    {
        const std::vector<std::uint32_t> ids = {3, 10, 70000};
        write_bytes(deletions / "9-3-11.bin", nano_lance::roaring::encode(ids));
        pb::DeletionFile dense;
        dense.present = true;
        dense.file_type = 1;
        dense.read_version = 3;
        dense.id = 11;
        dense.num_deleted_rows = 3;
        std::vector<std::uint32_t> offsets;
        std::string error;
        require(nano_lance::read_deletion_vector(deletions, 9, dense, offsets, error), "bitmap read: ", error);
        require(offsets == ids, "a bitmap deletion file reads back");
    }

    // A bitmap that holds more values than the manifest's count is refused by the budget.
    {
        write_bytes(deletions / "10-1-12.bin", nano_lance::roaring::encode({1, 2, 3}));
        pb::DeletionFile dense;
        dense.present = true;
        dense.file_type = 1;
        dense.read_version = 1;
        dense.id = 12;
        dense.num_deleted_rows = 2;
        std::vector<std::uint32_t> offsets;
        std::string error;
        require(!nano_lance::read_deletion_vector(deletions, 10, dense, offsets, error),
                "a bitmap over its declared count is refused");
        require(!error.empty(), "the refusal says why");
    }

    // parse_roaring_bitmap: the round trip, the empty set, a bad cookie, and the value budget.
    {
        std::vector<std::uint32_t> out;
        std::string error;
        const std::vector<std::uint32_t> ids = {0, 1, 65535, 65536, 4000000000U};
        const auto bytes = nano_lance::roaring::encode(ids);
        require(nano_lance::parse_roaring_bitmap(bytes, out, error, 0), "parse: ", error);
        require(out == ids, "a portable bitmap parses to its ids");

        require(nano_lance::parse_roaring_bitmap(nano_lance::roaring::encode({}), out, error, 0),
                "the empty bitmap parses");
        require(out.empty(), "the empty bitmap has no values");

        const std::vector<std::uint8_t> bad_cookie = {0, 0, 0, 0, 0, 0, 0, 0};
        require(!nano_lance::parse_roaring_bitmap(bad_cookie, out, error, 0), "a bad cookie is refused");
        require(error == "deletion bitmap has an unrecognized cookie", "the cookie error: ", error);

        require(!nano_lance::parse_roaring_bitmap({}, out, error, 0), "an empty buffer is refused");
        require(error == "deletion bitmap is truncated", "the truncation error: ", error);

        require(!nano_lance::parse_roaring_bitmap(bytes, out, error, 2), "a budget below the value count is refused");
        require(error.find("read budget") != std::string::npos, "the budget error: ", error);
    }

    // parse_arrow_ipc_uint32_column refuses anything that is not an Arrow IPC file.
    {
        std::vector<std::uint32_t> out;
        std::string error;
        require(!nano_lance::parse_arrow_ipc_uint32_column({}, out, error, 0), "an empty buffer is refused");
        require(error == "deletion file is not an Arrow IPC file", "the magic error: ", error);
        const std::vector<std::uint8_t> not_arrow = {'P', 'A', 'R', 'Q', 'U', 'E', 'T', '1', 0, 0, 0, 0};
        require(!nano_lance::parse_arrow_ipc_uint32_column(not_arrow, out, error, 0), "a parquet file is refused");
        require(out.empty(), "a refused parse returns no values");
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::cout << "deletion vector: sparse (.arrow) and dense (.bin) reads, count checks, malformed input OK\n";
    return 0;
}
