// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// nanolance/data_file_writer: write_lance_data_file's file framing, as the file format specification lays
// it out. An int64 and a nullable utf8 column (with nulls) plus one extra global buffer and schema
// metadata: the file ends in the 40-byte footer (offsets, 2 global buffers, 2 columns, version 2.2, "LANC");
// the column metadata, the column metadata offset table and every page buffer start on a 64-byte
// boundary; global buffer 0 is the schema (the FileDescriptor, with its row count and metadata) and
// buffer 1 the extra one; each column decodes back to what was written, compressed or not; written with
// and without parallel columns the bytes are identical; and a column count that does not match the
// schema is refused.

#include "nanolance/data_file_reader.hpp"
#include "nanolance/data_file_writer.hpp"
#include "nanolance/lance_column_decoder.hpp"
#include "nanolance/schema_mapper.hpp"

#include "lance_minimal.pb.hpp"

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

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t kRows = 3000;

std::filesystem::path scratch() {
    auto p = std::filesystem::temp_directory_path() / "nano_lance_data_file_writer_test";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p);
    return p;
}

nano_lance::LanceSchemaMapping mapping() {
    ArrowSchema schema;
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, 2) == NANOARROW_OK, "struct schema");
    require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK, "int64");
    require(ArrowSchemaSetName(schema.children[0], "id") == NANOARROW_OK, "id");
    schema.children[0]->flags &= ~ARROW_FLAG_NULLABLE;
    require(ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_STRING) == NANOARROW_OK, "utf8");
    require(ArrowSchemaSetName(schema.children[1], "name") == NANOARROW_OK, "name");
    nano_lance::LanceSchemaMapping out;
    std::string error;
    require(nano_lance::map_arrow_schema(schema, out, error), "map schema: " + error);
    schema.release(&schema);
    return out;
}

bool name_is_null(std::uint64_t i) { return i % 7U == 3U; }
std::string name_of(std::uint64_t i) { return "name-" + std::to_string(i % 250U); }

std::vector<nano_lance::ColumnValues> columns() {
    std::vector<nano_lance::ColumnValues> out(2);
    auto& ids = out[0];
    ids.kind = nano_lance::ColumnValues::Kind::FixedWidth;
    ids.rows = kRows;
    ids.fixed.resize(kRows * 8U);
    for (std::uint64_t i = 0; i < kRows; ++i) {
        const auto v = static_cast<std::int64_t>(i * 3U) - 100;
        std::memcpy(ids.fixed.data() + i * 8U, &v, 8U);
    }
    auto& names = out[1];
    names.kind = nano_lance::ColumnValues::Kind::VariableWidth;
    names.rows = kRows;
    names.validity.assign((kRows + 7U) / 8U, 0U);
    std::int32_t end = 0;
    names.variable.offsets.resize((kRows + 1U) * 4U);
    std::memcpy(names.variable.offsets.data(), &end, 4U);
    for (std::uint64_t i = 0; i < kRows; ++i) {
        if (name_is_null(i)) {
            ++names.null_count;
        } else {
            names.validity[i / 8U] |= static_cast<std::uint8_t>(1U << (i % 8U));
            const auto s = name_of(i);
            names.variable.data.insert(names.variable.data.end(), s.begin(), s.end());
            end += static_cast<std::int32_t>(s.size());
        }
        std::memcpy(names.variable.offsets.data() + (i + 1U) * 4U, &end, 4U);
    }
    return out;
}

Bytes file_bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

template <class T>
T le(const Bytes& b, std::size_t at) {
    T v{};
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

std::filesystem::path write(const std::filesystem::path& dir, const std::string& name, bool compress, bool parallel) {
    nano_lance::LanceFileExtras extras;
    extras.schema_metadata["purpose"] = {'t', 'e', 's', 't'};
    extras.global_buffers.push_back({'G', 'B', '1'});
    nano_lance::DataFileResult result;
    std::string error;
    require(nano_lance::write_lance_data_file(dir, name, mapping(), columns(), kRows, 3, compress, result, error,
                                              parallel, &extras),
            "write " + name + ": " + error);
    require(result.relative_path == std::filesystem::path("data") / name, "relative path data/<name>");
    const auto path = dir / result.relative_path;
    require(std::filesystem::file_size(path) == result.file_size_bytes, "the reported size is the file's");
    return path;
}

void check_file(const std::filesystem::path& path) {
    const auto bytes = file_bytes(path);
    require(bytes.size() > 40U, "a file longer than its footer");
    const auto f = bytes.size() - 40U;
    const auto column_metadata_start = le<std::uint64_t>(bytes, f);
    const auto cmo = le<std::uint64_t>(bytes, f + 8U);
    const auto gbo = le<std::uint64_t>(bytes, f + 16U);
    require(le<std::uint32_t>(bytes, f + 24U) == 2U, "two global buffers");
    require(le<std::uint32_t>(bytes, f + 28U) == 2U, "two columns");
    require(le<std::uint16_t>(bytes, f + 32U) == 2U && le<std::uint16_t>(bytes, f + 34U) == 2U, "version 2.2");
    require(std::memcmp(bytes.data() + bytes.size() - 4U, "LANC", 4U) == 0, "LANC magic");
    require(column_metadata_start % 64U == 0U && cmo % 64U == 0U, "metadata and offset table 64-byte aligned");
    require(column_metadata_start < cmo && cmo < gbo && gbo + 2U * 16U == f, "metadata, CMO, GBO, footer in order");

    nano_lance::pb::FileDescriptor descriptor;
    nano_lance::LanceDataFileFooterLayout layout;
    std::string error;
    require(nano_lance::read_lance_data_file_footer_and_descriptor(path, descriptor, layout, error), "footer: " + error);
    require(layout.major_version == 2U && layout.minor_version == 2U && !layout.is_v2_0(), "the reader sees 2.2");
    require(layout.num_columns == 2U && layout.num_global_buffers == 2U, "the reader's counts");
    require(descriptor.length == kRows, "the schema's row count");
    require(descriptor.fields.size() == 2U && descriptor.fields[0].name == "id" && descriptor.fields[1].name == "name",
            "the schema's fields");
    require(descriptor.schema_metadata.count("purpose") == 1U, "the schema metadata");

    Bytes buffer;
    require(nano_lance::read_lance_file_global_buffer(path, layout, 1, buffer, error) && buffer == Bytes({'G', 'B', '1'}),
            "global buffer 1 is the extra one");
    require(nano_lance::read_lance_file_global_buffer(path, layout, 0, buffer, error) &&
                le<std::uint64_t>(bytes, gbo) % 64U == 0U,
            "global buffer 0 (the schema) is aligned");
    require(!nano_lance::read_lance_file_global_buffer(path, layout, 2, buffer, error), "no global buffer 2");

    std::vector<nano_lance::pb::ColumnMetadata> metadata;
    require(nano_lance::read_lance_data_file_column_metadatas(path, layout, metadata, error), "columns: " + error);
    require(metadata.size() == 2U, "two column metadatas");
    for (const auto& column : metadata) {
        std::uint64_t page_rows = 0;
        for (const auto& page : column.pages) {
            page_rows += page.length;
            for (std::size_t b = 0; b < page.buffer_offsets.size(); ++b) {
                require(page.buffer_offsets[b] % 64U == 0U || page.buffer_sizes[b] == 0U, "page buffers 64-byte aligned");
                require(page.buffer_offsets[b] + page.buffer_sizes[b] <= column_metadata_start,
                        "page buffers before the column metadata");
            }
        }
        require(page_rows == kRows, "a column's pages hold every row");
    }

    const nano_lance::DataFileIo io(path);
    nano_lance::ColumnValues ids;
    require(nano_lance::decode_lance_physical_column(io, descriptor.fields[0], metadata[0], ids, error), "ids: " + error);
    require(ids.fixed.size() == kRows * 8U && ids.null_count == 0U, "ids rows");
    for (std::uint64_t i = 0; i < kRows; ++i) {
        require(le<std::int64_t>(ids.fixed, i * 8U) == static_cast<std::int64_t>(i * 3U) - 100, "id value");
    }
    nano_lance::ColumnValues names;
    require(nano_lance::decode_lance_physical_column(io, descriptor.fields[1], metadata[1], names, error),
            "names: " + error);
    require(names.variable.offsets.size() == (kRows + 1U) * 4U, "names rows");
    std::uint64_t nulls = 0;
    for (std::uint64_t i = 0; i < kRows; ++i) {
        const bool valid = names.validity.empty() || (names.validity[i / 8U] >> (i % 8U) & 1U) != 0U;
        require(valid == !name_is_null(i), "name validity of row " + std::to_string(i));
        if (!valid) {
            ++nulls;
            continue;
        }
        const auto begin = le<std::int32_t>(names.variable.offsets, i * 4U);
        const auto end = le<std::int32_t>(names.variable.offsets, (i + 1U) * 4U);
        require(std::string(names.variable.data.begin() + begin, names.variable.data.begin() + end) == name_of(i),
                "name value of row " + std::to_string(i));
    }
    require(names.null_count == nulls, "names null count");
}

}  // namespace

int main() {
    const auto dir = scratch();
    const auto plain = write(dir, "plain.lance", false, true);
    check_file(plain);
    const auto compressed = write(dir, "zstd.lance", true, true);
    check_file(compressed);

    const auto serial = write(dir, "serial.lance", false, false);
    require(file_bytes(serial) == file_bytes(plain), "parallel and streamed columns write the same bytes");

    nano_lance::DataFileResult result;
    std::string error;
    auto one = columns();
    one.pop_back();
    require(!nano_lance::write_lance_data_file(dir, "bad.lance", mapping(), one, kRows, 3, false, result, error),
            "a column count that does not match the schema is refused");
    require(error.find("count") != std::string::npos, "the refusal says why: " + error);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "data_file_writer: ok\n";
    return 0;
}
