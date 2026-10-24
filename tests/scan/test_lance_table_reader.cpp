// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The scan of a dataset: which rows a read returns, for a filter, a row range, a take, and the row
// address columns. A small two-fragment dataset is written with the C writer API (as the writer tests do)
// and read back through lance_dataset_scan and lance_dataset_take. A filter's rows must be the same
// whether or not an index narrows the read, and an index may only narrow which rows are read, never change
// which rows pass.

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/scalar_index.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace pb = nano_lance::pb;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path scratch() {
    auto p = std::filesystem::temp_directory_path() / "nano_lance_table_reader_test";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

// One int64 column named "id" holding `values`.
void write_ids(NanoLanceWriter* writer, const std::vector<std::int64_t>& values) {
    ArrowSchema child{};
    child.format = "l";
    child.name = "id";
    ArrowSchema* child_ptrs[1] = {&child};
    ArrowSchema schema{};
    schema.format = "+s";
    schema.n_children = 1;
    schema.children = child_ptrs;

    const void* buffers[2] = {nullptr, values.data()};
    ArrowArray id_array{};
    id_array.length = static_cast<std::int64_t>(values.size());
    id_array.n_buffers = 2;
    id_array.buffers = buffers;

    ArrowArray* array_children[1] = {&id_array};
    ArrowArray batch{};
    batch.length = static_cast<std::int64_t>(values.size());
    batch.n_children = 1;
    batch.children = array_children;

    require(nano_lance_write_batch(writer, &batch, &schema) == NANO_LANCE_OK,
            std::string("write_batch: ") + nano_lance_writer_last_error(writer));
}

// Two fragments: ids 10..14 (fragment 0) and 15..19 (fragment 1), written as two commits.
void write_dataset(const std::filesystem::path& path) {
    NanoLanceWriter w{};
    require(nano_lance_writer_init(&w, path.string().c_str(), 0) == NANO_LANCE_OK, "init");
    write_ids(&w, {10, 11, 12, 13, 14});
    require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit the first fragment");
    require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");

    NanoLanceWriter a{};
    require(nano_lance_writer_init_append(&a, path.string().c_str(), 0) == NANO_LANCE_OK, "init_append");
    write_ids(&a, {15, 16, 17, 18, 19});
    require(nano_lance_writer_commit(&a, true) == NANO_LANCE_OK, "commit the second fragment");
    require(nano_lance_writer_close(&a) == NANO_LANCE_OK, "close the append");
}

// The values of column `name` over every batch (a read yields one batch per fragment), in order.
std::vector<std::uint64_t> column(const ArrowSchema& schema, const std::vector<ArrowArray>& batches,
                                  const std::string& name) {
    std::int64_t index = -1;
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (name == schema.children[i]->name) {
            index = i;
        }
    }
    require(index >= 0, "the result has a column named " + name);
    std::vector<std::uint64_t> out;
    for (const auto& batch : batches) {
        const ArrowArray* col = batch.children[index];
        for (std::int64_t r = 0; r < col->length; ++r) {
            out.push_back(static_cast<const std::uint64_t*>(col->buffers[1])[col->offset + r]);
        }
    }
    return out;
}

std::vector<std::string> column_names(const ArrowSchema& schema) {
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        out.emplace_back(schema.children[i]->name);
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

// Which fragments the index answers for in these tests: fragment 0, with its rows at offsets 0, 1 and 3.
std::map<std::uint64_t, std::vector<std::uint32_t>> g_candidate_rows;
bool g_candidates_called = false;

bool answer_from_index(const std::filesystem::path& /*dataset_path*/, const pb::Manifest& /*manifest*/,
                       const nano_lance::expr::Expression& /*filter*/, nano_lance::IndexCandidates& out,
                       std::string& /*error*/) {
    g_candidates_called = true;
    out.rows = g_candidate_rows;
    out.used = {"ScalarIndexQuery: query=[test]@test(BTREE)"};
    out.exact = false;
    return true;
}

bool fail_index(const std::filesystem::path& /*dataset_path*/, const pb::Manifest& /*manifest*/,
                const nano_lance::expr::Expression& /*filter*/, nano_lance::IndexCandidates& /*out*/,
                std::string& error) {
    error = "the index cannot be read";
    return false;
}

}  // namespace

void require(bool ok, const std::string& msg, const std::string& detail) {
    require(ok, msg + detail);
}

int main() {
    const auto dataset = scratch();
    write_dataset(dataset);
    const std::vector<std::uint64_t> all = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};

    // A full read, one batch per fragment, every row in order.
    {
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        require(nano_lance::lance_dataset_scan(dataset, request, schema, batches, error), "full scan: ", error);
        require(batches.size() == 2, "one batch per fragment");
        require(column_names(schema) == std::vector<std::string>({"id"}), "a plain read has the data columns only");
        require(column(schema, batches, "id") == all, "every row, in fragment order");
        release(schema, batches);
    }

    // A filter: only the rows for which it is true.
    {
        const std::string filter = "id >= 13 AND id < 17";
        nano_lance::LanceScanRequest request;
        request.filter = &filter;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        require(nano_lance::lance_dataset_scan(dataset, request, schema, batches, error), "filtered scan: ", error);
        require(column(schema, batches, "id") == std::vector<std::uint64_t>({13, 14, 15, 16}),
                "the filter keeps 13 to 16, across the fragment boundary");
        release(schema, batches);
    }

    // A scalar index on id, so the manifest has an index for the scan to consult (it asks only then).
    {
        std::uint64_t version = 0;
        std::string error;
        const bool created = nano_lance::dataset_create_scalar_index(
            dataset, "id", nano_lance::ScalarIndexType::BTree, nano_lance::ScalarIndexOptions{}, version, error);
        require(created, "create the index: " + error);
    }

    // The index's answer is a superset of the rows that pass (what it promises). With it, the rows returned
    // are the same as without it: fragment 0's passing rows (offsets 1 to 4, ids 11 to 14) and fragment 1 in
    // full. A fragment the index answers with no candidates is not opened at all.
    {
        g_candidate_rows = {{0, {1, 2, 3, 4}}};
        g_candidates_called = false;
        const std::string filter = "id > 10";
        nano_lance::LanceScanRequest request;
        request.filter = &filter;
        request.index_candidates = &answer_from_index;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        const bool indexed = nano_lance::lance_dataset_scan(dataset, request, schema, batches, error);
        require(indexed, "indexed scan: " + error);
        require(g_candidates_called, "the index was asked");
        require(column(schema, batches, "id") == std::vector<std::uint64_t>({11, 12, 13, 14, 15, 16, 17, 18, 19}),
                "the indexed read returns the rows the filter passes");
        release(schema, batches);

        g_candidate_rows = {{0, {}}};  // the index says no row of fragment 0 can pass
        const std::string late = "id >= 15";
        request.filter = &late;
        const bool answered = nano_lance::lance_dataset_scan(dataset, request, schema, batches, error);
        require(answered, "empty answer: " + error);
        require(column(schema, batches, "id") == std::vector<std::uint64_t>({15, 16, 17, 18, 19}),
                "a fragment answered with nothing contributes nothing, and the rest is read");
        release(schema, batches);
    }

    // An index that cannot be read is not used: the scan reads everything the filter needs and returns the
    // same rows. (The scan code says so: "an index that cannot be read is not used: the read scans".)
    {
        const std::string filter = "id > 10";
        nano_lance::LanceScanRequest request;
        request.filter = &filter;
        request.index_candidates = &fail_index;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        const bool scanned = nano_lance::lance_dataset_scan(dataset, request, schema, batches, error);
        require(scanned, "an unreadable index does not fail the scan: " + error);
        require(column(schema, batches, "id") == std::vector<std::uint64_t>({11, 12, 13, 14, 15, 16, 17, 18, 19}),
                "the scan reads the rows itself");
        release(schema, batches);
    }

    // A row range counts logical rows and clamps at the end; an offset past the end is an error.
    {
        nano_lance::LanceScanRequest request;
        request.range.offset = 3;
        request.range.length = 4;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        require(nano_lance::lance_dataset_scan(dataset, request, schema, batches, error), "range scan: ", error);
        require(column(schema, batches, "id") == std::vector<std::uint64_t>({13, 14, 15, 16}),
                "rows 3 to 6 span the two fragments");
        release(schema, batches);

        request.range.offset = 8;
        request.range.length = nano_lance::LanceRowRange::kAllRows;
        require(nano_lance::lance_dataset_scan(dataset, request, schema, batches, error), "tail scan: ", error);
        require(column(schema, batches, "id") == std::vector<std::uint64_t>({18, 19}),
                "a range past the end is clamped to it");
        release(schema, batches);

        request.range.offset = 11;  // past the last of the 10 rows
        request.range.length = 1;
        require(!nano_lance::lance_dataset_scan(dataset, request, schema, batches, error),
                "an offset past the last row is an error");
        require(!error.empty(), "the error says something");
    }

    // A take: the rows at the indices, ascending and each once, whatever order they were asked in.
    {
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        require(nano_lance::lance_dataset_take(dataset, request, {9, 0, 7, 0}, schema, batches, error),
                "take: ", error);
        require(column(schema, batches, "id") == std::vector<std::uint64_t>({10, 17, 19}),
                "the taken rows come back ascending, once each");
        release(schema, batches);

        require(!nano_lance::lance_dataset_take(dataset, request, {10}, schema, batches, error),
                "an index past the end is an error");
        require(!error.empty(), "the take error says something");
    }

    // Row address columns: the fragment id in the high 32 bits, the offset in the low, for each row.
    {
        nano_lance::LanceScanRequest request;
        request.with_row_id = true;
        request.with_row_address = true;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        require(nano_lance::lance_dataset_scan(dataset, request, schema, batches, error), "address scan: ", error);
        require(column_names(schema) == std::vector<std::string>({"id", "_rowid", "_rowaddr"}),
                "the address columns come after the data columns");

        const std::vector<std::uint64_t> addresses = {0, 1, 2, 3, 4, 1ULL << 32U, (1ULL << 32U) + 1,
                                                      (1ULL << 32U) + 2, (1ULL << 32U) + 3, (1ULL << 32U) + 4};
        require(column(schema, batches, "_rowaddr") == addresses, "row addresses: fragment << 32 | offset");
        require(column(schema, batches, "_rowid") == addresses, "without stable row ids, the row id is the address");
        require(column(schema, batches, "id") == all, "the data is unchanged by the address columns");
        release(schema, batches);
    }

    // A missing dataset is an error that is reported.
    {
        nano_lance::LanceScanRequest request;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        require(!nano_lance::lance_dataset_scan(dataset / "does-not-exist", request, schema, batches, error),
                "a dataset that is not there is an error");
        require(!error.empty(), "the error says something");
    }

    std::error_code ec;
    std::filesystem::remove_all(dataset, ec);
    std::cout << "lance table reader: filter, index candidates, range, take, row addresses OK\n";
    return 0;
}
