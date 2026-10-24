// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// An index's own Lance files (index/index_files.hpp), as the index searches read them: write_file records
// a file's name and size, read_table and take_table read a range or rows of it (with a projection), a
// read of a missing file names the file, schema metadata is found by key, the cache key of a file changes
// when the file is rewritten, the bloom filter and zone map parameters are read from their files, and the
// bloom filter hashes a value's bytes as Lance does.

#include "index/index_files.hpp"

#include "nanolance/bloom_filter.hpp"
#include "nanolance/data_file_reader.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace ix = nano_lance::index_files;

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::filesystem::path scratch() {
    auto p = std::filesystem::temp_directory_path() / "nano_lance_index_files_test";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p);
    return p;
}

// A struct of "id" (int64) and "name" (utf8).
void make_schema(ArrowSchema& schema) {
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, 2) == NANOARROW_OK, "struct schema");
    require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK, "int64 column");
    require(ArrowSchemaSetName(schema.children[0], "id") == NANOARROW_OK, "id name");
    require(ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_STRING) == NANOARROW_OK, "utf8 column");
    require(ArrowSchemaSetName(schema.children[1], "name") == NANOARROW_OK, "name name");
}

void make_batch(std::int64_t first, std::int64_t count, const ArrowSchema& schema, ArrowArray& array) {
    ArrowError err{};
    require(ArrowArrayInitFromSchema(&array, &schema, &err) == NANOARROW_OK, std::string("array: ") + err.message);
    require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start appending");
    for (std::int64_t i = first; i < first + count; ++i) {
        require(ArrowArrayAppendInt(array.children[0], i) == NANOARROW_OK, "append id");
        const std::string name = "name-" + std::to_string(i);
        const ArrowStringView text = {name.data(), static_cast<std::int64_t>(name.size())};
        require(ArrowArrayAppendString(array.children[1], text) == NANOARROW_OK, "append name");
    }
    array.length = count;
    require(ArrowArrayFinishBuildingDefault(&array, &err) == NANOARROW_OK, std::string("finish: ") + err.message);
}

std::vector<std::int64_t> ids(const ix::FileTable& table) {
    std::vector<std::int64_t> out;
    for (const auto& batch : table.batches) {
        const ArrowArray* col = batch.children[0];
        for (std::int64_t r = 0; r < col->length; ++r) {
            out.push_back(static_cast<const std::int64_t*>(col->buffers[1])[col->offset + r]);
        }
    }
    return out;
}

std::string text_of(const std::vector<std::uint8_t>& b) { return {b.begin(), b.end()}; }

}  // namespace

int main() {
    const auto dir = scratch();
    std::string error;

    ArrowSchema schema{};
    make_schema(schema);
    ArrowArray batch{};
    make_batch(0, 5, schema, batch);  // ids 0..4

    // write_file: the file is written and recorded with its size; the metadata and a global buffer are in it.
    nano_lance::LanceFileExtras extras;
    extras.schema_metadata["owner"] = {'v', '1'};
    extras.global_buffers = {{'g', 'b'}};
    std::vector<ix::WrittenFile> files;
    require(ix::write_file(dir, "index.lance", schema, batch, extras, files, error), "write_file: " + error);
    require(files.size() == 1 && files[0].name == "index.lance", "the file is recorded by its name");
    const auto file = dir / "index.lance";
    require(files[0].size > 0 && files[0].size == std::filesystem::file_size(file), "the recorded size is the file's");

    // read_table: every row, a range, and a projection.
    {
        ix::FileTable table;
        require(ix::read_table(file, nullptr, nano_lance::LanceRowRange{}, table, error), "read: " + error);
        require(ids(table) == std::vector<std::int64_t>({0, 1, 2, 3, 4}), "every row, in order");
        require(std::string(table.type(0).format) == "l", "the id column is int64");
    }
    {
        ix::FileTable table;
        nano_lance::LanceRowRange range;
        range.offset = 1;
        range.length = 3;
        require(ix::read_table(file, nullptr, range, table, error), "ranged read: " + error);
        require(ids(table) == std::vector<std::int64_t>({1, 2, 3}), "the range [1, 4)");
    }
    {
        const std::vector<std::string> only_name = {"name"};
        ix::FileTable table;
        require(ix::read_table(file, &only_name, nano_lance::LanceRowRange{}, table, error), "projected: " + error);
        require(table.schema.n_children == 1 && std::string(table.schema.children[0]->name) == "name",
                "only the projected column");
    }

    // take_table: the rows asked for. A row past the end is refused, with the file's name.
    {
        ix::FileTable table;
        require(ix::take_table(file, nullptr, {0, 4}, table, error), "take: " + error);
        require(ids(table) == std::vector<std::int64_t>({0, 4}), "the rows 0 and 4");
        ix::FileTable bad;
        require(!ix::take_table(file, nullptr, {5}, bad, error), "a row past the end is refused");
        require(error.find("index.lance") != std::string::npos, "the refusal names the file: " + error);
    }

    // A missing file: refused, and the message names it.
    {
        ix::FileTable table;
        require(!ix::read_table(dir / "missing.lance", nullptr, nano_lance::LanceRowRange{}, table, error),
                "a missing file is refused");
        require(error.find("missing.lance") != std::string::npos, "the message names it: " + error);
    }

    // Schema metadata by key: found with its value; an absent key is not found, and that is not an error.
    {
        std::string value;
        bool found = false;
        nano_lance::LanceDataFileFooterLayout layout{};
        require(ix::schema_metadata(file, "owner", value, found, layout, error), "metadata: " + error);
        require(found && value == "v1", "the owner metadata is v1");
        require(ix::schema_metadata(file, "absent", value, found, layout, error) && !found,
                "an absent key is not found, without an error");
        // The layout the footer gives reads the global buffers the file has.
        std::vector<std::uint8_t> buffer;
        require(nano_lance::read_lance_file_global_buffer(file, layout, 1, buffer, error), "buffer: " + error);
        require(text_of(buffer) == "gb", "the global buffer is as written");
    }

    // The cache key: stable for an unchanged file, different once the file is written again at another size.
    {
        const auto before = ix::file_key(file);
        require(before == ix::file_key(file), "the key of an unchanged file is stable");
        ArrowArray bigger{};
        make_batch(0, 50, schema, bigger);
        std::vector<ix::WrittenFile> again;
        require(ix::write_file(dir, "index.lance", schema, bigger, nano_lance::LanceFileExtras{}, again, error),
                "rewrite: " + error);
        require(ix::file_key(file) != before, "a rewritten file has a new key");
        ArrowArrayRelease(&bigger);
    }

    // The parameters of a bloom filter and a zone map come from their files' metadata.
    {
        ArrowArray tiny{};
        make_batch(0, 1, schema, tiny);
        nano_lance::LanceFileExtras bloom;
        bloom.schema_metadata["bloomfilter_item"] = {'8', '1', '9', '2'};
        bloom.schema_metadata["bloomfilter_probability"] = {'0', '.', '0', '0', '0', '5', '7'};
        std::vector<ix::WrittenFile> written;
        require(ix::write_file(dir, "bloomfilter.lance", schema, tiny, bloom, written, error), "bloom file: " + error);
        std::uint64_t items = 0;
        double probability = 0.0;
        require(ix::bloomfilter_params(dir, items, probability, error), "bloom params: " + error);
        require(items == 8192 && probability == 0.00057, "8192 items at 0.00057");

        nano_lance::LanceFileExtras zone;
        zone.schema_metadata["rows_per_zone"] = {'4', '0', '9', '6'};
        require(ix::write_file(dir, "zonemap.lance", schema, tiny, zone, written, error), "zone file: " + error);
        std::uint64_t rows_per_zone = 0;
        require(ix::zonemap_rows_per_zone(dir, rows_per_zone, error), "zone params: " + error);
        require(rows_per_zone == 4096, "4096 rows per zone");
        ArrowArrayRelease(&tiny);

        // A directory with no bloom filter file has no parameters to read: refused.
        std::uint64_t none = 0;
        double none_p = 0.0;
        require(!ix::bloomfilter_params(dir / "absent", none, none_p, error), "no bloom file, no parameters");
    }

    // The bloom filter hashes a value's bytes: fixed-width ones at their width, strings and binaries by
    // their bytes, and a type Lance's filter does not take is refused.
    {
        ArrowSchema int_schema{};
        ArrowSchemaInit(&int_schema);
        require(ArrowSchemaSetType(&int_schema, NANOARROW_TYPE_INT64) == NANOARROW_OK, "int64 schema");
        ArrowArray ints{};
        ArrowError err{};
        require(ArrowArrayInitFromSchema(&ints, &int_schema, &err) == NANOARROW_OK, "int array");
        require(ArrowArrayStartAppending(&ints) == NANOARROW_OK, "int append start");
        require(ArrowArrayAppendInt(&ints, 42) == NANOARROW_OK, "append 42");
        require(ArrowArrayAppendInt(&ints, 7) == NANOARROW_OK, "append 7");
        require(ArrowArrayFinishBuildingDefault(&ints, &err) == NANOARROW_OK, "finish ints");
        ArrowArrayView view{};
        require(ArrowArrayViewInitFromSchema(&view, &int_schema, &err) == NANOARROW_OK, "view");
        require(ArrowArrayViewSetArray(&view, &ints, &err) == NANOARROW_OK, "view array");

        ArrowSchemaView sview{};
        require(ArrowSchemaViewInit(&sview, &int_schema, &err) == NANOARROW_OK, "schema view");
        std::size_t width = 0;
        require(ix::bloom_value_width(sview, width, error) && width == 8, "int64 hashes 8 bytes");
        const std::uint8_t forty_two[8] = {42, 0, 0, 0, 0, 0, 0, 0};
        require(ix::bloom_hash(&view, 0, width) == nano_lance::bloom_filter::xxh64(forty_two, 8),
                "element 0 hashes its little-endian bytes");
        require(ix::bloom_hash(&view, 1, width) == nano_lance::bloom_filter::xxh64(
                                                       std::vector<std::uint8_t>{7, 0, 0, 0, 0, 0, 0, 0}.data(), 8),
                "element 1 hashes its own bytes");
        ArrowArrayViewReset(&view);
        ArrowArrayRelease(&ints);

        ArrowSchema bool_schema{};
        ArrowSchemaInit(&bool_schema);
        require(ArrowSchemaSetType(&bool_schema, NANOARROW_TYPE_BOOL) == NANOARROW_OK, "bool schema");
        ArrowSchemaView bview{};
        require(ArrowSchemaViewInit(&bview, &bool_schema, &err) == NANOARROW_OK, "bool schema view");
        std::size_t unused = 0;
        require(!ix::bloom_value_width(bview, unused, error), "a boolean column has no bloom filter");
        require(error.find("not supported") != std::string::npos, "the refusal says so: " + error);
        ArrowSchemaRelease(&bool_schema);
        ArrowSchemaRelease(&int_schema);
    }

    ArrowArrayRelease(&batch);
    ArrowSchemaRelease(&schema);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    std::cout << "index_files: ok (write, read, take, metadata, file keys, bloom and zone parameters, hashes)\n";
    return 0;
}
