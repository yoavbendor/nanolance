// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "index_files.hpp"

#include <system_error>

namespace nano_lance::index_files {

FileTable::~FileTable() {
    for (auto& b : batches) {
        if (b.release != nullptr) {
            b.release(&b);
        }
    }
    if (schema.release != nullptr) {
        schema.release(&schema);
    }
}

bool read_table(const std::filesystem::path& path, const std::vector<std::string>* columns, const LanceRowRange& range,
                FileTable& out, std::string& error) {
    LanceScanRequest request;
    request.columns = columns;
    request.range = range;
    if (!lance_file_read(path, request, out.schema, out.batches, error)) {
        out.schema = ArrowSchema{};  // released by the reader on failure
        error = path.filename().string() + ": " + error;
        return false;
    }
    return true;
}

bool take_table(const std::filesystem::path& path, const std::vector<std::string>* columns,
                const std::vector<std::uint64_t>& rows, FileTable& out, std::string& error) {
    LanceScanRequest request;
    request.columns = columns;
    if (!lance_file_take(path, request, rows, out.schema, out.batches, error)) {
        out.schema = ArrowSchema{};
        error = path.filename().string() + ": " + error;
        return false;
    }
    return true;
}

std::string file_key(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    const auto mtime = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
    return path.string() + "|" + std::to_string(size) + "|" + std::to_string(static_cast<long long>(mtime));  // libc++'s file_clock counts in __int128
}

bool schema_metadata(const std::filesystem::path& path, const std::string& key, std::string& value, bool& found,
                     LanceDataFileFooterLayout& layout, std::string& error) {
    pb::FileDescriptor descriptor;
    if (!read_lance_data_file_footer_and_descriptor(path, descriptor, layout, error)) {
        return false;
    }
    const auto it = descriptor.schema_metadata.find(key);
    found = it != descriptor.schema_metadata.end();
    if (found) {
        value.assign(it->second.begin(), it->second.end());
    }
    return true;
}

}  // namespace nano_lance::index_files
