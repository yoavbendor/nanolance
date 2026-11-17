// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// FSST on a FullZip utf8 page (roadmap F4): a column of compressible text with nulls, about 60 KB of values, so one
// page that the writer compresses value by value against the column's table. Written with nano_lance::Writer; the
// page's descriptor must say FullZip over Fsst (not plain Variable), and the C table reader must give back every
// string and every null.

#include "nanolance/data_file_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/nano_lance_reader.h"
#include "nanolance/page_layout.hpp"
#include "nanolance/writer.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
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

/// A fresh directory, removed when the test ends however it ends.
class ScratchDir {
  public:
    ScratchDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("nano_lance_fullzip_fsst_" + std::to_string(rd()) + "_" + std::to_string(rd()));
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

constexpr std::int64_t kRows = 100;
constexpr std::int64_t kNullEvery = 10;  // every tenth row is null

/// Log-like sentences over a small vocabulary: ~600 bytes a row, compressible, so FSST pays on the page.
std::vector<std::string> sentences() {
    const std::vector<std::string> words = {"request", "user",    "session", "timeout", "retry",   "connected",
                                            "host",    "port",    "error",   "warning", "latency", "cache",
                                            "miss",    "hit",     "queue",   "worker",  "batch",   "flush"};
    std::mt19937 rng(7);
    std::vector<std::string> out;
    for (std::int64_t r = 0; r < kRows; ++r) {
        std::string text;
        while (text.size() < 600) {
            text += words[rng() % words.size()];
            text += ' ';
        }
        out.push_back(text);
    }
    return out;
}

/// A root struct with one utf8 child "s": every tenth row is null.
struct Batch {
    ArrowSchema schema{};
    ArrowArray array{};
    explicit Batch(const std::vector<std::string>& values) {
        ArrowSchemaInit(&schema);
        require(ArrowSchemaSetTypeStruct(&schema, 1) == NANOARROW_OK, "root struct");
        require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_STRING) == NANOARROW_OK, "utf8 type");
        require(ArrowSchemaSetName(schema.children[0], "s") == NANOARROW_OK, "utf8 name");
        require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array init");
        require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start appending");
        ArrowArray* s = array.children[0];
        for (std::int64_t r = 0; r < kRows; ++r) {
            if (r % kNullEvery == kNullEvery - 1) {
                require(ArrowArrayAppendNull(s, 1) == NANOARROW_OK, "null row");
            } else {
                const auto& text = values[static_cast<std::size_t>(r)];
                const ArrowStringView view{text.data(), static_cast<std::int64_t>(text.size())};
                require(ArrowArrayAppendString(s, view) == NANOARROW_OK, "string row");
            }
            require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "root element");
        }
        require(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK, "finish array");
    }
    ~Batch() {
        ArrowArrayRelease(&array);
        ArrowSchemaRelease(&schema);
    }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
};

bool bit(const void* bitmap, std::int64_t i) {
    return (static_cast<const std::uint8_t*>(bitmap)[i >> 3] >> (i & 7)) & 1U;
}

/// The first page of each column of the dataset's one data file must be FullZip over Fsst.
void check_page_is_fsst(const std::filesystem::path& dataset) {
    nano_lance::pb::Manifest manifest;
    std::uint64_t version = 0;
    std::string error;
    require(nano_lance::load_latest_manifest(dataset, manifest, version, error), "manifest: " + error);
    require(!manifest.fragments.empty() && !manifest.fragments[0].files.empty(), "a data file");
    const auto data_file = dataset / "data" / manifest.fragments[0].files[0].path;

    nano_lance::pb::FileDescriptor descriptor;
    nano_lance::LanceDataFileFooterLayout layout{};
    require(nano_lance::read_lance_data_file_footer_and_descriptor(data_file, descriptor, layout, error),
            "footer: " + error);
    std::vector<nano_lance::pb::ColumnMetadata> columns;
    require(nano_lance::read_lance_data_file_column_metadatas(data_file, layout, columns, error),
            "column metadata: " + error);
    require(!columns.empty() && !columns[0].pages.empty(), "a page");
    nano_lance::page_layout::PageLayout page;
    require(nano_lance::page_layout::decode_page_layout(columns[0].pages[0].encoding, page, error),
            "page layout: " + error);
    const auto text = nano_lance::page_layout::describe(page);
    require(text.find("FullZip") != std::string::npos && text.find("Fsst") != std::string::npos,
            "expected a FullZip page over Fsst, got: " + text);
}

void check_read_back(const std::filesystem::path& dataset, const std::vector<std::string>& values) {
    ArrowSchema schema{};
    ArrowArray* batches = nullptr;
    size_t count = 0;
    char err[512]{};
    const int rc = nano_lance_table_read_dataset(dataset.string().c_str(), &schema, &batches, &count, err, sizeof err);
    require(rc == NANO_LANCE_READER_OK, std::string("read back: ") + err);

    std::int64_t rows = 0;
    for (size_t b = 0; b < count; ++b) {
        rows += batches[b].length;
    }
    require(rows == kRows, "row count " + std::to_string(rows));

    // Each batch is a struct; its one child is the utf8 column. Rows arrive in order across batches.
    std::int64_t row = 0;
    for (size_t b = 0; b < count; ++b) {
        const ArrowArray& s = *batches[b].children[0];
        const auto* offsets = static_cast<const std::int32_t*>(s.buffers[1]) + s.offset;
        const auto* data = static_cast<const char*>(s.buffers[2]);
        for (std::int64_t i = 0; i < s.length; ++i, ++row) {
            const bool expect_null = row % kNullEvery == kNullEvery - 1;
            const bool valid = s.buffers[0] == nullptr || bit(s.buffers[0], s.offset + i);
            require(valid != expect_null, "validity of row " + std::to_string(row));
            if (valid) {
                const std::string got(data + offsets[i], static_cast<std::size_t>(offsets[i + 1] - offsets[i]));
                require(got == values[static_cast<std::size_t>(row)], "value of row " + std::to_string(row));
            }
        }
    }
    nano_lance_table_read_result_free(&schema, batches, count);
}

void test_round_trip(const std::filesystem::path& dir) {
    const auto values = sentences();
    Batch batch(values);
    nano_lance::Writer writer;
    const auto dataset = dir / "fsst.lance";
    require(writer.open(dataset), std::string("open: ") + writer.error());
    ArrowSchema schema_copy{};
    require(ArrowSchemaDeepCopy(&batch.schema, &schema_copy) == NANOARROW_OK, "schema copy");
    const bool written = writer.write_batch(&batch.array, &schema_copy);
    ArrowSchemaRelease(&schema_copy);
    require(written, std::string("write_batch: ") + writer.error());
    require(writer.commit(false), std::string("commit: ") + writer.error());
    require(writer.close(), "close");
    check_page_is_fsst(dataset);
    check_read_back(dataset, values);
}

}  // namespace

int main() {
    ScratchDir scratch;
    test_round_trip(scratch.path());
    std::cout << "fullzip_fsst: ok\n";
    return 0;
}
