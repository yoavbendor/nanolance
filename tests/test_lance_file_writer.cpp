// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// A standalone Lance file as write_lance_file writes it (what Lance's index files are): an int64 and a
// utf8 column over several batches, read back with lance_file_info, lance_file_read (whole, projected and
// ranged) and lance_file_take; the schema metadata and global buffers carried as LanceFileExtras asks;
// integers bit-packed when asked, and smaller for it; and a missing path refused with its reason.

#include "nanolance/data_file_reader.hpp"
#include "nanolance/lance_file_writer.hpp"
#include "nanolance/lance_table_reader.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

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

std::filesystem::path scratch() {
    auto p = std::filesystem::temp_directory_path() / "nano_lance_lance_file_writer_test";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p);
    return p;
}

// The schema of a struct of "id" (int64) and "name" (utf8).
void make_schema(ArrowSchema& schema) {
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, 2) == NANOARROW_OK, "struct schema");
    require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK, "int64 column");
    require(ArrowSchemaSetName(schema.children[0], "id") == NANOARROW_OK, "id name");
    require(ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_STRING) == NANOARROW_OK, "utf8 column");
    require(ArrowSchemaSetName(schema.children[1], "name") == NANOARROW_OK, "name name");
}

// A batch of `schema`, one row per entry of `ids` and `names`.
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

std::vector<std::uint8_t> bytes_of(const std::string& s) { return {s.begin(), s.end()}; }

std::string text_of(const std::vector<std::uint8_t>& b) { return {b.begin(), b.end()}; }

// The values of an int64 column, and of a utf8 column, over every batch in order.
std::vector<std::int64_t> ints(const std::vector<ArrowArray>& batches, std::int64_t column) {
    std::vector<std::int64_t> out;
    for (const auto& batch : batches) {
        const ArrowArray* col = batch.children[column];
        for (std::int64_t r = 0; r < col->length; ++r) {
            out.push_back(static_cast<const std::int64_t*>(col->buffers[1])[col->offset + r]);
        }
    }
    return out;
}

std::vector<std::string> texts(const std::vector<ArrowArray>& batches, std::int64_t column) {
    std::vector<std::string> out;
    for (const auto& batch : batches) {
        const ArrowArray* col = batch.children[column];
        const auto* offsets = static_cast<const std::int32_t*>(col->buffers[1]);
        const auto* data = static_cast<const char*>(col->buffers[2]);
        for (std::int64_t r = 0; r < col->length; ++r) {
            const auto begin = offsets[col->offset + r];
            const auto end = offsets[col->offset + r + 1];
            out.emplace_back(data + begin, static_cast<std::size_t>(end - begin));
        }
    }
    return out;
}

void release(ArrowSchema& schema, std::vector<ArrowArray>& batches) {
    for (auto& batch : batches) {
        if (batch.release != nullptr) {
            ArrowArrayRelease(&batch);
        }
    }
    batches.clear();
    if (schema.release != nullptr) {
        ArrowSchemaRelease(&schema);
    }
}

std::int64_t child_index(const ArrowSchema& schema, const std::string& name) {
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (name == schema.children[i]->name) {
            return i;
        }
    }
    return -1;
}

}  // namespace

int main() {
    const auto dir = scratch();
    const auto path = dir / "part.lance";

    // Two batches of ids 0..4 and 5..6 (names "n0".."n6"), with an empty batch and a null batch between: the
    // empty one is skipped, the null one is too.
    ArrowSchema schema{};
    make_schema(schema);
    ArrowArray first{};
    ArrowArray second{};
    ArrowArray empty{};
    make_batch({0, 1, 2, 3, 4}, {"n0", "n1", "n2", "n3", "n4"}, schema, first);
    make_batch({5, 6}, {"n5", "n6"}, schema, second);
    make_batch({}, {}, schema, empty);

    nano_lance::LanceFileExtras extras;
    extras.path = path;
    extras.schema_metadata["origin"] = bytes_of("unit-test");
    extras.global_buffers = {bytes_of("first-buffer"), bytes_of("second, longer buffer")};
    std::uint64_t written = 0;
    std::string error;
    const std::vector<const ArrowArray*> batches = {&first, nullptr, &empty, &second};
    require(nano_lance::write_lance_file(schema, batches, extras, written, error), "write: " + error);
    require(written > 0 && written == std::filesystem::file_size(path), "the size written is the file's size");

    // The file's info: rows, columns, schema metadata; and its schema.
    {
        nano_lance::LanceFileInfo info;
        ArrowSchema out_schema{};
        require(nano_lance::lance_file_info(path, info, out_schema, error), "info: " + error);
        require(info.num_rows == 7, "seven rows");
        require(info.num_columns == 2, "two columns");
        require(info.schema_metadata.count("origin") == 1 && info.schema_metadata.at("origin") == "unit-test",
                "the schema metadata is carried");
        require(out_schema.n_children == 2 && std::string(out_schema.children[0]->name) == "id" &&
                    std::string(out_schema.children[1]->name) == "name",
                "the schema has the columns in order");
        require(std::string(out_schema.children[0]->format) == "l", "id is int64");
        require(std::string(out_schema.children[1]->format) == "u", "name is utf8");
        ArrowSchemaRelease(&out_schema);
    }

    // A whole read: every row in order, across the batches written.
    {
        nano_lance::LanceScanRequest request;
        ArrowSchema out_schema{};
        std::vector<ArrowArray> out;
        require(nano_lance::lance_file_read(path, request, out_schema, out, error), "read: " + error);
        const auto id = child_index(out_schema, "id");
        const auto name = child_index(out_schema, "name");
        require(id >= 0 && name >= 0, "both columns are read");
        require(ints(out, id) == std::vector<std::int64_t>({0, 1, 2, 3, 4, 5, 6}), "the ids, in order");
        require(texts(out, name) == std::vector<std::string>({"n0", "n1", "n2", "n3", "n4", "n5", "n6"}),
                "the names, in order");
        release(out_schema, out);
    }

    // A projection reads one column; a range reads its rows.
    {
        const std::vector<std::string> only_name = {"name"};
        nano_lance::LanceScanRequest request;
        request.columns = &only_name;
        ArrowSchema out_schema{};
        std::vector<ArrowArray> out;
        require(nano_lance::lance_file_read(path, request, out_schema, out, error), "projected read: " + error);
        require(out_schema.n_children == 1 && std::string(out_schema.children[0]->name) == "name",
                "only the projected column is read");
        release(out_schema, out);
    }
    {
        nano_lance::LanceScanRequest request;
        request.range = {5, 2};
        ArrowSchema out_schema{};
        std::vector<ArrowArray> out;
        require(nano_lance::lance_file_read(path, request, out_schema, out, error), "ranged read: " + error);
        require(ints(out, child_index(out_schema, "id")) == std::vector<std::int64_t>({5, 6}),
                "the range [5, 7) reads rows 5 and 6");
        release(out_schema, out);
    }

    // A take reads the rows asked for, in ascending order.
    {
        nano_lance::LanceScanRequest request;
        ArrowSchema out_schema{};
        std::vector<ArrowArray> out;
        require(nano_lance::lance_file_take(path, request, {1, 6}, out_schema, out, error), "take: " + error);
        require(ints(out, child_index(out_schema, "id")) == std::vector<std::int64_t>({1, 6}), "the rows 1 and 6");
        require(texts(out, child_index(out_schema, "name")) == std::vector<std::string>({"n1", "n6"}),
                "their names");
        release(out_schema, out);

        ArrowSchema bad_schema{};
        std::vector<ArrowArray> bad;
        require(!nano_lance::lance_file_take(path, request, {7}, bad_schema, bad, error),
                "a row past the end is refused");
        require(!error.empty(), "the refusal says why");
    }

    // The global buffers: the footer names their count, and buffer 0 is the schema, so the two given ones
    // are 1 and 2.
    {
        nano_lance::pb::FileDescriptor descriptor;
        nano_lance::LanceDataFileFooterLayout layout{};
        require(nano_lance::read_lance_data_file_footer_and_descriptor(path, descriptor, layout, error),
                "footer: " + error);
        require(layout.num_global_buffers == 3, "the schema and two global buffers");
        std::vector<std::uint8_t> one;
        std::vector<std::uint8_t> two;
        require(nano_lance::read_lance_file_global_buffer(path, layout, 1, one, error), "buffer 1: " + error);
        require(nano_lance::read_lance_file_global_buffer(path, layout, 2, two, error), "buffer 2: " + error);
        require(text_of(one) == "first-buffer", "global buffer 1 is the first buffer given");
        require(text_of(two) == "second, longer buffer", "global buffer 2 is the second buffer given");
        require(!nano_lance::read_lance_file_global_buffer(path, layout, 3, one, error),
                "a global buffer past the last is refused");
    }

    // Bit-packing: 5000 integers below 8 need 3 bits each packed, and 8 bytes each without it.
    {
        std::vector<std::int64_t> ids;
        std::vector<std::string> names;
        for (std::int64_t i = 0; i < 5000; ++i) {
            ids.push_back(i % 8);
            names.push_back("x");
        }
        ArrowSchema packed_schema{};
        make_schema(packed_schema);
        ArrowArray packed_batch{};
        make_batch(ids, names, packed_schema, packed_batch);
        const std::vector<const ArrowArray*> one = {&packed_batch};

        nano_lance::LanceFileExtras plain;
        plain.path = dir / "plain.lance";
        std::uint64_t plain_size = 0;
        require(nano_lance::write_lance_file(packed_schema, one, plain, plain_size, error), "plain: " + error);

        nano_lance::LanceFileExtras bits = plain;
        bits.path = dir / "bits.lance";
        bits.bitpack_integers = true;
        std::uint64_t bits_size = 0;
        require(nano_lance::write_lance_file(packed_schema, one, bits, bits_size, error), "packed: " + error);
        require(bits_size < plain_size, "bit-packed integers take less space");

        nano_lance::LanceScanRequest request;
        ArrowSchema out_schema{};
        std::vector<ArrowArray> out;
        require(nano_lance::lance_file_read(bits.path, request, out_schema, out, error), "packed read: " + error);
        require(ints(out, child_index(out_schema, "id")) == ids, "the packed ids read back exactly");
        release(out_schema, out);
        ArrowSchemaRelease(&packed_schema);
        ArrowArrayRelease(&packed_batch);
    }

    // A file needs a path: refused with the reason, and nothing written.
    {
        nano_lance::LanceFileExtras no_path;
        std::uint64_t size = 99;
        const std::vector<const ArrowArray*> one = {&first};
        require(!nano_lance::write_lance_file(schema, one, no_path, size, error), "a file without a path is refused");
        require(error == "write_lance_file needs a path", "the message: " + error);
        require(size == 0, "no size for a file not written");
    }

    ArrowArrayRelease(&first);
    ArrowArrayRelease(&second);
    ArrowArrayRelease(&empty);
    ArrowSchemaRelease(&schema);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    std::cout << "lance_file_writer: ok (write, info, whole/projected/ranged read, take, global buffers, bit-packing)\n";
    return 0;
}
