// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Reading an index's Lance files (internal): what the scalar and vector index searches share.

#pragma once

#include "nanolance/data_file_reader.hpp"
#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
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

}  // namespace nano_lance::index_files
