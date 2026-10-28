// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The table reader's C entry points that the other api tests leave out: the one-shot reads
// (nano_lance_table_read_dataset, _ex, _projected, _range), the row take, and the ranged stream. The
// dataset is written here with the C writer, so every value checked is one this test put there.

#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& what) {
    if (!ok) {
        throw std::runtime_error(what);
    }
}

/// A fresh directory with a name nothing else uses; removed when the test ends, however it ends.
class ScratchDir {
  public:
    ScratchDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("nano_lance_table_reader_c_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        require(std::filesystem::create_directory(path_), "scratch directory already exists");
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    [[nodiscard]] std::filesystem::path path() const { return path_; }

  private:
    std::filesystem::path path_;
};

constexpr std::int64_t kRowsPerFragment = 500;
constexpr int kFragments = 4;
constexpr std::int64_t kRows = kRowsPerFragment * kFragments;

bool build_schema(ArrowSchema& schema) {
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, 2) != NANOARROW_OK) {
        return false;
    }
    return ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK &&
           ArrowSchemaSetName(schema.children[0], "id") == NANOARROW_OK &&
           ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_STRING) == NANOARROW_OK &&
           ArrowSchemaSetName(schema.children[1], "tag") == NANOARROW_OK;
}

// Rows [first, first + count): id = row number, tag = "tag-<id>".
bool build_batch(ArrowArray& array, const ArrowSchema& schema, std::int64_t first, std::int64_t count) {
    if (ArrowArrayInitFromSchema(&array, &schema, nullptr) != NANOARROW_OK ||
        ArrowArrayStartAppending(&array) != NANOARROW_OK) {
        ArrowArrayRelease(&array);
        return false;
    }
    for (std::int64_t i = first; i < first + count; ++i) {
        const std::string tag = "tag-" + std::to_string(i);
        const ArrowStringView sv{tag.c_str(), static_cast<std::int64_t>(tag.size())};
        if (ArrowArrayAppendInt(array.children[0], i) != NANOARROW_OK ||
            ArrowArrayAppendString(array.children[1], sv) != NANOARROW_OK ||
            ArrowArrayFinishElement(&array) != NANOARROW_OK) {
            ArrowArrayRelease(&array);
            return false;
        }
    }
    return ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK;
}

// One commit per fragment, so the dataset has kFragments data files.
void write_dataset(const std::filesystem::path& path, const ArrowSchema& schema) {
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, path.string().c_str(), 0) == NANO_LANCE_OK, "writer init failed");
    for (int fragment = 0; fragment < kFragments; ++fragment) {
        ArrowArray batch{};
        require(build_batch(batch, schema, fragment * kRowsPerFragment, kRowsPerFragment), "batch build failed");
        ArrowSchema batch_schema{};
        require(ArrowSchemaDeepCopy(&schema, &batch_schema) == NANOARROW_OK, "schema copy failed");
        const int rc = nano_lance_write_batch(&writer, &batch, &batch_schema);
        require(rc == NANO_LANCE_OK, std::string("write_batch: ") + nano_lance_writer_last_error(&writer));
        ArrowArrayRelease(&batch);
        ArrowSchemaRelease(&batch_schema);
        require(nano_lance_writer_commit(&writer, fragment != 0) == NANO_LANCE_OK,
                std::string("commit: ") + nano_lance_writer_last_error(&writer));
    }
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "writer close failed");
}

/// The result of a one-shot read; releases what it holds when it goes out of scope.
struct Read {
    ArrowSchema schema{};
    ArrowArray* batches = nullptr;
    size_t count = 0;
    char error[512]{};
    ~Read() { nano_lance_table_read_result_free(&schema, batches, count); }
};

/// The `id` values of every batch, in the order they come back; each batch's first column is `id`.
std::vector<std::int64_t> ids_of(const Read& read) {
    std::vector<std::int64_t> ids;
    for (size_t b = 0; b < read.count; ++b) {
        const ArrowArray& batch = read.batches[b];
        require(batch.n_children >= 1, "batch has no columns");
        const ArrowArray& column = *batch.children[0];
        const auto* values = static_cast<const std::int64_t*>(column.buffers[1]) + column.offset;
        for (std::int64_t i = 0; i < batch.length; ++i) {
            ids.push_back(values[i]);
        }
    }
    return ids;
}

std::vector<std::int64_t> id_range(std::int64_t first, std::int64_t count) {
    std::vector<std::int64_t> ids;
    for (std::int64_t i = first; i < first + count; ++i) {
        ids.push_back(i);
    }
    return ids;
}

std::int64_t drain_ids(ArrowArrayStream& stream, std::vector<std::int64_t>& ids) {
    for (;;) {
        ArrowArray batch{};
        require(stream.get_next(&stream, &batch) == 0, "get_next failed");
        if (batch.release == nullptr) {
            break;
        }
        const auto* values = static_cast<const std::int64_t*>(batch.children[0]->buffers[1]) +
                             batch.children[0]->offset;
        for (std::int64_t i = 0; i < batch.length; ++i) {
            ids.push_back(values[i]);
        }
        ArrowArrayRelease(&batch);
    }
    return static_cast<std::int64_t>(ids.size());
}

void check_whole_read(Read& read, std::int64_t first, std::int64_t rows, const char* what) {
    require(read.schema.n_children == 2, std::string(what) + ": schema should keep both columns");
    require(ids_of(read) == id_range(first, rows), std::string(what) + ": rows are not the ones written");
}

/// Every argument the C header says is required, and a missing dataset, fail by status with a message.
void test_bad_arguments(const std::string& path) {
    ArrowSchema schema{};
    ArrowArray* batches = nullptr;
    size_t count = 0;
    char err[256]{};
    const char* p = path.c_str();

    require(nano_lance_table_read_dataset(p, nullptr, &batches, &count, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "null out_schema must be INVALID_ARGUMENT");
    require(nano_lance_table_read_dataset(p, &schema, nullptr, &count, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "null out_batches must be INVALID_ARGUMENT");
    require(nano_lance_table_read_dataset(p, &schema, &batches, nullptr, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "null out_batch_count must be INVALID_ARGUMENT");
    require(nano_lance_table_read_dataset(nullptr, &schema, &batches, &count, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "null dataset_path must be INVALID_ARGUMENT");
    require(std::strlen(err) > 0, "a null-argument failure must say what was null");

    // A projection that names no columns, or a column_names array with a null entry, is refused.
    const char* names[] = {"id", nullptr};
    require(nano_lance_table_read_dataset_projected(p, nullptr, 1, 0, &schema, &batches, &count, err,
                                                    sizeof err) == NANO_LANCE_READER_INVALID_ARGUMENT,
            "projected with NULL names must be INVALID_ARGUMENT");
    require(nano_lance_table_read_dataset_projected(p, names, 0, 0, &schema, &batches, &count, err,
                                                    sizeof err) == NANO_LANCE_READER_INVALID_ARGUMENT,
            "projected with zero columns must be INVALID_ARGUMENT");
    require(nano_lance_table_read_dataset_projected(p, names, 2, 0, &schema, &batches, &count, err,
                                                    sizeof err) == NANO_LANCE_READER_INVALID_ARGUMENT,
            "a null column name must be INVALID_ARGUMENT");
    require(nano_lance_table_read_dataset_range(p, nullptr, 1, 0, -1, 0, &schema, &batches, &count, err,
                                                sizeof err) == NANO_LANCE_READER_INVALID_ARGUMENT,
            "range with NULL names and a nonzero count must be INVALID_ARGUMENT");

    // Indices that are NULL while the count says there are some.
    require(nano_lance_table_take(p, nullptr, 0, nullptr, 2, 0, &schema, &batches, &count, err, sizeof err) ==
                NANO_LANCE_READER_INVALID_ARGUMENT,
            "take with NULL indices and a nonzero count must be INVALID_ARGUMENT");

    // A dataset that is not there: a status, a message, and nothing handed back.
    const std::string missing = path + "_missing";
    const int rc = nano_lance_table_read_dataset(missing.c_str(), &schema, &batches, &count, err, sizeof err);
    require(rc != NANO_LANCE_READER_OK, "a missing dataset must not read");
    require(std::strlen(err) > 0, "a missing dataset must say why");
    require(batches == nullptr && count == 0, "a failed read must hand back no batches");

    ArrowArrayStream stream{};
    require(nano_lance_table_open_stream_range(missing.c_str(), nullptr, 0, 0, -1, 0, &stream, err, sizeof err) !=
                NANO_LANCE_READER_OK,
            "open_stream_range on a missing dataset must fail");
    require(stream.release == nullptr, "a failed open_stream_range must leave the stream zeroed");
}

/// Whole-dataset reads, the projected and ranged reads, and the take, against the dataset written above.
void test_reads(const std::string& path) {
    const char* p = path.c_str();
    {
        Read r;
        require(nano_lance_table_read_dataset(p, &r.schema, &r.batches, &r.count, r.error, sizeof r.error) ==
                    NANO_LANCE_READER_OK,
                std::string("read_dataset: ") + r.error);
        check_whole_read(r, 0, kRows, "read_dataset");
    }
    {
        Read r;
        require(nano_lance_table_read_dataset_ex(p, 1, &r.schema, &r.batches, &r.count, r.error, sizeof r.error) ==
                    NANO_LANCE_READER_OK,
                std::string("read_dataset_ex: ") + r.error);
        check_whole_read(r, 0, kRows, "read_dataset_ex (trusted)");
    }
    {
        // A projection keeps only the named column; the other is not decoded at all.
        const char* names[] = {"tag"};
        Read r;
        require(nano_lance_table_read_dataset_projected(p, names, 1, 0, &r.schema, &r.batches, &r.count, r.error,
                                                        sizeof r.error) == NANO_LANCE_READER_OK,
                std::string("projected: ") + r.error);
        require(r.schema.n_children == 1 && std::string(r.schema.children[0]->name) == "tag",
                "projection must keep only the named column");
        std::int64_t rows = 0;
        for (size_t b = 0; b < r.count; ++b) {
            rows += r.batches[b].length;
        }
        require(rows == kRows, "projection must keep every row");
    }
    {
        // An unknown name is an error, not an empty column. The error is a caller mistake, so it is
        // an argument error, not an I/O one.
        const char* names[] = {"no_such_column"};
        Read r;
        const int rc = nano_lance_table_read_dataset_projected(p, names, 1, 0, &r.schema, &r.batches, &r.count,
                                                               r.error, sizeof r.error);
        require(rc != NANO_LANCE_READER_OK, "an unknown projected column must fail");
        require(std::strlen(r.error) > 0, "an unknown projected column must say so");
        require(r.schema.release == nullptr, "a failed projected read must leave the schema released");
        require(rc == NANO_LANCE_READER_INVALID_ARGUMENT, "an unknown projected column is INVALID_ARGUMENT");
    }
    {
        // A range: rows [600, 850), read as the fragments they fall in.
        Read r;
        require(nano_lance_table_read_dataset_range(p, nullptr, 0, 600, 250, 0, &r.schema, &r.batches, &r.count,
                                                    r.error, sizeof r.error) == NANO_LANCE_READER_OK,
                std::string("range: ") + r.error);
        require(ids_of(r) == id_range(600, 250), "range [600, 850) must hold exactly those rows");
    }
    {
        // A negative length reads to the end of the dataset.
        Read r;
        require(nano_lance_table_read_dataset_range(p, nullptr, 0, 1500, -1, 0, &r.schema, &r.batches, &r.count,
                                                    r.error, sizeof r.error) == NANO_LANCE_READER_OK,
                std::string("range to end: ") + r.error);
        require(ids_of(r) == id_range(1500, 500), "a negative length must read to the end");
    }
    {
        // An empty range is a successful read with nothing in it.
        Read r;
        require(nano_lance_table_read_dataset_range(p, nullptr, 0, 0, 0, 0, &r.schema, &r.batches, &r.count,
                                                    r.error, sizeof r.error) == NANO_LANCE_READER_OK,
                std::string("empty range: ") + r.error);
        require(r.count == 0 && r.batches == nullptr, "an empty range must hand back no batches");
        require(r.schema.n_children == 2, "an empty range must still carry the schema");
    }
    {
        // take: the rows at these indices, ascending, each once, whatever order or repeats they came in.
        const std::uint64_t indices[] = {1999, 0, 750, 3, 750};
        const char* names[] = {"id"};
        Read r;
        require(nano_lance_table_take(p, names, 1, indices, 5, 0, &r.schema, &r.batches, &r.count, r.error,
                                      sizeof r.error) == NANO_LANCE_READER_OK,
                std::string("take: ") + r.error);
        require(ids_of(r) == std::vector<std::int64_t>({0, 3, 750, 1999}),
                "take must return the requested rows in ascending order, each once");
        require(r.schema.n_children == 1, "take must honour the projection");
    }
    {
        // An index past the end is refused.
        const std::uint64_t indices[] = {kRows};
        Read r;
        require(nano_lance_table_take(p, nullptr, 0, indices, 1, 0, &r.schema, &r.batches, &r.count, r.error,
                                      sizeof r.error) != NANO_LANCE_READER_OK,
                "take of a row past the end must fail");
    }
    {
        // Nothing to take is a successful, empty read.
        Read r;
        require(nano_lance_table_take(p, nullptr, 0, nullptr, 0, 0, &r.schema, &r.batches, &r.count, r.error,
                                      sizeof r.error) == NANO_LANCE_READER_OK,
                "take of no rows must succeed");
        require(r.count == 0, "take of no rows must hand back no batches");
    }
}

/// The ranged stream: the rows it yields are exactly the range, and the stream is released by the caller.
void test_ranged_stream(const std::string& path) {
    ArrowArrayStream stream{};
    char err[256]{};
    require(nano_lance_table_open_stream_range(path.c_str(), nullptr, 0, 1000, 500, 0, &stream, err, sizeof err) ==
                NANO_LANCE_READER_OK,
            std::string("open_stream_range: ") + err);
    ArrowSchema schema{};
    require(stream.get_schema(&stream, &schema) == 0, "get_schema failed");
    require(schema.n_children == 2, "ranged stream lost a column");
    ArrowSchemaRelease(&schema);

    std::vector<std::int64_t> ids;
    drain_ids(stream, ids);
    require(ids == id_range(1000, 500), "ranged stream must yield exactly rows [1000, 1500)");
    stream.release(&stream);

    // A projected ranged stream keeps only the named column.
    const char* names[] = {"id"};
    require(nano_lance_table_open_stream_range(path.c_str(), names, 1, 2000 - 10, -1, 0, &stream, err, sizeof err) ==
                NANO_LANCE_READER_OK,
            std::string("projected open_stream_range: ") + err);
    require(stream.get_schema(&stream, &schema) == 0, "get_schema failed");
    require(schema.n_children == 1, "projected ranged stream must keep one column");
    ArrowSchemaRelease(&schema);
    ids.clear();
    drain_ids(stream, ids);
    require(ids == id_range(kRows - 10, 10), "a range to the end must yield its tail");
    stream.release(&stream);
}

void run() {
    ScratchDir scratch;
    const auto dataset = scratch.path() / "dataset.lance";

    ArrowSchema schema{};
    require(build_schema(schema), "schema build failed");
    write_dataset(dataset, schema);
    ArrowSchemaRelease(&schema);

    const std::string path = dataset.string();
    test_bad_arguments(path);
    test_reads(path);
    test_ranged_stream(path);
}

}  // namespace

int main() {
    try {
        run();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "table reader C API tests passed\n";
    return 0;
}
