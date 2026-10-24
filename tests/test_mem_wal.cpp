// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// mem_wal: initializing MemWAL on a dataset (a CreateIndex commit of the `__lance_mem_wal` system index
// holding the given details) and reading the details back by version and as of the latest. Checks the
// index record the manifest ends up with (name, base version, details type, no fields or files, a fresh
// uuid, a creation time), that versions before the initialization have no MemWAL, that the details survive
// later commits byte for byte (empty and large ones too), and the refusals: no base version, a missing
// dataset or version, an index of that name whose details are not MemWAL's.

#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/mem_wal.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace nano_lance;
using Bytes = std::vector<std::uint8_t>;

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

fs::path make_dataset(const std::string& name) {
    auto ds = fs::temp_directory_path() / ("nano_lance_mem_wal_" + name);
    std::error_code ec;
    fs::remove_all(ds, ec);
    LanceSchemaMapping mapping;
    LanceField id;
    id.name = "id";
    id.logical_type = "int64";
    id.column_index = 0;
    mapping.fields.push_back(id);
    NewFragment f;
    f.data_file.relative_path = "data/a.lance";
    f.data_file.file_size_bytes = 10;
    f.rows = 4;
    std::uint64_t version = 0;
    std::string err;
    require(commit_dataset_version(ds, mapping, {f}, CommitMode::Create, version, err), "create: " + err);
    return ds;
}

void append_fragment(const fs::path& ds, const std::string& file) {
    LanceSchemaMapping mapping;
    NewFragment f;
    f.data_file.relative_path = "data/" + file;
    f.data_file.file_size_bytes = 10;
    f.rows = 2;
    std::uint64_t version = 0;
    std::string err;
    require(commit_dataset_version(ds, mapping, {f}, CommitMode::Append, version, err), "append: " + err);
}

void test_round_trip() {
    const auto ds = make_dataset("round_trip");
    std::optional<Bytes> details;
    std::string err;
    require(dataset_mem_wal_details(ds, false, 0, details, err) && !details, "no MemWAL before it is initialized: " + err);
    require(dataset_mem_wal_details(ds, true, 1, details, err) && !details, "...by version too");

    const Bytes config{0x08, 0x01, 0x12, 0x03, 'a', 'b', 'c'};
    std::uint64_t v2 = 0;
    const auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    require(dataset_initialize_mem_wal(ds, 1, config, v2, err), "initialize: " + err);
    require(v2 == 2U, "a new version");

    require(dataset_mem_wal_details(ds, false, 0, details, err) && details && *details == config, "latest holds the details: " + err);
    require(dataset_mem_wal_details(ds, true, 2, details, err) && details && *details == config, "by version");
    require(dataset_mem_wal_details(ds, true, 1, details, err) && !details, "the earlier version is untouched");

    // The index record in the manifest.
    pb::Manifest m1;
    pb::Manifest m2;
    require(load_manifest_version(ds, 1, m1, err) && load_manifest_version(ds, 2, m2, err), "load: " + err);
    require(m1.indices.empty() && m2.indices.size() == 1U, "one index added");
    const auto& index = m2.indices[0];
    require(index.name == kMemWalIndexName && std::string(kMemWalIndexName) == "__lance_mem_wal", "index name");
    require(index.dataset_version == 1U, "built on the base version");
    require(index.details_type_url == "/lance.table.MemWalIndexDetails" && index.details_value == config, "details Any");
    require(index.fields.empty() && index.files.empty() && !index.has_fragment_bitmap, "covers the table, no fields, files or fragments");
    require(index.index_version == 0U, "index version 0");
    require(static_cast<std::int64_t>(index.created_at) >= before &&
                static_cast<std::int64_t>(index.created_at) <=
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
            "created_at in milliseconds, now");
    bool nonzero_uuid = false;
    for (auto b : index.uuid) {
        nonzero_uuid = nonzero_uuid || b != 0U;
    }
    require(nonzero_uuid, "a fresh uuid");
    require(m2.fragments.size() == m1.fragments.size() && m2.fragments[0].physical_rows == 4U, "the data is as it was");

    // Later commits carry the index (and so the details) along.
    append_fragment(ds, "b.lance");
    require(dataset_mem_wal_details(ds, false, 0, details, err) && details && *details == config, "details survive an append: " + err);
    pb::Manifest m3;
    require(load_manifest_version(ds, 3, m3, err) && m3.indices.size() == 1U && m3.fragments.size() == 2U &&
                m3.indices[0].uuid == index.uuid,
            "same index, new fragment");
    fs::remove_all(ds);
}

void test_details_shapes() {
    const auto ds = make_dataset("shapes");
    std::string err;
    std::optional<Bytes> details;
    // Empty details are details, not "no MemWAL".
    std::uint64_t v = 0;
    require(dataset_initialize_mem_wal(ds, 1, {}, v, err), "initialize with empty details: " + err);
    require(dataset_mem_wal_details(ds, false, 0, details, err) && details && details->empty(), "present but empty");
    fs::remove_all(ds);

    const auto big = make_dataset("big");
    Bytes large(300000);
    for (std::size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<std::uint8_t>(i * 31U + 7U);
    }
    require(dataset_initialize_mem_wal(big, 1, large, v, err), "initialize with large details: " + err);
    require(dataset_mem_wal_details(big, true, v, details, err) && details && *details == large, "large details round trip");
    fs::remove_all(big);
}

void test_refusals() {
    const auto ds = make_dataset("refusals");
    std::string err;
    std::optional<Bytes> details{Bytes{1}};
    std::uint64_t v = 99;
    require(!dataset_initialize_mem_wal(ds, 0, {1}, v, err) && has(err, "Invalid user input"), "base version 0: " + err);
    pb::Manifest m;
    require(load_manifest_version(ds, 2, m, err) == false, "a refused initialize commits nothing");

    require(!dataset_mem_wal_details(ds, true, 7, details, err) && !err.empty() && !details, "a missing version: " + err);
    const auto nowhere = fs::temp_directory_path() / "nano_lance_mem_wal_nowhere";
    fs::remove_all(nowhere);
    details = Bytes{1};
    require(!dataset_mem_wal_details(nowhere, false, 0, details, err) && !err.empty() && !details, "a missing dataset: " + err);
    require(!dataset_initialize_mem_wal(nowhere, 1, {1}, v, err) && !err.empty(), "initialize on a missing dataset: " + err);

    // An index of the MemWAL name whose details are another type is refused rather than misread.
    pb::Manifest base;
    require(load_manifest_version(ds, 1, base, err), "load: " + err);
    pb::Manifest next = base;
    next.version = 2;
    next.transaction_file.clear();
    next.indices.push_back(pb::make_index_metadata({}, {}, kMemWalIndexName, 1, {}, "/lance.table.BTreeIndexDetails", 0, 1, {}));
    require(publish_manifest(ds, next, err), "publish a mistyped index: " + err);
    require(!dataset_mem_wal_details(ds, false, 0, details, err) && has(err, "not for the MemWAL index") &&
                has(err, "BTreeIndexDetails") && !details,
            "details of another type: " + err);
    fs::remove_all(ds);
}

}  // namespace

int main() {
    test_round_trip();
    test_details_shapes();
    test_refusals();
    std::cout << "mem_wal: ok\n";
    return 0;
}
