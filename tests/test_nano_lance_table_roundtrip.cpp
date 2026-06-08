#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_table_rt_" + std::string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

std::size_t utf8_diff_count(const ArrowArray& left, const ArrowArray& right) {
    if (left.length != right.length || left.buffers == nullptr || right.buffers == nullptr) {
        return static_cast<std::size_t>(-1);
    }
    const auto* l_off = static_cast<const std::int32_t*>(left.buffers[1]);
    const auto* r_off = static_cast<const std::int32_t*>(right.buffers[1]);
    const auto* l_data = static_cast<const char*>(left.buffers[2]);
    const auto* r_data = static_cast<const char*>(right.buffers[2]);
    std::size_t diffs = 0;
    for (int64_t i = 0; i < left.length; ++i) {
        const auto l_len = l_off[i + 1] - l_off[i];
        const auto r_len = r_off[i + 1] - r_off[i];
        if (l_len != r_len || std::memcmp(l_data + l_off[i], r_data + r_off[i], static_cast<std::size_t>(l_len)) != 0) {
            ++diffs;
        }
    }
    return diffs;
}

/** Lance reader returns struct-typed batches when the table has top-level fields; unwrap first child if present. */
const ArrowArray* first_column_array(const ArrowArray& batch) {
    if (batch.n_children > 0 && batch.children != nullptr && batch.children[0] != nullptr) {
        return batch.children[0];
    }
    return &batch;
}

void test_int64_roundtrip() {
    const auto ds = temp_dataset("int64");
    ArrowArray source{};
    ArrowSchema field{};
    field.format = "l";
    field.name = "x";
    field.flags = 0;
    const std::int64_t values[] = {10, 11, 12, 13, 14, 15, 16};
    const void* buffers[] = {nullptr, values};
    source.length = 7;
    source.n_buffers = 2;
    source.buffers = buffers;

    {
        NanoLanceWriter writer{};
        require(nano_lance_writer_init(&writer, ds.string().c_str(), 0) == NANO_LANCE_OK, "init");
        require(nano_lance_write_batch(&writer, &source, &field) == NANO_LANCE_OK, "write");
        require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    }

    ArrowSchema read_schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, read_schema, batches, error), error.c_str());
    require(batches.size() == 1U, "expected one batch");
    const ArrowArray* col = first_column_array(batches[0]);
    require(col->length == source.length, "row count mismatch");
    const auto* read_vals = static_cast<const std::int64_t*>(col->buffers[1]);
    for (int64_t i = 0; i < source.length; ++i) {
        require(read_vals[i] == values[static_cast<std::size_t>(i)], "int64 value mismatch");
    }

    ArrowSchemaRelease(&read_schema);
    ArrowArrayRelease(&batches[0]);
}

void test_utf8_roundtrip() {
    const auto ds = temp_dataset("utf8");
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetFormat(&schema, "u") == NANOARROW_OK, "schema format");
    require(ArrowSchemaSetName(&schema, "label") == NANOARROW_OK, "schema name");

    ArrowArray source{};
    require(ArrowArrayInitFromSchema(&source, &schema, nullptr) == NANOARROW_OK, "array init");
    require(ArrowArrayStartAppending(&source) == NANOARROW_OK, "append start");
    require(ArrowArrayAppendString(&source, {"alpha", 5}) == NANOARROW_OK, "a");
    require(ArrowArrayAppendString(&source, {"beta", 4}) == NANOARROW_OK, "b");
    require(ArrowArrayAppendString(&source, {"gamma", 5}) == NANOARROW_OK, "g");
    require(ArrowArrayFinishBuildingDefault(&source, nullptr) == NANOARROW_OK, "finish");

    {
        NanoLanceWriter writer{};
        require(nano_lance_writer_init(&writer, ds.string().c_str(), 0) == NANO_LANCE_OK, "init");
        require(nano_lance_writer_set_ignore_nullability(&writer, true) == NANO_LANCE_OK, "ignore nullability");
        require(nano_lance_write_batch(&writer, &source, &schema) == NANO_LANCE_OK, "write");
        require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    }

    ArrowSchema read_schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, read_schema, batches, error), error.c_str());
    require(batches.size() == 1U, "expected one batch");
    const ArrowArray* read_col = first_column_array(batches[0]);
    require(utf8_diff_count(source, *read_col) == 0U, "source-read diff must be zero for utf8");

    ArrowSchemaRelease(&schema);
    ArrowSchemaRelease(&read_schema);
    ArrowArrayRelease(&source);
    ArrowArrayRelease(&batches[0]);
}

}  // namespace

int main() {
    test_int64_roundtrip();
    test_utf8_roundtrip();
    return 0;
}
