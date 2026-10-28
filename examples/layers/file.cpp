// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer file: how a .lance file is framed. A file is data pages, column metadata, global buffers (the schema is
// global buffer 0) and a footer. This example writes a one-column .lance file with write_lance_file, reads its footer
// and schema, then decodes the column through the encoding layer, reading the file's bytes with a DataFileIo.
//
//   - write_lance_file: Arrow batches -> one standalone .lance file (format 2.2).
//   - read_lance_data_file_footer_and_descriptor, read_lance_data_file_column_metadatas: the footer and metadata.
//   - decode_lance_physical_column: one column's values, from the file's pages.
//
// Run it with: ctest --test-dir build -R nano_lance_example_file --output-on-failure

#include "nanolance/column_values.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/lance_column_decoder.hpp"
#include "nanolance/lance_file_writer.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

// A fresh directory under the temp directory, removed when the example ends (also on an early return).
struct ScratchDir {
    std::filesystem::path path;
    explicit ScratchDir(const std::string& name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

int fail(const std::string& message) { std::cerr << "FAIL: " << message << "\n"; return 1; }

// One int64 column "id" as an Arrow struct batch, written out field by field in the C data interface's order (this
// layer's README does not name nanoarrow). The batch points at `values`, which must outlive it.
struct IdBatch {
    ArrowSchema id_schema{"l", "id", nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr};
    ArrowSchema* schema_children[1] = {&id_schema};
    ArrowSchema schema{"+s", nullptr, nullptr, 0, 1, schema_children, nullptr, nullptr, nullptr};
    const void* id_buffers[2] = {nullptr, nullptr};
    ArrowArray id_array{0, 0, 0, 2, 0, id_buffers, nullptr, nullptr, nullptr, nullptr};
    ArrowArray* array_children[1] = {&id_array};
    const void* root_buffers[1] = {nullptr};
    ArrowArray array{0, 0, 0, 1, 1, root_buffers, array_children, nullptr, nullptr, nullptr};

    explicit IdBatch(const std::vector<std::int64_t>& values) {
        id_buffers[1] = values.data();
        id_array.length = array.length = static_cast<std::int64_t>(values.size());
    }
    IdBatch(const IdBatch&) = delete;
    IdBatch& operator=(const IdBatch&) = delete;
};

}  // namespace

int main() {
    const ScratchDir scratch{"nano_lance_example_file"};
    const auto path = scratch.path / "ids.lance";
    const std::vector<std::int64_t> ids = {10, 20, 30, 40, 50};
    std::string error;

    // Step 1: write the batch as one .lance file. The extras say where the file goes.
    IdBatch batch(ids);
    nano_lance::LanceFileExtras extras;
    extras.path = path;
    std::uint64_t file_size = 0;
    if (!nano_lance::write_lance_file(batch.schema, {&batch.array}, extras, file_size, error)) {
        return fail("write_lance_file: " + error);
    }
    std::cout << "wrote " << file_size << " bytes\n";

    // Step 2: the footer and the schema. The file is format 2.2 and holds one column named id.
    nano_lance::pb::FileDescriptor descriptor;
    nano_lance::LanceDataFileFooterLayout layout;
    if (!nano_lance::read_lance_data_file_footer_and_descriptor(path, descriptor, layout, error)) {
        return fail("read footer: " + error);
    }
    std::cout << "format " << layout.major_version << "." << layout.minor_version << ", " << layout.num_columns
              << " column(s)\n";
    if (layout.major_version != 2 || layout.minor_version != 2 || layout.num_columns != 1 ||
        descriptor.fields.size() != 1 || descriptor.fields[0].name != "id") {
        return fail("the file should be format 2.2 with one column named id");
    }

    // Step 3: the column's metadata (where its pages are), then decode the column. DataFileIo reads the bytes.
    std::vector<nano_lance::pb::ColumnMetadata> columns;
    if (!nano_lance::read_lance_data_file_column_metadatas(path, layout, columns, error)) {
        return fail("read column metadata: " + error);
    }
    nano_lance::DataFileIo io(path);
    nano_lance::ColumnValues values;
    if (!nano_lance::decode_lance_physical_column(io, descriptor.fields[0], columns[0], values, error)) {
        return fail("decode_lance_physical_column: " + error);
    }

    // Step 4: the decoded values are the ids that were written.
    std::vector<std::int64_t> got(values.fixed.size() / sizeof(std::int64_t));
    std::memcpy(got.data(), values.fixed.data(), got.size() * sizeof(std::int64_t));
    if (got != ids) {
        return fail("the decoded ids differ from the written ids");
    }
    std::cout << "decoded " << got.size() << " values, equal to the input\n";
    return 0;
}
