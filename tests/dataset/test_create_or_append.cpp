// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// NANO_LANCE_COMMIT_CREATE_OR_APPEND, and many writers starting one dataset at once:
//  - sequential: a writer opened on nothing creates; a second one, also opened on nothing, appends;
//  - a schema mismatch is refused, and the dataset is left as it was;
//  - a race: N forked writers (default 16, NANOLANCE_TEST_RACE_WRITERS) each stage their own rows and finish with
//    CREATE_OR_APPEND. None fails, there are exactly N versions, and every row is there exactly once. The parent
//    draws transaction ids before it forks, so the children start from a copied generator state -- the case in
//    which writers named their transactions alike and a loser took the winner's manifest for its own.
// NANOLANCE_TEST_DATASET_URI (e.g. s3://bucket/prefix) runs the race on an object store instead of a local path.

#include "support.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <set>

using namespace dataset_test;

namespace {

/// Open a staged writer at `path`, write `ids`, and finish with `mode`. The status, and the error in `error`.
int stage_and_finish(const std::string& path, const std::vector<std::int64_t>& ids, int mode, std::string& error,
                     const char* column = "id") {
    NanoLanceWriteOptions options{};
    nano_lance_write_options_init(&options);
    options.stage_fragments = true;
    NanoLanceWriter w{};
    int rc = nano_lance_writer_open(&w, path.c_str(), &options);
    if (rc != NANO_LANCE_OK) {
        error = nano_lance_writer_last_error(&w);
        return rc;
    }
    ArrowSchema schema{};
    ArrowArray array{};
    make_ids(ids, schema, array);
    ArrowSchemaSetName(schema.children[0], column);
    rc = nano_lance_write_batch(&w, &array, &schema);
    if (array.release != nullptr) {
        ArrowArrayRelease(&array);
    }
    ArrowSchemaRelease(&schema);
    if (rc == NANO_LANCE_OK) {
        std::uint64_t version = 0;
        rc = nano_lance_writer_finish(&w, mode, &version);
    }
    error = rc == NANO_LANCE_OK ? "" : nano_lance_writer_last_error(&w);
    nano_lance_writer_close(&w);
    return rc;
}

std::vector<std::int64_t> range(std::int64_t from, std::int64_t count) {
    std::vector<std::int64_t> out;
    for (std::int64_t i = 0; i < count; ++i) {
        out.push_back(from + i);
    }
    return out;
}

std::size_t versions(const std::filesystem::path& path) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(path / "_versions", ec)) {
        n += entry.path().extension() == ".manifest" ? 1U : 0U;
    }
    return n;
}

}  // namespace

int main() {
    const int mode = NANO_LANCE_COMMIT_CREATE_OR_APPEND;
    std::string error;

    // Sequential: the first creates, the second (opened on nothing too) appends.
    const auto seq = scratch("create_or_append_seq");
    require(stage_and_finish(seq.string(), range(0, 100), mode, error) == NANO_LANCE_OK, "first writer: " + error);
    require(stage_and_finish(seq.string(), range(100, 50), mode, error) == NANO_LANCE_OK, "second writer: " + error);
    require(versions(seq) == 2U, "versions 1 and 2");
    auto rows = read_column(seq, "id");
    std::sort(rows.begin(), rows.end());
    require(rows == range(0, 150), "150 rows, each once");

    // Schema mismatch: a column of another name is refused, and nothing is published.
    require(stage_and_finish(seq.string(), range(1000, 10), mode, error, "other") != NANO_LANCE_OK,
            "a writer with another schema is refused");
    require(error.find("schema mismatch:") != std::string::npos && error.find("'other'") != std::string::npos,
            "the error names the field: " + error);
    require(versions(seq) == 2U, "no version was added");
    require(read_column(seq, "id").size() == 150U, "the rows are as they were");

    // Plain create still refuses an existing dataset.
    require(stage_and_finish(seq.string(), range(2000, 10), NANO_LANCE_COMMIT_CREATE, error) != NANO_LANCE_OK,
            "create refuses an existing dataset");

    // The race.
    int writers = 16;
    if (const char* env = std::getenv("NANOLANCE_TEST_RACE_WRITERS"); env != nullptr && std::atoi(env) > 0) {
        writers = std::atoi(env);
    }
    const bool verbose = std::getenv("NANOLANCE_TEST_RACE_VERBOSE") != nullptr;
    const char* uri = std::getenv("NANOLANCE_TEST_DATASET_URI");
    const auto local = scratch("create_or_append_race");
    const std::string target = uri != nullptr && *uri != '\0' ? uri : local.string();
    constexpr std::int64_t kRows = 1000;
    // Draw ids in the parent first, so each child starts from the same copied generator state.
    {
        const auto warm = scratch("create_or_append_warm");
        require(stage_and_finish(warm.string(), range(0, 1), mode, error) == NANO_LANCE_OK, "warm-up: " + error);
        std::error_code ec;
        std::filesystem::remove_all(warm, ec);
    }
    std::vector<pid_t> children;
    for (int k = 0; k < writers; ++k) {
        const pid_t pid = fork();
        require(pid >= 0, "fork");
        if (pid == 0) {
            std::string child_error;
            const int rc = stage_and_finish(target, range(static_cast<std::int64_t>(k) * kRows, kRows), mode, child_error);
            if (verbose || rc != NANO_LANCE_OK) {
                std::cerr << "writer " << k << ": " << (rc == NANO_LANCE_OK ? "ok" : child_error) << '\n';
            }
            std::_Exit(rc == NANO_LANCE_OK ? 0 : 1);
        }
        children.push_back(pid);
    }
    int failed = 0;
    for (const auto pid : children) {
        int status = 0;
        waitpid(pid, &status, 0);
        failed += WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
    }
    require(failed == 0, std::to_string(failed) + " of " + std::to_string(writers) + " writers failed");
    if (uri == nullptr || *uri == '\0') {
        require(versions(local) == static_cast<std::size_t>(writers), "one version per writer");
    }
    rows = read_column(target, "id");
    require(rows.size() == static_cast<std::size_t>(writers) * kRows,
            "every row of every writer: " + std::to_string(rows.size()));
    const std::set<std::int64_t> unique(rows.begin(), rows.end());
    require(unique.size() == rows.size(), "no row twice");

    std::error_code ec;
    std::filesystem::remove_all(seq, ec);
    std::filesystem::remove_all(local, ec);
    std::cout << "create-or-append: sequential, schema mismatch, " << writers << " forked writers OK\n";
    return 0;
}
