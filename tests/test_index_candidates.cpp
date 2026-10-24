// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// index_candidates (nanolance/index_search.hpp): which rows of which fragments may pass a filter, answered
// from a BTREE on "id" and a BITMAP on "grp" of a two-fragment dataset (written with the C writer API, as
// the writer tests do), and nothing for a column with no index. An answer covers the rows that pass; it is
// exact (rows are exactly those that pass) for =, <, IN, OR and AND over indexed columns, and a superset
// when one side of an AND is not indexed. A filter no index answers gives no rows and no line.

#include "nanolance/expr.hpp"
#include "nanolance/index_search.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/scalar_index.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

// A row, as an index answers it: (fragment id, physical offset in that fragment).
using Row = std::pair<std::uint64_t, std::uint32_t>;

// Dataset rows: id 0..19 (fragment 0 holds 0..9 at offsets 0..9, fragment 1 holds 10..19 at offsets 0..9),
// grp = id % 4 (BITMAP), other = id % 3 (no index).
std::uint64_t fragment_of(std::int64_t id) { return id < 10 ? 0U : 1U; }
std::uint32_t offset_of(std::int64_t id) { return static_cast<std::uint32_t>(id < 10 ? id : id - 10); }

Row row_of(std::int64_t id) { return {fragment_of(id), offset_of(id)}; }

std::vector<Row> rows_of(const std::vector<std::int64_t>& ids) {
    std::vector<Row> out;
    for (const auto id : ids) {
        out.push_back(row_of(id));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The rows an answer holds, over all fragments, ascending.
std::vector<Row> rows_in(const nano_lance::IndexCandidates& answer) {
    std::vector<Row> out;
    for (const auto& [fragment, offsets] : answer.rows) {
        for (const auto offset : offsets) {
            out.emplace_back(fragment, offset);
        }
    }
    return out;
}

// Each id 0..19 with its grp and other values, in one batch of `count` rows from `first`.
void write_rows(NanoLanceWriter* writer, std::int64_t first, std::int64_t count) {
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, 3) == NANOARROW_OK, "struct schema");
    const char* names[3] = {"id", "grp", "other"};
    for (std::int64_t c = 0; c < 3; ++c) {
        require(ArrowSchemaSetType(schema.children[c], NANOARROW_TYPE_INT64) == NANOARROW_OK, "int64 column");
        require(ArrowSchemaSetName(schema.children[c], names[c]) == NANOARROW_OK, "column name");
    }
    ArrowArray array{};
    ArrowError err{};
    require(ArrowArrayInitFromSchema(&array, &schema, &err) == NANOARROW_OK, std::string("array: ") + err.message);
    require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start appending");
    for (std::int64_t i = first; i < first + count; ++i) {
        require(ArrowArrayAppendInt(array.children[0], i) == NANOARROW_OK, "append id");
        require(ArrowArrayAppendInt(array.children[1], i % 4) == NANOARROW_OK, "append grp");
        require(ArrowArrayAppendInt(array.children[2], i % 3) == NANOARROW_OK, "append other");
    }
    array.length = count;
    require(ArrowArrayFinishBuildingDefault(&array, &err) == NANOARROW_OK, std::string("finish: ") + err.message);
    const auto status = nano_lance_write_batch(writer, &array, &schema);
    require(status == NANO_LANCE_OK, std::string("write_batch: ") + nano_lance_writer_last_error(writer));
    if (array.release != nullptr) {
        ArrowArrayRelease(&array);
    }
    if (schema.release != nullptr) {
        ArrowSchemaRelease(&schema);
    }
}

std::filesystem::path scratch() {
    auto p = std::filesystem::temp_directory_path() / "nano_lance_index_candidates_test";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

void write_dataset(const std::filesystem::path& path) {
    NanoLanceWriter w{};
    require(nano_lance_writer_init(&w, path.string().c_str(), 0) == NANO_LANCE_OK, "init");
    write_rows(&w, 0, 10);
    require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit the first fragment");
    require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");

    NanoLanceWriter a{};
    require(nano_lance_writer_init_append(&a, path.string().c_str(), 0) == NANO_LANCE_OK, "init_append");
    write_rows(&a, 10, 10);
    require(nano_lance_writer_commit(&a, true) == NANO_LANCE_OK, "commit the second fragment");
    require(nano_lance_writer_close(&a) == NANO_LANCE_OK, "close the append");
}

// The answer to `sql` over the latest version of `path`.
bool answer_for(const std::filesystem::path& path, const std::string& sql, nano_lance::IndexCandidates& answer,
                std::string& error) {
    nano_lance::pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!nano_lance::load_latest_manifest(path, manifest, version, error)) {
        return false;
    }
    nano_lance::LanceScanRequest request;
    ArrowSchema schema{};
    if (!nano_lance::lance_dataset_schema(path, request, schema, error)) {
        return false;
    }
    nano_lance::expr::Expression filter;
    bool ok = nano_lance::expr::Expression::parse(sql, filter, error) && filter.bind(schema, error);
    ArrowSchemaRelease(&schema);
    if (!ok) {
        return false;
    }
    return nano_lance::index_candidates(path, manifest, filter, answer, error);
}

}  // namespace

int main() {
    const auto path = scratch();
    write_dataset(path);

    std::string error;
    {
        std::uint64_t version = 0;
        require(nano_lance::dataset_create_scalar_index(path, "id", nano_lance::ScalarIndexType::BTree,
                                                        nano_lance::ScalarIndexOptions{}, version, error),
                "a BTREE on id: " + error);
        require(nano_lance::dataset_create_scalar_index(path, "grp", nano_lance::ScalarIndexType::Bitmap,
                                                        nano_lance::ScalarIndexOptions{}, version, error),
                "a BITMAP on grp: " + error);
    }

    // Filters the indices answer exactly: the rows are the rows that pass, and each index is named.
    struct Exact {
        const char* sql;
        std::vector<std::int64_t> passing;
        std::size_t lines;
    };
    const std::vector<Exact> exact = {
        {"id = 5", {5}, 1},
        {"id < 4", {0, 1, 2, 3}, 1},
        {"id IN (3, 12, 19)", {3, 12, 19}, 1},
        {"grp = 1 AND id >= 10", {13, 17}, 2},
        {"id = 2 OR grp = 3", {2, 3, 7, 11, 15, 19}, 2},
        {"grp IN (0, 2)", {0, 2, 4, 6, 8, 10, 12, 14, 16, 18}, 1},
        {"id = 100", {}, 1},
    };
    for (const auto& c : exact) {
        nano_lance::IndexCandidates answer;
        require(answer_for(path, c.sql, answer, error), std::string(c.sql) + ": " + error);
        require(answer.exact, std::string(c.sql) + " is answered exactly");
        require(rows_in(answer) == rows_of(c.passing), std::string(c.sql) + " gives the rows that pass");
        require(answer.used.size() == c.lines, std::string(c.sql) + " names " + std::to_string(c.lines) + " index(es)");
        for (const auto& line : answer.used) {
            require(line.rfind("ScalarIndexQuery: query=[", 0) == 0, "the line starts as explain names it: " + line);
        }
    }
    {
        nano_lance::IndexCandidates answer;
        require(answer_for(path, "id = 5", answer, error), "id = 5: " + error);
        require(answer.used.size() == 1 && answer.used[0].find("@id_idx(BTree)") != std::string::npos,
                "the BTree answers id = 5 by name: " + (answer.used.empty() ? std::string() : answer.used[0]));
        require(answer.rows.size() == 2, "both fragments are covered by the index");
    }

    // An AND with an unindexed side: the indexed side's rows, a superset of the rows that pass (here none:
    // ids 0..5 with other = 1 are 1 and 4, and both are in the superset), and not exact.
    {
        nano_lance::IndexCandidates answer;
        require(answer_for(path, "id < 6 AND other = 1", answer, error), "AND with a scan side: " + error);
        require(!answer.exact, "an AND with a side nothing answers is not exact");
        require(answer.used.size() == 1 && answer.used[0].find("@id_idx(BTree)") != std::string::npos,
                "only the indexed side is named");
        const auto got = rows_in(answer);
        require(got == rows_of({0, 1, 2, 3, 4, 5}), "the indexed side's rows, ids 0 to 5");
        for (const auto id : {1, 4}) {
            require(std::find(got.begin(), got.end(), row_of(id)) != got.end(),
                    "a row that passes is in the candidates: id " + std::to_string(id));
        }
    }
    {
        nano_lance::IndexCandidates answer;
        require(answer_for(path, "id = 2 AND grp = 2", answer, error), "AND of two indices: " + error);
        require(answer.exact && answer.used.size() == 2, "two indices, both exact");
        require(rows_in(answer) == rows_of({2}), "id 2 with grp 2 is id 2");
    }

    // A column with no index, and a filter with a part no index answers: no rows, no line, not exact.
    for (const char* sql : {"other = 1", "id = 2 OR other = 1", "NOT id = 5"}) {
        nano_lance::IndexCandidates answer;
        require(answer_for(path, sql, answer, error), std::string(sql) + ": " + error);
        require(answer.used.empty(), std::string(sql) + " names no index");
        require(rows_in(answer).empty(), std::string(sql) + " gives no rows");
        require(!answer.exact, std::string(sql) + " is not exact");
    }

    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::cout << "index_candidates: ok (BTREE and BITMAP answers for =, <, IN, AND, OR; unindexed filters)\n";
    return 0;
}
