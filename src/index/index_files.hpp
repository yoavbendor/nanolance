// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Reading an index's Lance files (internal): what the scalar and vector index searches share.

#pragma once

#include "nanolance/data_file_reader.hpp"
#include "nanolance/data_file_writer.hpp"
#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
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

/// The bytes a BLOOMFILTER hashes per value (as_bytes.rs): a fixed-width value's bytes (`width`), a
/// string's or binary's bytes (width 0). False, with `error`, for a type Lance's filter does not take.
bool bloom_value_width(const ArrowSchemaView& view, std::size_t& width, std::string& error);

/// The hash of element `i` of `v`: XXH64 of the bytes `bloom_value_width` names for its `width`.
std::uint64_t bloom_hash(const ArrowArrayView* v, std::int64_t i, std::size_t width);

/// The values per zone and the probability of the bloomfilter.lance in `dir` (its schema metadata).
bool bloomfilter_params(const std::filesystem::path& dir, std::uint64_t& number_of_items, double& probability,
                        std::string& error);

/// The rows per zone of the zonemap.lance in `dir` (its schema metadata).
bool zonemap_rows_per_zone(const std::filesystem::path& dir, std::uint64_t& rows_per_zone, std::string& error);

// ── writing ─────────────────────────────────────────────────────────────────────────────────────

struct WrittenFile {
    std::string name;
    std::uint64_t size = 0;
};

/// Write `batch` as the Lance file `dir / name`, recorded in `files`.
bool write_file(const std::filesystem::path& dir, const std::string& name, const ArrowSchema& schema,
                ArrowArray& batch, LanceFileExtras extras, std::vector<WrittenFile>& files, std::string& error);

}  // namespace nano_lance::index_files
