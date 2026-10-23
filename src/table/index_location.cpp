// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "table/index_location.hpp"

#include "base/proto_wire.hpp"
#include "nanolance/byte_sink.hpp"
#include "nanolance/byte_source.hpp"
#include "nanolance/data_file_reader.hpp"

#include <system_error>

namespace nano_lance::index_location {

namespace {

/// Removes every object under the object-store prefix `dir` (its subdirectories' too), best effort.
void remove_remote_tree(const std::string& dir) {
    std::vector<io::DirEntry> entries;
    std::string ignored;  // a failed listing or removal leaves objects behind: harmless, cleanup finds them
    if (!io::list_dir(dir, entries, ignored)) {
        return;
    }
    for (const auto& entry : entries) {
        const std::string child = dir + "/" + entry.name;
        if (entry.is_dir) {
            remove_remote_tree(child);
        } else {
            io::remove_output(child, ignored);
        }
    }
}

}  // namespace

bool create_index_dir(const std::filesystem::path& dir, std::string& error) {
    if (io::is_object_store_output(dir.string())) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    return true;
}

void remove_index_dir(const std::filesystem::path& dir) {
    if (io::is_object_store_output(dir.string())) {
        remove_remote_tree(dir.string());
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    close_data_files_under(dir.string());
}

const pb::Field* find_field(const pb::Manifest& manifest, const std::string& path, std::vector<std::string>& parts) {
    parts.clear();
    for (const auto& f : manifest.fields) {
        if (f.parent_id == -1 && f.name == path) {
            parts.push_back(path);
            return &f;
        }
    }
    std::int32_t parent = -1;
    const pb::Field* found = nullptr;
    std::size_t start = 0;
    while (true) {
        const auto dot = path.find('.', start);
        const auto part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        found = nullptr;
        for (const auto& f : manifest.fields) {
            if (f.parent_id == parent && f.name == part) {
                found = &f;
                break;
            }
        }
        if (found == nullptr) {
            return nullptr;
        }
        parts.push_back(part);
        if (dot == std::string::npos) {
            return found;
        }
        parent = found->id;
        start = dot + 1;
    }
}

bool json_index_path(const std::vector<std::uint8_t>& details, std::string& path) {
    path.clear();
    proto_wire::Reader reader(details.data(), details.size());
    proto_wire::Field f;
    while (reader.next(f)) {
        if (f.number == 1U && f.wire == 2U) {
            path.assign(reinterpret_cast<const char*>(f.data), f.size);
            return true;
        }
    }
    return reader.ok();
}

}  // namespace nano_lance::index_location
