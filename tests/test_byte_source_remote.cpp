// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Every read of a dataset goes through the byte-source interface. A dataset is written locally (several
// fragments, a deletion, a BTREE index), its expected results are read from the local path, and then the
// dataset is copied into an in-memory object store registered as the `mem://` scheme and its local
// directory is REMOVED. The same reads through `mem://` must then return the same rows; a read that
// touched the local file system could not succeed. The store counts what it serves: opens, range reads
// and listings, per kind of file (manifests, data, deletion files, index files), so the test also shows
// each kind of file was read through the interface.
#include "nanolance/byte_source.hpp"
#include "nanolance/dataset_ops.hpp"
#include "nanolance/index_search.hpp"
#include "nanolance/index_segments.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace nano_lance;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAILED: " << msg << '\n';
        std::exit(1);
    }
}

/// `require` for a call that sets `err`: the message is built after the call ran.
void require_err(bool ok, const char* msg, const std::string& err) {
    if (!ok) {
        std::cerr << "FAILED: " << msg << ": " << err << '\n';
        std::exit(1);
    }
}

constexpr const char* kScheme = "mem://";

/// The object store: the dataset's files by absolute path, and the directories above them.
struct Store {
    std::mutex mu;
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::set<std::string> dirs;
    std::map<std::string, std::uint64_t> counts;  // "open:manifest", "read:data", "list:deletion", ...

    void count(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu);
        ++counts[key];
    }
    std::uint64_t get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = counts.find(key);
        return it == counts.end() ? 0 : it->second;
    }
};

Store& store() {
    static Store s;
    return s;
}

/// The kind of a dataset file, from its path.
std::string kind_of(const std::string& path) {
    if (path.find("/_versions/") != std::string::npos) return "manifest";
    if (path.find("/data/") != std::string::npos) return "data";
    if (path.find("/_deletions/") != std::string::npos) return "deletion";
    if (path.find("/_indices/") != std::string::npos) return "index";
    return "other";
}

/// The object key of a `mem://` URI: its absolute local path.
std::string key_of(const std::string& uri) { return uri.substr(std::strlen(kScheme)); }

std::string parent_of(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
}

class MemSource final : public io::ByteSource {
public:
    MemSource(std::shared_ptr<const std::vector<std::uint8_t>> bytes, std::string kind)
        : bytes_(std::move(bytes)), kind_(std::move(kind)) {}
    std::uint64_t size() const override { return bytes_->size(); }
    bool read_at(std::uint64_t offset, std::uint64_t n, std::uint8_t* dst, std::string& error) override {
        store().count("read:" + kind_);
        if (offset > bytes_->size() || n > bytes_->size() - offset) {
            error = "short read from mem://";
            return false;
        }
        std::memcpy(dst, bytes_->data() + offset, static_cast<std::size_t>(n));
        return true;
    }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> bytes_;
    std::string kind_;
};

std::unique_ptr<io::ByteSource> mem_open(const std::string& uri, std::string& error) {
    const auto key = key_of(uri);
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mu);
    const auto it = s.files.find(key);
    if (it == s.files.end()) {
        error = "no such object: " + key;
        return nullptr;
    }
    const auto kind = kind_of(key);
    ++s.counts["open:" + kind];
    // Copy-on-open: the source holds its own bytes, as a remote object would.
    auto bytes = std::make_shared<const std::vector<std::uint8_t>>(it->second);
    return std::make_unique<MemSource>(std::move(bytes), kind);
}

bool mem_list(const std::string& uri, std::vector<io::DirEntry>& out, std::string& error) {
    const auto dir = key_of(uri);
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mu);
    ++s.counts["list:" + kind_of(dir + "/")];
    for (const auto& [path, bytes] : s.files) {
        if (parent_of(path) == dir) {
            out.push_back({path.substr(path.rfind('/') + 1), bytes.size(), false});
        }
    }
    for (const auto& path : s.dirs) {
        if (parent_of(path) == dir) {
            out.push_back({path.substr(path.rfind('/') + 1), 0, true});
        }
    }
    error.clear();
    return true;
}

/// Copies every file of the dataset at `root` into the store, keyed by its absolute path.
void snapshot(const fs::path& root) {
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mu);
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto path = entry.path().generic_string();
        if (entry.is_directory()) {
            s.dirs.insert(path);
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        s.files[path] = std::move(bytes);
        for (auto dir = parent_of(path); dir.size() > root.generic_string().size(); dir = parent_of(dir)) {
            s.dirs.insert(dir);
        }
    }
}

void write_batch(NanoLanceWriter& writer, std::int64_t first, std::int64_t rows) {
    ArrowArray batch{};
    batch.length = rows;
    ArrowSchema field{};
    field.format = "l";
    field.name = "id";
    field.flags = 0;
    std::vector<std::int64_t> values(static_cast<std::size_t>(rows));
    for (std::int64_t i = 0; i < rows; ++i) values[static_cast<std::size_t>(i)] = first + i;
    const void* buffers[] = {nullptr, values.data()};
    batch.n_buffers = 2;
    batch.buffers = buffers;
    require(nano_lance_write_batch(&writer, &batch, &field) == NANO_LANCE_OK, "write batch");
}

/// The `id` column of scan or take results, in order; releases them.
std::vector<std::int64_t> ids_of(ArrowSchema& schema, std::vector<ArrowArray>& batches) {
    std::size_t col = 0;
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (std::strcmp(schema.children[i]->name, "id") == 0) col = static_cast<std::size_t>(i);
    }
    std::vector<std::int64_t> ids;
    for (auto& batch : batches) {
        const ArrowArray* child = batch.children[col];
        const auto* values = static_cast<const std::int64_t*>(child->buffers[1]);
        for (std::int64_t r = 0; r < batch.length; ++r) {
            ids.push_back(values[child->offset + r]);
        }
        if (batch.release) batch.release(&batch);
    }
    if (schema.release) schema.release(&schema);
    return ids;
}

struct Reads {
    std::vector<std::int64_t> all;
    std::vector<std::int64_t> taken;
    std::vector<std::int64_t> filtered;
    std::vector<std::int64_t> indexed;
    std::vector<std::int64_t> latest;
    std::uint64_t count = 0;
};

/// The reads under test, through `dataset` (a path or a mem:// URI).
Reads read_all_ways(const std::string& dataset, std::uint64_t latest_version) {
    const std::vector<std::string> columns{"id"};
    const std::vector<std::uint64_t> indices{0, 5, 17, 120, 200};
    const std::string filter = "id % 2 = 0 AND id < 150";
    const std::string indexed_filter = "id = 42";
    const fs::path path(dataset);
    std::string err;
    Reads r;

    LanceScanRequest req;
    req.columns = &columns;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    require_err(lance_dataset_scan(path, req, schema, batches, err), "scan", err);
    r.all = ids_of(schema, batches);

    require_err(lance_dataset_take(path, req, indices, schema, batches, err), "take", err);
    r.taken = ids_of(schema, batches);

    LanceScanRequest filtered = req;
    filtered.filter = &filter;
    require_err(lance_dataset_scan(path, filtered, schema, batches, err), "filtered scan", err);
    r.filtered = ids_of(schema, batches);
    r.count = r.filtered.size();

    LanceScanRequest indexed = req;
    indexed.filter = &indexed_filter;
    indexed.index_candidates = index_candidates;  // the planner's part: answer the filter from indexes
    require_err(lance_dataset_scan(path, indexed, schema, batches, err), "indexed scan", err);
    r.indexed = ids_of(schema, batches);

    LanceScanRequest at_version = req;
    at_version.has_version = true;
    at_version.version = latest_version;
    require_err(lance_dataset_scan(path, at_version, schema, batches, err), "version scan", err);
    r.latest = ids_of(schema, batches);
    return r;
}

}  // namespace

int main() {
    // Factories, and a scheme with a source but no lister (to check the error path).
    io::register_source_factory("mem", [](const std::string& uri, std::string& error) {
        return mem_open(uri, error);
    });
    io::register_list_factory("mem", mem_list);
    io::register_source_factory("nolist", [](const std::string&, std::string&) {
        return std::unique_ptr<io::ByteSource>();
    });

    const auto ds = fs::temp_directory_path() / "nanolance_byte_source_remote_ds";
    std::error_code ec;
    fs::remove_all(ds, ec);

    // ── 1. Write a local dataset: three fragments, a deletion, a BTREE index. ──────────────────────────
    for (int f = 0; f < 3; ++f) {
        NanoLanceWriter writer{};
        const std::string dir = ds.string();
        require((f == 0 ? nano_lance_writer_init(&writer, dir.c_str(), 3)
                        : nano_lance_writer_init_append(&writer, dir.c_str(), 3)) == NANO_LANCE_OK,
                "writer init");
        write_batch(writer, f * 100, 100);
        require(nano_lance_writer_commit(&writer, f != 0) == NANO_LANCE_OK, "writer commit");
        require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "writer close");
    }
    std::uint64_t deleted = 0;
    std::uint64_t version = 0;
    std::string err;
    require_err(dataset_delete(ds, "id % 7 = 3", deleted, version, err) && deleted > 0, "delete", err);

    IndexSegmentBuild build;
    build.type = "BTREE";
    build.name = "id_idx";
    std::vector<std::uint8_t> segment;
    SegmentErrorKind kind = SegmentErrorKind::None;
    require_err(dataset_build_index_segment(ds, "id", build, segment, err, kind), "build index", err);
    require_err(dataset_commit_index_segments(ds, "id_idx", "id", {segment}, version, err, kind), "commit index", err);

    // ── 2. Expected results, read from the local path. ─────────────────────────────────────────────────
    const auto versions = list_manifest_versions(ds, err);
    require(!versions.empty() && versions.back() == version, "latest version is the index commit");
    const auto expected = read_all_ways(ds.string(), version);
    std::uint64_t local_rows = 0;
    require_err(lance_table_count_rows(ds, local_rows, err), "count rows locally", err);
    require(expected.all.size() == 300 - deleted, "full scan skips the deleted rows");
    require(expected.count > 0 && expected.indexed.size() == 1, "filtered and indexed reads have rows");

    // ── 3. Move the dataset into the object store and remove its local directory. ──────────────────────
    snapshot(ds);
    fs::remove_all(ds, ec);
    require(!fs::exists(ds), "the local dataset directory is gone");
    require(!io::exists(ds.string() + "/_versions"), "io::exists sees no local dataset");

    // ── 4. The same reads through mem://. ──────────────────────────────────────────────────────────────
    const std::string remote = kScheme + ds.generic_string();
    std::vector<io::DirEntry> listing;
    require_err(io::list_dir(remote + "/_versions", listing, err) && !listing.empty(), "list _versions", err);
    require(io::exists(remote + "/_versions/" + listing.front().name), "a manifest exists through mem://");
    const auto remote_versions = list_manifest_versions(remote, err);
    require(remote_versions == versions, "versions through mem://");

    std::vector<std::uint8_t> remote_bytes;
    require_err(io::read_all(remote + "/_versions/" + listing.front().name, remote_bytes, err) && !remote_bytes.empty(),
                "read_all through mem://", err);

    const auto got = read_all_ways(remote, version);
    require(got.all == expected.all, "full scan through mem:// equals the local scan");
    require(got.taken == expected.taken, "take through mem:// equals the local take");
    require(got.filtered == expected.filtered, "filtered scan through mem:// equals the local one");
    require(got.count == expected.count, "filtered row count through mem://");
    require(got.indexed == expected.indexed, "indexed read through mem:// equals the local one");
    require(got.latest == expected.latest, "read of the latest version through mem:// equals the local one");

    std::uint64_t rows = 0;
    require_err(lance_table_count_rows(remote, rows, err) && rows == local_rows, "count rows through mem://", err);

    // ── 5. Every kind of file was read through the interface. ──────────────────────────────────────────
    auto& s = store();
    const std::pair<const char*, const char*> expect_positive[] = {
        {"open:manifest", "manifest opens"},   {"read:manifest", "manifest reads"},
        {"list:manifest", "manifest listings"}, {"open:data", "data file opens"},
        {"read:data", "data file reads"},      {"open:deletion", "deletion file opens"},
        {"read:deletion", "deletion file reads"}, {"open:index", "index file opens"},
    };
    for (const auto& [key, what] : expect_positive) {
        std::cout << what << ": " << s.get(key) << '\n';
        require(s.get(key) > 0, std::string("no ") + what + " through mem://");
    }
    std::cout << "list calls (manifest/data/deletion/index): " << s.get("list:manifest") << ' '
              << s.get("list:data") << ' ' << s.get("list:deletion") << ' ' << s.get("list:index") << '\n';

    // ── 6. Local-only semantics that the remote path mirrors. ──────────────────────────────────────────
    std::vector<io::DirEntry> none;
    require(io::list_dir("/nonexistent-nanolance-dir", none, err) && none.empty(), "missing local dir lists empty");
    require(!io::exists("/nonexistent-nanolance-dir/x"), "missing local file does not exist");
    require(!io::list_dir("nolist://bucket/dir", none, err) && err.find("no directory listing") != std::string::npos,
            "a scheme with no lister is an error, not an empty listing");

    io::register_source_factory("mem", nullptr);
    io::register_list_factory("mem", nullptr);
    io::register_source_factory("nolist", nullptr);
    std::cout << "byte_source remote test passed\n";
    return 0;
}
