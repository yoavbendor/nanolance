// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "index_files.hpp"

#include "nanolance/lance_file_writer.hpp"

#include <chrono>
#include <cstring>
#include <random>

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

bool uint_array(ArrowType type, const void* values, std::int64_t n, std::size_t width, ArrowArray& out,
                std::string& error) {
    if (ArrowArrayInitFromType(&out, type) != NANOARROW_OK ||
        ArrowBufferAppend(ArrowArrayBuffer(&out, 1), values, n * static_cast<std::int64_t>(width)) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    out.length = n;
    out.null_count = 0;
    return ArrowArrayFinishBuildingDefault(&out, nullptr) == NANOARROW_OK || (error = "bad array", false);
}

/// A struct schema of `children` (name, type to copy, nullable).
bool struct_schema(const std::vector<std::tuple<const char*, const ArrowSchema*, bool>>& children, ArrowSchema& out,
                   std::string& error) {
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetTypeStruct(&out, static_cast<std::int64_t>(children.size())) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        ArrowSchema* child = out.children[i];
        child->release(child);
        if (ArrowSchemaDeepCopy(std::get<1>(children[i]), child) != NANOARROW_OK ||
            ArrowSchemaSetName(child, std::get<0>(children[i])) != NANOARROW_OK ||
            ArrowSchemaSetMetadata(child, nullptr) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
        child->flags = std::get<2>(children[i]) ? ARROW_FLAG_NULLABLE : 0;
    }
    return true;
}

bool type_schema(ArrowType type, ArrowSchema& out) {
    ArrowSchemaInit(&out);
    return ArrowSchemaSetType(&out, type) == NANOARROW_OK;
}

bool struct_batch(std::vector<ArrowArray*> children, std::int64_t length, ArrowArray& out, std::string& error) {
    if (ArrowArrayInitFromType(&out, NANOARROW_TYPE_STRUCT) != NANOARROW_OK ||
        ArrowArrayAllocateChildren(&out, static_cast<std::int64_t>(children.size())) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        ArrowArrayMove(children[i], out.children[i]);
    }
    out.length = length;
    out.null_count = 0;
    return true;
}

std::vector<std::uint8_t> bytes_of(const std::string& s) { return {s.begin(), s.end()}; }

bool write_file(const std::filesystem::path& dir, const std::string& name, const ArrowSchema& schema,
                ArrowArray& batch, LanceFileExtras extras, std::vector<WrittenFile>& files, std::string& error) {
    extras.path = dir / name;
    std::uint64_t size = 0;
    const std::vector<const ArrowArray*> batches = {&batch};
    if (!write_lance_file(schema, batches, extras, size, error)) {
        error = name + ": " + error;
        return false;
    }
    files.push_back({name, size});
    return true;
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

std::array<std::uint8_t, 16> new_uuid() {
    std::random_device device;
    std::mt19937_64 rng((static_cast<std::uint64_t>(device()) << 32U) ^ device() ^
                        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::array<std::uint8_t, 16> id{};
    for (std::size_t i = 0; i < id.size(); i += 8) {
        const auto r = rng();
        std::memcpy(id.data() + i, &r, 8);
    }
    id[6] = static_cast<std::uint8_t>((id[6] & 0x0FU) | 0x40U);  // version 4
    id[8] = static_cast<std::uint8_t>((id[8] & 0x3FU) | 0x80U);  // RFC 4122 variant
    return id;
}

}  // namespace nano_lance::index_files
