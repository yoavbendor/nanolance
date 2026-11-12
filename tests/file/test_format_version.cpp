// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The file-format versions nanolance reads, without Rust: a file this build writes (format 2.2) with its footer
// re-stamped as 2.3 reads back the same rows, and a footer that names a version nanolance does not read (2.4) is
// refused by name, with no rows.

#include "nanolance/data_file_reader.hpp"
#include "nanolance/lance_file_writer.hpp"
#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::filesystem::path scratch() {
    const auto dir = std::filesystem::temp_directory_path() / "nanolance_format_version_test";
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_all(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> bytes_of(const std::string& s) { return {s.begin(), s.end()}; }

void make_schema(ArrowSchema& schema) {
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, 2) == NANOARROW_OK, "struct schema");
    require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK, "int64 column");
    require(ArrowSchemaSetName(schema.children[0], "id") == NANOARROW_OK, "id name");
    require(ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_STRING) == NANOARROW_OK, "utf8 column");
    require(ArrowSchemaSetName(schema.children[1], "name") == NANOARROW_OK, "name name");
}

void make_batch(const std::vector<std::int64_t>& ids, const std::vector<std::string>& names,
                const ArrowSchema& schema, ArrowArray& array) {
    ArrowError err{};
    require(ArrowArrayInitFromSchema(&array, &schema, &err) == NANOARROW_OK, std::string("array: ") + err.message);
    require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start appending");
    for (std::size_t i = 0; i < ids.size(); ++i) {
        require(ArrowArrayAppendInt(array.children[0], ids[i]) == NANOARROW_OK, "append id");
        const ArrowStringView text = {names[i].data(), static_cast<std::int64_t>(names[i].size())};
        require(ArrowArrayAppendString(array.children[1], text) == NANOARROW_OK, "append name");
    }
    array.length = static_cast<std::int64_t>(ids.size());
    require(ArrowArrayFinishBuildingDefault(&array, &err) == NANOARROW_OK, std::string("finish: ") + err.message);
}

}  // namespace

int main() {
    const auto dir = scratch();
    const auto v22 = dir / "v22.lance";
    const auto v23 = dir / "v23.lance";
    const auto bad_version = dir / "bad_version.lance";
    const auto corrupt = dir / "corrupt.lance";

    ArrowSchema schema{};
    make_schema(schema);
    ArrowArray batch{};
    make_batch({0, 1, 2, 3, 4, 5, 6}, {"n0", "n1", "n2", "n3", "n4", "n5", "n6"}, schema, batch);

    nano_lance::LanceFileExtras extras;
    extras.path = v22;
    std::uint64_t written = 0;
    std::string error;
    const std::vector<const ArrowArray*> batches = {&batch};
    require(nano_lance::write_lance_file(schema, batches, extras, written, error), "write: " + error);

    const auto bytes = read_all(v22);
    require(bytes.size() > 8 && bytes[bytes.size() - 4] == 'L' && bytes[bytes.size() - 1] == 'C',
            "the file ends in the magic");
    // Footer: ... major (u16) at size-8, minor (u16) at size-6, magic at size-4.
    require(bytes[bytes.size() - 8] == 2 && bytes[bytes.size() - 6] == 2, "written as format 2.2");

    const auto read_rows = [&](const std::filesystem::path& path, std::uint64_t& rows) {
        nano_lance::LanceScanRequest request;
        ArrowSchema out_schema{};
        std::vector<ArrowArray> out;
        const bool ok = nano_lance::lance_file_read(path, request, out_schema, out, error);
        rows = 0;
        for (const auto& array : out) {
            rows += static_cast<std::uint64_t>(array.length);
        }
        for (auto& array : out) {
            if (array.release != nullptr) {
                ArrowArrayRelease(&array);
            }
        }
        if (out_schema.release != nullptr) {
            ArrowSchemaRelease(&out_schema);
        }
        return ok;
    };

    // 2.2 reads, as before.
    std::uint64_t rows = 0;
    require(read_rows(v22, rows) && rows == 7, "the 2.2 file reads seven rows: " + error);

    // 2.3: the same container and pages, so the same rows.
    auto restamped = bytes;
    restamped[restamped.size() - 6] = 3;
    write_all(v23, restamped);
    require(read_rows(v23, rows) && rows == 7, "a 2.3 footer reads seven rows: " + error);

    // A version nanolance does not read is refused by name, with no rows.
    auto unknown = bytes;
    unknown[unknown.size() - 6] = 4;
    write_all(bad_version, unknown);
    require(!read_rows(bad_version, rows), "a 2.4 footer is refused");
    require(error.find("2.4") != std::string::npos, "the refusal names the version: " + error);

    ArrowArrayRelease(&batch);
    ArrowSchemaRelease(&schema);
    std::filesystem::remove_all(dir);
    std::cout << "format version tests passed\n";
    return 0;
}
