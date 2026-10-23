// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The write session under the C writer and under nanolance's own operations: what is pending and what
// is committed, the status each misuse gets (an empty path, an out-of-range zstd level, a URI that is not
// a local path or an object store, a commit with nothing pending, a setting changed after a write), and
// the staged mode, which hands its fragments to a caller and writes no version of its own.

#include "dataset/write_session.hpp"
#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path scratch(const char* name) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_write_session_" + std::string(name));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

// A one-column struct batch of int64 "id" values, built with nanoarrow.
void make_batch(const std::vector<std::int64_t>& values, ArrowSchema& schema, ArrowArray& array) {
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, 1) == NANOARROW_OK, "struct schema");
    require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK, "int64 column");
    require(ArrowSchemaSetName(schema.children[0], "id") == NANOARROW_OK, "column name");
    schema.children[0]->flags = 0;

    ArrowError err{};
    require(ArrowArrayInitFromSchema(&array, &schema, &err) == NANOARROW_OK, std::string("array: ") + err.message);
    require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start appending");
    for (const auto v : values) {
        require(ArrowArrayAppendInt(array.children[0], v) == NANOARROW_OK, "append");
    }
    array.length = static_cast<std::int64_t>(values.size());
    require(ArrowArrayFinishBuildingDefault(&array, &err) == NANOARROW_OK, std::string("finish: ") + err.message);
}

// Write `values` as one batch through `session`, then release what it was built from.
nano_lance::WriteStatus write_values(nano_lance::WriteSession& session, const std::vector<std::int64_t>& values) {
    ArrowSchema schema{};
    ArrowArray array{};
    make_batch(values, schema, array);
    const auto status = session.write_batch(&array, &schema);
    // The session borrows the batch; release it here unless the session has taken it over.
    if (array.release != nullptr) {
        ArrowArrayRelease(&array);
    }
    if (schema.release != nullptr) {
        ArrowSchemaRelease(&schema);
    }
    return status;
}

// The "id" values a read of `path` returns, in order.
std::vector<std::int64_t> read_ids(const std::filesystem::path& path) {
    nano_lance::LanceScanRequest request;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    const bool read = nano_lance::lance_dataset_scan(path, request, schema, batches, error);
    require(read, "read back: " + error);
    std::vector<std::int64_t> out;
    std::int64_t column = -1;
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (std::string(schema.children[i]->name) == "id") {
            column = i;
        }
    }
    require(column >= 0, "the read has an id column");
    for (const auto& batch : batches) {
        const ArrowArray* col = batch.children[column];
        for (std::int64_t r = 0; r < col->length; ++r) {
            out.push_back(static_cast<const std::int64_t*>(col->buffers[1])[col->offset + r]);
        }
    }
    for (auto& batch : batches) {
        ArrowArrayRelease(&batch);
    }
    ArrowSchemaRelease(&schema);
    return out;
}

}  // namespace

void require(bool ok, const std::string& msg, const std::string& detail) {
    require(ok, msg + detail);
}

int main() {
    // Misuse at open: each refused with its status and a message.
    {
        nano_lance::WriteSession session;
        nano_lance::WriteOptions options;
        require(session.open("", options) == nano_lance::WriteStatus::InvalidArgument, "an empty path is refused");
        require(session.error() == "dataset path must not be empty", "the empty-path message: " + session.error());
    }
    {
        nano_lance::WriteSession session;
        nano_lance::WriteOptions options;
        options.compression_level = 23;
        require(session.open(scratch("level").string(), options) == nano_lance::WriteStatus::InvalidArgument,
                "a zstd level above 22 is refused");
        require(!session.error().empty(), "the level refusal says why");
    }
    {
        nano_lance::WriteSession session;
        nano_lance::WriteOptions options;
        options.compression_level = -1;
        require(session.open(scratch("negative").string(), options) == nano_lance::WriteStatus::InvalidArgument,
                "a negative zstd level is refused");
    }
    {
        nano_lance::WriteSession session;
        nano_lance::WriteOptions options;
        require(session.open("ftp://example.invalid/dataset", options) == nano_lance::WriteStatus::Unsupported,
                "a URI that is not a local path or an object store is unsupported");
        require(session.error().find("local paths and s3:// only") != std::string::npos,
                "the message says what is supported: " + session.error());
    }
    {
        nano_lance::WriteSession session;
        nano_lance::WriteOptions options;
        options.column_encodings = {{"id", "not-an-encoding"}};
        require(session.open(scratch("encoding").string(), options) == nano_lance::WriteStatus::InvalidArgument,
                "an unknown column encoding is refused");
    }

    // A create, pending counters before and after a commit, and the data read back.
    const auto dataset = scratch("create");
    {
        nano_lance::WriteSession session;
        require(session.open(dataset.string(), nano_lance::WriteOptions{}) == nano_lance::WriteStatus::Ok,
                "open a new dataset");
        require(session.error().empty(), "a successful open leaves no error");
        require(session.pending_batches() == 0 && session.pending_rows() == 0 && session.pending_bytes() == 0,
                "nothing is pending before the first write");

        require(session.commit(false) == nano_lance::WriteStatus::InvalidState,
                "a commit with nothing pending is an invalid state");
        require(session.error() == "no pending rows to commit", "the message: " + session.error());

        require(write_values(session, {1, 2, 3, 4}) == nano_lance::WriteStatus::Ok, "write the first batch");
        require(session.pending_batches() == 1 && session.pending_rows() == 4, "one batch of four rows pending");
        require(session.pending_bytes() > 0, "pending bytes are counted");
        require(write_values(session, {5, 6}) == nano_lance::WriteStatus::Ok, "write the second batch");
        require(session.pending_batches() == 2 && session.pending_rows() == 6, "two batches, six rows pending");

        require(session.set_compression(true) == nano_lance::WriteStatus::InvalidState,
                "a setting cannot change once rows are written");
        require(session.error() == "compression must be set before writing batches", "the message: " + session.error());

        require(session.commit(false) == nano_lance::WriteStatus::Ok, "commit the pending rows");
        require(session.error().empty(), "a successful commit leaves no error");
        require(session.pending_rows() == 0 && session.pending_batches() == 0, "nothing pending after the commit");
        require(std::filesystem::exists(dataset / "_versions"), "the commit wrote a version");
        require(session.commit(false) == nano_lance::WriteStatus::InvalidState, "a second empty commit is refused");
    }
    require(read_ids(dataset) == std::vector<std::int64_t>({1, 2, 3, 4, 5, 6}),
            "the committed rows read back in order");

    // Append: a second session adds a fragment to the dataset, as a new version.
    {
        nano_lance::WriteOptions options;
        options.append = true;
        nano_lance::WriteSession session;
        require(session.open(dataset.string(), options) == nano_lance::WriteStatus::Ok, "open to append");
        require(write_values(session, {7, 8}) == nano_lance::WriteStatus::Ok, "write the appended batch");
        require(session.commit(true) == nano_lance::WriteStatus::Ok, "commit the append");
    }
    require(read_ids(dataset) == std::vector<std::int64_t>({1, 2, 3, 4, 5, 6, 7, 8}),
            "the appended rows follow the earlier ones");

    // Appending to a path that holds no dataset is an I/O error that says so.
    {
        nano_lance::WriteOptions options;
        options.append = true;
        nano_lance::WriteSession session;
        require(session.open(scratch("missing").string(), options) == nano_lance::WriteStatus::IoError,
                "an append to nothing is an I/O error");
        require(session.error() == "init_append requires an existing dataset with _versions",
                "the message: " + session.error());
    }

    // Staged: the fragments come back to the caller, and no version is written.
    {
        const auto staged = scratch("staged");
        nano_lance::WriteOptions options;
        options.stage_fragments = true;
        nano_lance::WriteSession session;
        require(session.open(staged.string(), options) == nano_lance::WriteStatus::Ok, "open a staged writer");
        require(write_values(session, {100, 101}) == nano_lance::WriteStatus::Ok, "write into the staged writer");

        std::vector<nano_lance::NewFragment> fragments;
        nano_lance::LanceSchemaMapping mapping;
        std::string error;
        require(session.take_staged(fragments, mapping, error), "take the staged fragments: ", error);
        require(fragments.size() == 1, "one fragment was staged");
        require(fragments[0].rows == 2, "the staged fragment holds the two rows");
        // The take commits the data files; the version directory may be made, but holds no manifest.
        require(!std::filesystem::exists(staged / "_versions") || std::filesystem::is_empty(staged / "_versions"),
                "a staged writer writes no version of its own");

        nano_lance::WriteSession plain;
        require(plain.open(scratch("unstaged").string(), nano_lance::WriteOptions{}) == nano_lance::WriteStatus::Ok,
                "open an ordinary writer");
        std::vector<nano_lance::NewFragment> none;
        require(!plain.take_staged(none, mapping, error), "only a staged writer has fragments to take");
        require(error == "not a staged writer", "the error says so: ", error);
    }

    std::error_code ec;
    std::filesystem::remove_all(dataset, ec);
    std::filesystem::remove_all(scratch("level"), ec);
    std::filesystem::remove_all(scratch("negative"), ec);
    std::filesystem::remove_all(scratch("encoding"), ec);
    std::filesystem::remove_all(scratch("missing"), ec);
    std::filesystem::remove_all(scratch("staged"), ec);
    std::filesystem::remove_all(scratch("unstaged"), ec);
    std::cout << "write session: pending counters, commit, append, staged take, refused misuse OK\n";
    return 0;
}
