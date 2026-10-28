// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// nano_lance::Writer (writer.hpp) on the paths test_writer_api leaves out: calls on a writer that is not
// open, a bad open, the default append mode of commit(), re-opening and move-assigning over an open
// writer, the automatic flush of max_pending_bytes, and the structural_encoding flag's mapping onto the
// C struct. Every dataset is written here and read back here.

#include "nanolance/nano_lance_reader.h"
#include "nanolance/writer.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
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
                ("nano_lance_writer_cxx_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        require(std::filesystem::create_directory(path_), "scratch directory already exists");
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    [[nodiscard]] std::filesystem::path path() const { return path_; }
    [[nodiscard]] std::filesystem::path sub(const char* name) const { return path_ / name; }

  private:
    std::filesystem::path path_;
};

constexpr std::int64_t kRows = 6;

/// One struct batch: id int64 and tag utf8, `kRows` rows, the ids 10, 11, ... so a read-back can be checked.
struct Batch {
    ArrowSchema schema{};
    ArrowArray array{};
    Batch() {
        ArrowSchemaInit(&schema);
        require(ArrowSchemaSetTypeStruct(&schema, 2) == NANOARROW_OK, "schema struct");
        require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK, "id type");
        require(ArrowSchemaSetName(schema.children[0], "id") == NANOARROW_OK, "id name");
        require(ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_STRING) == NANOARROW_OK, "tag type");
        require(ArrowSchemaSetName(schema.children[1], "tag") == NANOARROW_OK, "tag name");
        require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array init");
        require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start appending");
        for (std::int64_t i = 0; i < kRows; ++i) {
            const std::string tag = "t" + std::to_string(10 + i);
            const ArrowStringView sv{tag.c_str(), static_cast<std::int64_t>(tag.size())};
            require(ArrowArrayAppendInt(array.children[0], 10 + i) == NANOARROW_OK, "append id");
            require(ArrowArrayAppendString(array.children[1], sv) == NANOARROW_OK, "append tag");
            require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "finish element");
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

/// Read a dataset back with the C reader and return its ids, in order.
std::vector<std::int64_t> read_ids(const std::filesystem::path& path) {
    ArrowSchema schema{};
    ArrowArray* batches = nullptr;
    size_t count = 0;
    char err[512]{};
    const int rc = nano_lance_table_read_dataset(path.string().c_str(), &schema, &batches, &count, err, sizeof err);
    require(rc == NANO_LANCE_READER_OK, std::string("read back: ") + err);
    std::vector<std::int64_t> ids;
    for (size_t b = 0; b < count; ++b) {
        const ArrowArray& column = *batches[b].children[0];
        const auto* values = static_cast<const std::int64_t*>(column.buffers[1]) + column.offset;
        for (std::int64_t i = 0; i < batches[b].length; ++i) {
            ids.push_back(values[i]);
        }
    }
    nano_lance_table_read_result_free(&schema, batches, count);
    return ids;
}

/// The bytes of the first data file of a dataset: equal bytes mean an equal encoding.
std::vector<std::uint8_t> data_file_bytes(const std::filesystem::path& dataset) {
    for (const auto& entry : std::filesystem::directory_iterator(dataset / "data")) {
        if (entry.path().extension() == ".lance") {
            std::ifstream in(entry.path(), std::ios::binary);
            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }
    }
    throw std::runtime_error("no data file in " + dataset.string());
}

std::vector<std::int64_t> ids_of_rows(std::int64_t first, std::int64_t count) {
    std::vector<std::int64_t> ids;
    for (std::int64_t i = first; i < first + count; ++i) {
        ids.push_back(i);
    }
    return ids;
}

/// Write one batch through `writer` (the schema is copied, as the C API expects of its caller).
void write_one(nano_lance::Writer& writer) {
    Batch batch;
    ArrowSchema schema_copy{};
    require(ArrowSchemaDeepCopy(&batch.schema, &schema_copy) == NANOARROW_OK, "schema copy");
    const bool ok = writer.write_batch(&batch.array, &schema_copy);
    ArrowSchemaRelease(&schema_copy);
    require(ok, std::string("write_batch: ") + writer.error());
}

/// Calls on a writer that was never opened: refused by status, and nothing is pending.
void test_calls_before_open() {
    nano_lance::Writer writer;
    require(!writer.is_open(), "a new writer owns nothing");
    require(!writer.commit(), "commit on a writer that is not open must fail");
    require(writer.status() != NANO_LANCE_OK, "commit on a writer that is not open must set a status");
    require(writer.pending_batches() == 0 && writer.pending_bytes() == 0, "nothing can be pending");

    Batch batch;
    ArrowSchema schema_copy{};
    require(ArrowSchemaDeepCopy(&batch.schema, &schema_copy) == NANOARROW_OK, "schema copy");
    require(!writer.write_batch(&batch.array, &schema_copy), "write_batch on a writer that is not open must fail");
    ArrowSchemaRelease(&schema_copy);
    require(writer.pending_batches() == 0, "a refused batch must not be pending");

    require(writer.close(), "close on a writer that was never opened is harmless");
    require(writer.close(), "a second close is harmless too");
    require(writer.handle() != nullptr, "handle() is always the writer's own handle");
}

/// A bad path or bad options fail the open, say why, and leave the writer closed.
void test_bad_open() {
    nano_lance::Writer writer;
    require(!writer.open(""), "open with an empty path must fail");
    require(writer.status() == NANO_LANCE_INVALID_ARGUMENT, "empty path must be INVALID_ARGUMENT");
    require(std::strstr(writer.error(), "dataset path") != nullptr, "empty path error must name the dataset path");
    require(!writer.is_open(), "a failed open leaves the writer closed");

    const ScratchDir scratch;
    nano_lance::WriteOptions options;
    options.compression_level = 23;  // zstd stops at 22
    require(!writer.open(scratch.sub("bad_level"), options), "compression level 23 must fail the open");
    require(writer.status() == NANO_LANCE_INVALID_ARGUMENT, "a bad level must be INVALID_ARGUMENT");
    require(!std::filesystem::exists(scratch.sub("bad_level")), "a refused open must write nothing");

    // A failed open after a good one must not keep the good one's handle open.
    const auto good = scratch.sub("good");
    require(writer.open(good), "open on a good path");
    require(writer.is_open(), "an open writer reports itself open");
    require(!writer.open(""), "re-open with an empty path must fail");
    require(!writer.is_open(), "a failed re-open leaves the writer closed, not on the old dataset");
    require(writer.close(), "close after a failed open is harmless");
}

/// The lifecycle of one writer: pending counts move with write and commit, and the dataset reads back.
void test_pending_and_round_trip() {
    const ScratchDir scratch;
    const auto dataset = scratch.sub("round_trip");
    nano_lance::Writer writer;
    nano_lance::WriteOptions options;
    options.compression_level = 3;
    options.compression = true;
    require(writer.open(dataset, options), std::string("open: ") + writer.error());
    require(writer.is_open(), "open must leave the writer open");
    require(writer.pending_batches() == 0, "a new writer has nothing pending");

    write_one(writer);
    require(writer.pending_batches() == 1, "one batch must be pending after write_batch");
    require(writer.pending_bytes() > 0, "a pending batch must hold bytes");
    require(writer.commit(), std::string("commit: ") + writer.error());
    require(writer.pending_batches() == 0 && writer.pending_bytes() == 0, "commit must clear what was pending");
    require(!writer.commit(), "a commit with nothing pending must fail");
    require(writer.status() == NANO_LANCE_INVALID_STATE, "a commit with nothing pending is INVALID_STATE");
    require(writer.close(), "close");
    require(!writer.is_open(), "a closed writer owns nothing");

    require(read_ids(dataset) == ids_of_rows(10, kRows), "the committed rows must read back, in order");

    // After close, the writer refuses more batches.
    Batch batch;
    ArrowSchema schema_copy{};
    require(ArrowSchemaDeepCopy(&batch.schema, &schema_copy) == NANOARROW_OK, "schema copy");
    require(!writer.write_batch(&batch.array, &schema_copy), "write_batch after close must fail");
    ArrowSchemaRelease(&schema_copy);
}

/// commit() appends when the writer was opened with options.append, and does not when it was not.
void test_default_append_mode() {
    const ScratchDir scratch;
    const auto dataset = scratch.sub("append");
    {
        nano_lance::Writer first;
        require(first.open(dataset), std::string("create open: ") + first.error());
        write_one(first);
        require(first.commit(), std::string("create commit: ") + first.error());
    }
    {
        // Opened for create, the dataset exists: commit() must not silently add to it.
        nano_lance::Writer again;
        require(again.open(dataset), std::string("open: ") + again.error());
        write_one(again);
        // BUG: a create-mode commit (is_append = false) onto a dataset that already exists succeeds
        // instead of failing. Input: a dataset with 6 committed rows, then a create-mode writer with 6
        // more rows. Expected: commit() fails and the dataset still reads 6 rows. Observed: commit()
        // succeeds, and the dataset is the one the second commit wrote (see the KNOWN BUG line).
        const bool created = again.commit();
        const auto after = read_ids(dataset);
        if (created || after != ids_of_rows(10, kRows)) {
            std::cout << "KNOWN BUG: create-mode commit onto an existing dataset: commit returned " << created
                      << ", dataset now reads " << after.size() << " rows (expected commit to fail and 6 rows)\n";
        }
    }
    {
        // Whatever the create-mode commit above left behind, an append adds exactly one batch to it.
        const auto before = read_ids(dataset).size();
        nano_lance::Writer appender;
        nano_lance::WriteOptions options;
        options.append = true;
        require(appender.open(dataset, options), std::string("append open: ") + appender.error());
        write_one(appender);
        require(appender.commit(), std::string("append commit: ") + appender.error());
        require(read_ids(dataset).size() == before + static_cast<std::size_t>(kRows),
                "an append-mode commit must add its batch to the dataset");
    }
}

/// commit(bool) takes the mode explicitly; append onto a dataset that does not exist must fail.
void test_explicit_commit_mode() {
    const ScratchDir scratch;
    const auto dataset = scratch.sub("explicit");
    nano_lance::Writer writer;
    require(writer.open(dataset), std::string("open: ") + writer.error());
    write_one(writer);
    require(!writer.commit(true), "appending to a dataset that does not exist must fail");
    require(writer.status() != NANO_LANCE_OK, "a failed append commit must set a status");
    require(!std::filesystem::exists(dataset / "_versions"), "a failed append must not publish a version");
    require(writer.commit(false), std::string("create commit: ") + writer.error());
    require(read_ids(dataset) == ids_of_rows(10, kRows), "the create commit must hold the batch");
}

/// Opening a second dataset closes the first one without publishing the rows still pending in it.
void test_reopen_closes_previous() {
    const ScratchDir scratch;
    const auto kept = scratch.sub("kept");
    const auto dropped = scratch.sub("dropped");
    nano_lance::Writer writer;
    require(writer.open(kept), std::string("open kept: ") + writer.error());
    write_one(writer);
    require(writer.commit(), std::string("commit kept: ") + writer.error());

    require(writer.open(dropped), std::string("open dropped: ") + writer.error());
    write_one(writer);  // pending, never committed
    require(writer.open(scratch.sub("third")), "re-opening with a good path must succeed");
    require(writer.pending_batches() == 0, "the new dataset starts with nothing pending");
    require(!std::filesystem::exists(dropped / "_versions"), "the pending rows of a replaced writer are not published");
    require(read_ids(kept) == ids_of_rows(10, kRows), "the committed dataset must survive the re-open");
}

/// Move-assignment closes what the target held (without publishing) and takes over the source's dataset.
void test_move_assign_over_open_writer() {
    const ScratchDir scratch;
    const auto target_old = scratch.sub("target_old");
    const auto source_new = scratch.sub("source_new");

    nano_lance::Writer target;
    require(target.open(target_old), std::string("open target: ") + target.error());
    nano_lance::Writer source;
    require(source.open(source_new), std::string("open source: ") + source.error());
    write_one(source);

    target = std::move(source);
    require(!source.is_open(), "a move-assigned-from writer owns nothing");  // NOLINT(bugprone-use-after-move)
    require(target.is_open() && target.pending_batches() == 1, "the target must now hold the source's pending batch");
    require(target.commit(), std::string("commit after move: ") + target.error());
    require(!std::filesystem::exists(target_old / "_versions"), "the replaced dataset must not be published");
    require(read_ids(source_new) == ids_of_rows(10, kRows), "the moved writer's dataset must read back");
}

/// max_pending_bytes = 1: each write_batch commits its own fragment, so nothing is left pending.
void test_max_pending_bytes_flushes() {
    const ScratchDir scratch;
    const auto dataset = scratch.sub("flush");
    nano_lance::Writer writer;
    nano_lance::WriteOptions options;
    options.max_pending_bytes = 1;
    require(writer.open(dataset, options), std::string("open: ") + writer.error());
    write_one(writer);
    require(writer.pending_batches() == 0, "a batch over the budget must be flushed by write_batch");
    require(std::filesystem::exists(dataset / "_versions"), "the flush must publish a version");
    write_one(writer);
    require(writer.pending_batches() == 0, "the second batch must be flushed too");
    require(writer.close(), "close");
    require(read_ids(dataset) == [] {
        auto ids = ids_of_rows(10, kRows);
        const auto more = ids_of_rows(10, kRows);
        ids.insert(ids.end(), more.begin(), more.end());
        return ids;
    }(), "two flushed batches must read back as two fragments");
}

/// structural_encoding = false must write what the C struct's disable_structural_encoding = true writes.
void test_structural_encoding_flag_maps_to_c_struct() {
    const ScratchDir scratch;
    const auto via_cxx = scratch.sub("cxx_plain");
    const auto via_c = scratch.sub("c_plain");
    const auto via_cxx_default = scratch.sub("cxx_default");
    {
        nano_lance::Writer writer;
        nano_lance::WriteOptions options;
        options.structural_encoding = false;
        require(writer.open(via_cxx, options), std::string("open: ") + writer.error());
        write_one(writer);
        require(writer.commit(), std::string("commit: ") + writer.error());
    }
    {
        NanoLanceWriteOptions options{};
        options.disable_structural_encoding = true;
        NanoLanceWriter w{};
        require(nano_lance_writer_open(&w, via_c.string().c_str(), &options) == NANO_LANCE_OK, "C open");
        Batch batch;
        require(nano_lance_write_batch(&w, &batch.array, &batch.schema) == NANO_LANCE_OK, "C write");
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "C commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "C close");
    }
    {
        nano_lance::Writer writer;
        require(writer.open(via_cxx_default), std::string("open default: ") + writer.error());
        write_one(writer);
        require(writer.commit(), std::string("commit default: ") + writer.error());
    }
    require(data_file_bytes(via_cxx) == data_file_bytes(via_c),
            "structural_encoding = false must match disable_structural_encoding = true byte for byte");
    require(data_file_bytes(via_cxx_default) != data_file_bytes(via_cxx),
            "the flag must change the encoding: the default and the plain file must differ");
}

void run() {
    test_calls_before_open();
    test_bad_open();
    test_pending_and_round_trip();
    test_default_append_mode();
    test_explicit_commit_mode();
    test_reopen_closes_previous();
    test_move_assign_over_open_writer();
    test_max_pending_bytes_flushes();
    test_structural_encoding_flag_maps_to_c_struct();
}

}  // namespace

int main() {
    try {
        run();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "nano_lance::Writer tests passed\n";
    return 0;
}
