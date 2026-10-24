// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Feature flags: a manifest with a flag bit this build does not support is refused as Lance 12 refuses
// it. Writes a dataset, patches the flags into its manifest (the protobuf is rewritten as Lance's
// footer lays it out, with the field appended), and checks the error the reader and the writer give.

#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
    auto p = std::filesystem::temp_directory_path() / "nano_lance_feature_flags_ds";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

void write_one_batch(NanoLanceWriter& writer) {
    ArrowArray batch{};
    batch.length = 4;
    ArrowSchema field{};
    field.format = "l";
    field.name = "id";
    field.flags = 0;
    const std::int64_t values[] = {1, 2, 3, 4};
    const void* buffers[] = {nullptr, values};
    batch.n_buffers = 2;
    batch.buffers = buffers;
    require(nano_lance_write_batch(&writer, &batch, &field) == NANO_LANCE_OK, "write batch");
}

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>((v & 0x7FU) | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

std::filesystem::path the_manifest(const std::filesystem::path& ds) {
    for (const auto& entry : std::filesystem::directory_iterator(ds / "_versions")) {
        if (entry.path().extension() == ".manifest") {
            return entry.path();
        }
    }
    require(false, "no manifest");
    return {};
}

// Appends `reader` (field 9, tag 0x48) and `writer` (field 10, tag 0x50) to the manifest's protobuf: a
// proto3 scalar takes its last value, so the appended field wins. The footer is <u64 pos><u16><u16>LANC,
// and at `pos` a u32 length precedes the protobuf body.
void patch_flags(const std::filesystem::path& manifest, std::uint64_t reader, std::uint64_t writer) {
    std::ifstream in(manifest, std::ios::binary);
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(file.size() >= 16, "manifest footer");
    std::uint64_t pos = 0;
    std::memcpy(&pos, file.data() + file.size() - 16, sizeof pos);
    std::uint32_t len = 0;
    std::memcpy(&len, file.data() + pos, sizeof len);
    std::vector<std::uint8_t> body(file.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                                   file.begin() + static_cast<std::ptrdiff_t>(pos + 4 + len));
    body.push_back(0x48);
    put_varint(body, reader);
    body.push_back(0x50);
    put_varint(body, writer);

    std::vector<std::uint8_t> out(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(pos));
    const auto new_len = static_cast<std::uint32_t>(body.size());
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&new_len),
               reinterpret_cast<const std::uint8_t*>(&new_len) + sizeof new_len);
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), file.begin() + static_cast<std::ptrdiff_t>(pos + 4 + len), file.end());
    std::ofstream(manifest, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

bool contains(const char* text, const char* needle) {
    return std::strstr(text, needle) != nullptr;
}

}  // namespace

int main() {
    const auto ds = temp_dataset();
    {
        NanoLanceWriter writer{};
        require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
        write_one_batch(writer);
        require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    }

    // An unknown reader bit (9): the read is refused with pylance's text and the reader word.
    patch_flags(the_manifest(ds), 512, 512);
    char err[512]{};
    NanoLanceDatasetMetadata meta{};
    require(nano_lance_dataset_read_latest(ds.string().c_str(), &meta, err, sizeof err) != NANO_LANCE_READER_OK,
            "a reader bit this build does not support must refuse the read");
    const char* expected =
        "Not supported: This dataset cannot be read by this version of Lance. Please upgrade Lance to read this "
        "dataset. Flags: 512";
    if (!contains(err, expected)) {
        std::cerr << "unexpected read error: " << err << '\n';
        return 1;
    }

    // An unknown writer bit only (11): reads still work, and an append is refused before any data file.
    patch_flags(the_manifest(ds), 0, 2048);
    require(nano_lance_dataset_read_latest(ds.string().c_str(), &meta, err, sizeof err) == NANO_LANCE_READER_OK,
            "an unknown writer bit must not refuse the read");
    require(meta.total_physical_rows == 4, "rows of the writer-flagged dataset");
    nano_lance_dataset_metadata_free(&meta);

    NanoLanceWriter appender{};
    require(nano_lance_writer_init_append(&appender, ds.string().c_str(), 3) != NANO_LANCE_OK,
            "an append to a writer-flagged dataset must be refused");
    const char* refused = nano_lance_writer_last_error(&appender);
    const char* expected_write =
        "Not supported: This dataset cannot be written by this version of Lance. Please upgrade Lance to write "
        "to this dataset.\n Flags: 2048";
    if (!contains(refused, expected_write)) {
        std::cerr << "unexpected write error: " << refused << '\n';
        return 1;
    }
    std::cout << "feature flags: refused read and append as Lance does\n";
    return 0;
}
