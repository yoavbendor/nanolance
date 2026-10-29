// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// lance_dataset_scan and LanceTableStream::open_request, the dataset layer's entry points for a whole scan
// request: the version, fragments, projection, row addresses, range and filter a request names, and the
// failures a request can meet. The dataset is written here (four fragments, two versions) and every value
// checked is one this test put there. test_lance_table_reader covers the scan layer's own reads.

#include "nanolance/lance_table_reader.hpp"
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

/// A fresh directory, removed when the test ends however it ends.
class ScratchDir {
  public:
    ScratchDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("nano_lance_dataset_scan_" + std::to_string(rd()) + "_" + std::to_string(rd()));
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
constexpr std::int64_t kRows = 4 * kRowsPerFragment;

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

/// Four fragments of kRowsPerFragment rows, one commit each: version 1 holds fragment 0 only.
/// Returns after the last commit, so the dataset has four fragments and four versions.
void write_dataset(const std::filesystem::path& path, const ArrowSchema& schema) {
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, path.string().c_str(), 0) == NANO_LANCE_OK, "writer init failed");
    for (int fragment = 0; fragment < 4; ++fragment) {
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

/// What a scan returned: releases its schema and batches when it goes out of scope.
struct Scan {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    ~Scan() {
        for (auto& b : batches) {
            if (b.release != nullptr) {
                b.release(&b);
            }
        }
        if (schema.release != nullptr) {
            ArrowSchemaRelease(&schema);
        }
    }
};

/// The `id` column of every batch, in order. The id is the first column of a scan that keeps columns.
std::vector<std::int64_t> ids_of(const Scan& scan) {
    std::vector<std::int64_t> ids;
    for (const auto& batch : scan.batches) {
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

/// The ids a stream yields, drained batch by batch; the caller's batches are released as they are read.
std::vector<std::int64_t> drain(nano_lance::LanceTableStream& stream) {
    std::vector<std::int64_t> ids;
    for (;;) {
        ArrowArray batch{};
        std::string error;
        require(stream.next(batch, error), "stream next failed: " + error);
        if (batch.release == nullptr) {
            break;
        }
        const ArrowArray& column = *batch.children[0];
        const auto* values = static_cast<const std::int64_t*>(column.buffers[1]) + column.offset;
        for (std::int64_t i = 0; i < batch.length; ++i) {
            ids.push_back(values[i]);
        }
        ArrowArrayRelease(&batch);
    }
    return ids;
}

bool scan_ok(const std::filesystem::path& path, const nano_lance::LanceScanRequest& request, Scan& out,
             std::string& error) {
    return nano_lance::lance_dataset_scan(path, request, out.schema, out.batches, error);
}

/// lance_dataset_scan_core, which consults no index: the same arguments and results as lance_dataset_scan here.
bool core_scan(const std::filesystem::path& path, const nano_lance::LanceScanRequest& request, Scan& out,
                          std::string& error) {
    return nano_lance::lance_dataset_scan_core(path, request, out.schema, out.batches, error);
}

void test_full_and_range_scans(const std::filesystem::path& path) {
    nano_lance::LanceScanRequest request;
    Scan all;
    std::string error;
    require(scan_ok(path, request, all, error), "full scan: " + error);
    require(all.schema.n_children == 2, "a default request keeps both columns");
    require(ids_of(all) == id_range(0, kRows), "a full scan must return every row, in order");

    nano_lance::LanceScanRequest range_request;
    range_request.range.offset = 700;
    range_request.range.length = 300;
    Scan window;
    require(scan_ok(path, range_request, window, error), "range scan: " + error);
    require(ids_of(window) == id_range(700, 300), "a row range must return exactly its rows");

    // trusted_input is a flag on the same read: for a dataset this test wrote, it changes nothing.
    Scan trusted;
    require(nano_lance::lance_dataset_scan(path, request, trusted.schema, trusted.batches, error, true),
            "trusted scan: " + error);
    require(ids_of(trusted) == id_range(0, kRows), "trusted_input must not change the rows");

    // The indexed and the core entry points agree when there is no index to consult.
    Scan core;
    require(core_scan(path, request, core, error), "core scan: " + error);
    require(ids_of(core) == id_range(0, kRows), "the core scan must return the same rows");
}

void test_version_fragments_and_projection(const std::filesystem::path& path) {
    std::string error;

    // Version 1 is the first commit: one fragment.
    nano_lance::LanceScanRequest old_version;
    old_version.has_version = true;
    old_version.version = 1;
    Scan v1;
    require(scan_ok(path, old_version, v1, error), "version 1 scan: " + error);
    require(ids_of(v1) == id_range(0, kRowsPerFragment), "version 1 must hold only the first fragment");

    // A fragment list reads just those fragments, in the order given.
    const std::vector<std::uint64_t> fragments = {2, 0};
    nano_lance::LanceScanRequest picked;
    picked.fragment_ids = &fragments;
    Scan two;
    require(scan_ok(path, picked, two, error), "fragment scan: " + error);
    std::vector<std::int64_t> expected = id_range(1000, kRowsPerFragment);
    const auto first = id_range(0, kRowsPerFragment);
    expected.insert(expected.end(), first.begin(), first.end());
    require(ids_of(two) == expected, "a fragment list must read those fragments in the order given");

    // Projection: only the named top-level columns come back.
    const std::vector<std::string> columns = {"tag"};
    nano_lance::LanceScanRequest projected;
    projected.columns = &columns;
    Scan tags;
    require(scan_ok(path, projected, tags, error), "projected scan: " + error);
    require(tags.schema.n_children == 1 && std::string(tags.schema.children[0]->name) == "tag",
            "a projection must keep only the named column");

    // A row address is the fragment id in the high 32 bits and the offset in the low ones.
    nano_lance::LanceScanRequest addresses;
    addresses.with_row_address = true;
    const std::vector<std::string> only_id = {"id"};
    addresses.columns = &only_id;
    Scan with_addr;
    require(scan_ok(path, addresses, with_addr, error), "row address scan: " + error);
    require(with_addr.schema.n_children == 2, "with_row_address adds one column after the data columns");
    const ArrowArray& addr = *with_addr.batches.front().children[1];
    require(addr.length == kRowsPerFragment, "the first batch holds one fragment");
    const auto* a = static_cast<const std::uint64_t*>(addr.buffers[1]) + addr.offset;
    require(a[0] == 0 && a[kRowsPerFragment - 1] == kRowsPerFragment - 1,
            "fragment 0's row addresses must be 0 .. 499");
    const ArrowArray& last_batch = with_addr.batches.back();
    const auto* b = static_cast<const std::uint64_t*>(last_batch.children[1]->buffers[1]) +
                    last_batch.children[1]->offset;
    require(b[0] == (std::uint64_t{3} << 32), "fragment 3's first row address is 3 << 32");
}

void test_filter_and_index_default(const std::filesystem::path& path) {
    std::string error;
    const std::string filter = "id >= 1000 AND id < 1010";
    nano_lance::LanceScanRequest request;
    request.filter = &filter;
    Scan rows;
    require(scan_ok(path, request, rows, error), "filtered scan: " + error);
    require(ids_of(rows) == id_range(1000, 10), "a filter must return exactly the rows it accepts");

    // With a range: the range counts the rows that pass the filter.
    nano_lance::LanceScanRequest counted = request;
    counted.range.offset = 2;
    counted.range.length = 3;
    Scan some;
    require(scan_ok(path, counted, some, error), "filtered range scan: " + error);
    require(ids_of(some) == id_range(1002, 3), "a range over a filter counts the rows that pass");

    // use_scalar_index defaults on; with no index on id the rows are the same either way.
    nano_lance::LanceScanRequest no_index = request;
    no_index.use_scalar_index = false;
    Scan unindexed;
    require(scan_ok(path, no_index, unindexed, error), "unindexed filtered scan: " + error);
    require(ids_of(unindexed) == ids_of(rows), "use_scalar_index must not change the rows");
}

void test_failures(const std::filesystem::path& path) {
    std::string error;
    Scan missing;
    require(!scan_ok(path.string() + "_missing", nano_lance::LanceScanRequest{}, missing, error),
            "a scan of a missing dataset must fail");
    require(!error.empty(), "a failed scan must say why");
    require(missing.schema.release == nullptr && missing.batches.empty(),
            "a failed scan must leave nothing to release");

    const std::vector<std::string> no_such = {"no_such_column"};
    nano_lance::LanceScanRequest bad_column;
    bad_column.columns = &no_such;
    Scan none;
    require(!scan_ok(path, bad_column, none, error), "an unknown column must fail the scan");
    require(!error.empty(), "an unknown column must be named in the error");

    const std::vector<std::uint64_t> missing_fragment = {99};
    nano_lance::LanceScanRequest bad_fragment;
    bad_fragment.fragment_ids = &missing_fragment;
    Scan nothing;
    require(!scan_ok(path, bad_fragment, nothing, error), "a fragment the dataset does not have must fail");

    const std::string bad_filter = "id >>> 3";
    nano_lance::LanceScanRequest syntax;
    syntax.filter = &bad_filter;
    Scan unparsed;
    require(!scan_ok(path, syntax, unparsed, error), "a filter that does not parse must fail the scan");
}

void test_open_request(const std::filesystem::path& path) {
    const std::string filter = "id < 5 OR id >= 1995";
    nano_lance::LanceScanRequest request;
    request.filter = &filter;
    request.range.offset = 1;
    request.range.length = 6;  // rows 1..6 of the rows that pass: 1, 2, 3, 4, 1995, 1996

    ArrowSchema schema{};
    nano_lance::LanceTableStream stream;
    std::string error;
    require(nano_lance::LanceTableStream::open_request(path, request, schema, stream, error),
            "open_request: " + error);
    require(schema.n_children == 2, "open_request must return the schema of the scan");
    ArrowSchemaRelease(&schema);
    require(drain(stream) == std::vector<std::int64_t>({1, 2, 3, 4, 1995, 1996}),
            "open_request must yield the rows the request names, in order");

    // A failed open leaves the schema released and the stream unopened.
    nano_lance::LanceTableStream never;
    ArrowSchema failed{};
    require(!nano_lance::LanceTableStream::open_request(path.string() + "_missing",
                                                        nano_lance::LanceScanRequest{}, failed, never, error),
            "open_request on a missing dataset must fail");
    require(failed.release == nullptr, "a failed open_request must leave the schema released");
    ArrowArray batch{};
    std::string next_error;
    require(!never.next(batch, next_error) && !next_error.empty(),
            "next() on an unopened stream must fail with a message, not crash");
}

void run() {
    ScratchDir scratch;
    const auto dataset = scratch.path() / "dataset.lance";
    ArrowSchema schema{};
    require(build_schema(schema), "schema build failed");
    write_dataset(dataset, schema);
    ArrowSchemaRelease(&schema);

    test_full_and_range_scans(dataset);
    test_version_fragments_and_projection(dataset);
    test_filter_and_index_default(dataset);
    test_failures(dataset);
    test_open_request(dataset);
}

}  // namespace

int main() {
    try {
        run();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "dataset scan tests passed\n";
    return 0;
}
