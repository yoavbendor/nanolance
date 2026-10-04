// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Reading an index's Lance files (internal): what the scalar and vector index searches share.

#pragma once

#include "nanolance/data_file_reader.hpp"
#include "nanolance/data_file_writer.hpp"
#include "nanolance/lance_table_reader.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace nano_lance::index_files {

/// A read of an index file: its schema and batches, owned.
struct FileTable {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    FileTable() = default;
    FileTable(const FileTable&) = delete;
    FileTable& operator=(const FileTable&) = delete;
    ~FileTable();
    const ArrowSchema& type(std::size_t column) const { return *schema.children[column]; }
};

bool read_table(const std::filesystem::path& path, const std::vector<std::string>* columns, const LanceRowRange& range,
                FileTable& out, std::string& error);

bool take_table(const std::filesystem::path& path, const std::vector<std::string>* columns,
                const std::vector<std::uint64_t>& rows, FileTable& out, std::string& error);

/// A cache key for `path`: an index's files never change, but a path could be reused by a dataset
/// written anew, so size and modification time are part of it.
std::string file_key(const std::filesystem::path& path);

/// `key` of the file's schema metadata (`found` false when absent); `layout` is its footer.
bool schema_metadata(const std::filesystem::path& path, const std::string& key, std::string& value, bool& found,
                     LanceDataFileFooterLayout& layout, std::string& error);

/// Loaded indices, by file_key.
template <typename T>
class IndexCache {
public:
    std::shared_ptr<const T> find(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : it->second;
    }
    void put(const std::string& key, std::shared_ptr<const T> value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.size() >= 64U) {
            entries_.erase(entries_.begin());
        }
        entries_[key] = std::move(value);
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const T>> entries_;
};


// ── writing ─────────────────────────────────────────────────────────────────────────────────────

struct OwnedSchema {
    ArrowSchema s{};
    OwnedSchema() = default;
    OwnedSchema(const OwnedSchema&) = delete;
    OwnedSchema& operator=(const OwnedSchema&) = delete;
    ~OwnedSchema() {
        if (s.release != nullptr) {
            s.release(&s);
        }
    }
};

struct OwnedArray {
    ArrowArray a{};
    OwnedArray() = default;
    OwnedArray(const OwnedArray&) = delete;
    OwnedArray& operator=(const OwnedArray&) = delete;
    ~OwnedArray() {
        if (a.release != nullptr) {
            a.release(&a);
        }
    }
};

struct OwnedBatches {
    std::vector<ArrowArray> v;
    ~OwnedBatches() {
        for (auto& a : v) {
            if (a.release != nullptr) {
                a.release(&a);
            }
        }
    }
};

struct OwnedViews {
    std::vector<std::unique_ptr<ArrowArrayView>> v;
    ~OwnedViews() {
        for (auto& view : v) {
            ArrowArrayViewReset(view.get());
        }
    }
};

/// A non-nullable array of `n` fixed-width values of `type` (`width` bytes each).
bool uint_array(ArrowType type, const void* values, std::int64_t n, std::size_t width, ArrowArray& out,
                std::string& error);

/// A struct schema of `children` (name, type to copy, nullable).
bool struct_schema(const std::vector<std::tuple<const char*, const ArrowSchema*, bool>>& children, ArrowSchema& out,
                   std::string& error);

bool type_schema(ArrowType type, ArrowSchema& out);

/// A struct batch of `children` (moved in).
bool struct_batch(std::vector<ArrowArray*> children, std::int64_t length, ArrowArray& out, std::string& error);

std::vector<std::uint8_t> bytes_of(const std::string& s);

struct WrittenFile {
    std::string name;
    std::uint64_t size = 0;
};

/// Write `batch` as the Lance file `dir / name`, recorded in `files`.
bool write_file(const std::filesystem::path& dir, const std::string& name, const ArrowSchema& schema,
                ArrowArray& batch, LanceFileExtras extras, std::vector<WrittenFile>& files, std::string& error);

/// The field at dotted `path` (its parts in `parts`), or null.
const pb::Field* find_field(const pb::Manifest& manifest, const std::string& path, std::vector<std::string>& parts);

/// A random (version 4) UUID.
std::array<std::uint8_t, 16> new_uuid();

}  // namespace nano_lance::index_files
