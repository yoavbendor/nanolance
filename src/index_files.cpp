// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "index_files.hpp"

#include "nanolance/bloom_filter.hpp"

#include "nanolance/lance_file_writer.hpp"
#include "proto_wire.hpp"

#include <chrono>
#include <cstdlib>
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

/// The bytes a BLOOMFILTER hashes per value (as_bytes.rs): a fixed-width value's bytes (`width`), a
/// string's or binary's bytes (width 0). False, with `error`, for a type Lance's filter does not take.
bool bloom_value_width(const ArrowSchemaView& view, std::size_t& width, std::string& error) {
    switch (view.type) {
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_UINT8:
        width = 1;
        break;
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_UINT16:
        width = 2;
        break;
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DATE32:
    case NANOARROW_TYPE_TIME32:
        width = 4;
        break;
    case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_UINT64:
    case NANOARROW_TYPE_DOUBLE:
    case NANOARROW_TYPE_DATE64:
    case NANOARROW_TYPE_TIME64:
    case NANOARROW_TYPE_TIMESTAMP:
        width = 8;
        break;
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY:
        width = 0;
        break;
    default:
        error = std::string("a BLOOMFILTER index on a column of type ") + ArrowTypeString(view.type) +
                " is not supported";
        return false;
    }
    return true;
}

/// The hash of item `k`'s value: XXH64 of the bytes `bloom_value_width` names.
std::uint64_t bloom_hash(const ArrowArrayView* v, std::int64_t i, std::size_t width) {
    if (width == 0) {
        const auto b = ArrowArrayViewGetBytesUnsafe(v, i);
        return bloom_filter::xxh64(reinterpret_cast<const std::uint8_t*>(b.data.as_char),
                                   static_cast<std::size_t>(b.size_bytes));
    }
    return bloom_filter::xxh64(v->buffer_views[1].data.as_uint8 + static_cast<std::size_t>(v->offset + i) * width, width);
}

bool zonemap_rows_per_zone(const std::filesystem::path& dir, std::uint64_t& rows_per_zone, std::string& error) {
    std::string value;
    bool found = false;
    LanceDataFileFooterLayout layout{};
    if (!schema_metadata(dir / "zonemap.lance", "rows_per_zone", value, found, layout, error)) {
        return false;
    }
    if (!found || value.empty()) {
        error = "zonemap.lance has no rows_per_zone";
        return false;
    }
    rows_per_zone = std::strtoull(value.c_str(), nullptr, 10);
    if (rows_per_zone == 0) {
        error = "zonemap.lance has rows_per_zone 0";
        return false;
    }
    return true;
}

bool bloomfilter_params(const std::filesystem::path& dir, std::uint64_t& number_of_items, double& probability,
                        std::string& error) {
    std::string items;
    std::string fpp;
    bool found = false;
    LanceDataFileFooterLayout layout{};
    if (!schema_metadata(dir / "bloomfilter.lance", "bloomfilter_item", items, found, layout, error)) {
        return false;
    }
    if (!found || items.empty() || !schema_metadata(dir / "bloomfilter.lance", "bloomfilter_probability", fpp, found,
                                                    layout, error) ||
        !found || fpp.empty()) {
        error = "bloomfilter.lance has no bloomfilter_item or bloomfilter_probability";
        return false;
    }
    number_of_items = std::strtoull(items.c_str(), nullptr, 10);
    probability = std::strtod(fpp.c_str(), nullptr);
    if (number_of_items == 0 || !(probability > 0.0 && probability < 1.0)) {
        error = "bloomfilter.lance has bloomfilter_item " + items + " and bloomfilter_probability " + fpp;
        return false;
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

}  // namespace nano_lance::index_files
