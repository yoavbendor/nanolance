// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/base_paths.hpp"

#include "proto_wire.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace nano_lance {

namespace {

using namespace proto_wire;

constexpr std::uint32_t kManifestBasePaths = 18;
constexpr std::uint32_t kManifestBranch = 20;
constexpr std::uint32_t kBaseIdField = 7;  // DataFile and DeletionFile
constexpr std::uint32_t kIndexBaseIdField = 9;  // IndexMetadata

/// `unknown` without the fields numbered `number`.
std::vector<std::uint8_t> without_field(const std::vector<std::uint8_t>& unknown, std::uint32_t number) {
    std::vector<std::uint8_t> out;
    Reader r(unknown.data(), unknown.size());
    Field f;
    while (r.next(f)) {
        if (f.number != number) {
            out.insert(out.end(), f.begin, f.end);
        }
    }
    return out;
}

/// The value of the varint field `number` in `unknown` (the last one wins, as protobuf says).
bool find_varint(const std::vector<std::uint8_t>& unknown, std::uint32_t number, std::optional<std::uint32_t>& out,
                 std::string& error) {
    out.reset();
    Reader r(unknown.data(), unknown.size());
    Field f;
    while (r.next(f)) {
        if (f.number == number && f.wire == 0) {
            if (f.value > std::numeric_limits<std::uint32_t>::max()) {
                error = "base_id does not fit in 32 bits";
                return false;
            }
            out = static_cast<std::uint32_t>(f.value);
        }
    }
    if (!r.ok()) {
        error = "malformed base_id";
        return false;
    }
    return true;
}

void put_base_id(std::vector<std::uint8_t>& unknown, std::optional<std::uint32_t> id) {
    unknown = without_field(unknown, kBaseIdField);
    if (id) {
        put_key(unknown, kBaseIdField, 0);
        put_varint(unknown, *id);
    }
}

bool decode_base_path(const std::uint8_t* data, std::size_t size, BasePath& out, std::string& error) {
    Reader r(data, size);
    Field f;
    while (r.next(f)) {
        switch (f.number) {
            case 1:
                if (f.wire != 0 || f.value > std::numeric_limits<std::uint32_t>::max()) {
                    error = "malformed base path id";
                    return false;
                }
                out.id = static_cast<std::uint32_t>(f.value);
                break;
            case 2:
                out.has_name = true;
                out.name.assign(reinterpret_cast<const char*>(f.data), f.size);
                break;
            case 3:
                out.is_dataset_root = f.value != 0;
                break;
            case 4:
                out.path.assign(reinterpret_cast<const char*>(f.data), f.size);
                break;
            default:
                break;
        }
    }
    if (!r.ok()) {
        error = "malformed base path";
        return false;
    }
    return true;
}

}  // namespace

bool decode_base_path_message(const std::uint8_t* data, std::size_t size, BasePath& out, std::string& error) {
    return decode_base_path(data, size, out, error);
}

bool read_base_paths(const pb::Manifest& manifest, std::vector<BasePath>& out, std::string& error) {
    out.clear();
    Reader r(manifest.unknown.data(), manifest.unknown.size());
    Field f;
    while (r.next(f)) {
        if (f.number != kManifestBasePaths) {
            continue;
        }
        if (f.wire != 2) {
            error = "malformed base paths";
            return false;
        }
        BasePath base;
        if (!decode_base_path(f.data, f.size, base, error)) {
            return false;
        }
        out.push_back(std::move(base));
    }
    if (!r.ok()) {
        error = "malformed manifest base paths";
        return false;
    }
    return true;
}

bool read_manifest_branch(const pb::Manifest& manifest, std::string& out, std::string& error) {
    out.clear();
    Reader r(manifest.unknown.data(), manifest.unknown.size());
    Field f;
    while (r.next(f)) {
        if (f.number == kManifestBranch && f.wire == 2) {
            out.assign(reinterpret_cast<const char*>(f.data), f.size);
        }
    }
    if (!r.ok()) {
        error = "malformed manifest branch";
        return false;
    }
    return true;
}

void write_base_paths(pb::Manifest& manifest, const std::vector<BasePath>& bases) {
    std::vector<std::uint8_t> out = without_field(manifest.unknown, kManifestBasePaths);
    for (const auto& base : bases) {
        std::vector<std::uint8_t> inner;
        if (base.id != 0) {
            put_key(inner, 1, 0);
            put_varint(inner, base.id);
        }
        if (base.has_name) {
            put_bytes_field(inner, 2, std::vector<std::uint8_t>(base.name.begin(), base.name.end()));
        }
        put_u64_field(inner, 3, base.is_dataset_root ? 1U : 0U);
        put_bytes_field(inner, 4, std::vector<std::uint8_t>(base.path.begin(), base.path.end()));
        put_bytes_field(out, kManifestBasePaths, inner);
    }
    manifest.unknown = std::move(out);
}

void write_manifest_branch(pb::Manifest& manifest, const std::string& branch) {
    std::vector<std::uint8_t> out = without_field(manifest.unknown, kManifestBranch);
    if (!branch.empty()) {
        put_bytes_field(out, kManifestBranch, std::vector<std::uint8_t>(branch.begin(), branch.end()));
    }
    manifest.unknown = std::move(out);
}

bool data_file_base_id(const pb::DataFile& file, std::optional<std::uint32_t>& out, std::string& error) {
    return find_varint(file.unknown, kBaseIdField, out, error);
}

void set_data_file_base_id(pb::DataFile& file, std::optional<std::uint32_t> id) {
    put_base_id(file.unknown, id);
}

bool deletion_file_base_id(const pb::DeletionFile& file, std::optional<std::uint32_t>& out, std::string& error) {
    return find_varint(file.unknown, kBaseIdField, out, error);
}

void set_deletion_file_base_id(pb::DeletionFile& file, std::optional<std::uint32_t> id) {
    put_base_id(file.unknown, id);
}

void set_index_base_id(pb::IndexMetadata& index, std::optional<std::uint32_t> id) {
    // IndexMetadata's field (9), not the file field the other base ids use.
    index.raw = without_field(index.raw, kIndexBaseIdField);
    if (id) {
        put_key(index.raw, kIndexBaseIdField, 0);
        put_varint(index.raw, *id);
    }
}

bool index_base_id(const pb::IndexMetadata& index, std::optional<std::uint32_t>& out, std::string& error) {
    Reader r(index.raw.data(), index.raw.size());
    Field f;
    out.reset();
    while (r.next(f)) {
        if (f.number == kIndexBaseIdField && f.wire == 0) {
            if (f.value > std::numeric_limits<std::uint32_t>::max()) {
                error = "index base_id does not fit in 32 bits";
                return false;
            }
            out = static_cast<std::uint32_t>(f.value);
        }
    }
    if (!r.ok()) {
        error = "malformed index base_id";
        return false;
    }
    return true;
}

namespace {

/// The base path `base_id` names; false (and `error`) when there is none.
bool find_base(const std::vector<BasePath>& bases, std::uint32_t base_id, const BasePath*& out, std::string& error) {
    const auto it = std::find_if(bases.begin(), bases.end(), [&](const BasePath& b) { return b.id == base_id; });
    if (it == bases.end()) {
        error = "refers to unknown base path " + std::to_string(base_id);
        return false;
    }
    out = &*it;
    return true;
}

}  // namespace

bool data_file_dir(const std::filesystem::path& dataset_path, const std::vector<BasePath>& bases,
                   const pb::DataFile& file, std::filesystem::path& out, std::string& error) {
    std::optional<std::uint32_t> base_id;
    if (!data_file_base_id(file, base_id, error)) {
        return false;
    }
    if (!base_id) {
        out = dataset_path / "data";
        return true;
    }
    const BasePath* base = nullptr;
    if (!find_base(bases, *base_id, base, error)) {
        error = "data file " + error;
        return false;
    }
    out = base->is_dataset_root ? std::filesystem::path(base->path) / "data" : std::filesystem::path(base->path);
    return true;
}

bool deletion_dir(const std::filesystem::path& dataset_path, const std::vector<BasePath>& bases,
                  std::optional<std::uint32_t> base_id, std::filesystem::path& out, std::string& error) {
    if (!base_id) {
        out = dataset_path / "_deletions";
        return true;
    }
    const BasePath* base = nullptr;
    if (!find_base(bases, *base_id, base, error)) {
        error = "deletion file " + error;
        return false;
    }
    if (!base->is_dataset_root) {
        error = "deletion file names base path " + std::to_string(*base_id) + ", which is not a dataset root";
        return false;
    }
    out = std::filesystem::path(base->path) / "_deletions";
    return true;
}

bool deletion_file_dir(const std::filesystem::path& dataset_path, const std::vector<BasePath>& bases,
                       const pb::DeletionFile& file, std::filesystem::path& out, std::string& error) {
    std::optional<std::uint32_t> base_id;
    if (!deletion_file_base_id(file, base_id, error)) {
        return false;
    }
    return deletion_dir(dataset_path, bases, base_id, out, error);
}

bool deletion_file_dir(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                       const pb::DeletionFile& file, std::filesystem::path& out, std::string& error) {
    std::optional<std::uint32_t> base_id;
    if (!deletion_file_base_id(file, base_id, error)) {
        return false;
    }
    if (!base_id) {
        return deletion_dir(dataset_path, {}, base_id, out, error);
    }
    std::vector<BasePath> bases;
    if (!read_base_paths(manifest, bases, error)) {
        return false;
    }
    return deletion_dir(dataset_path, bases, base_id, out, error);
}

bool index_dir_of(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                  const pb::IndexMetadata& index, std::filesystem::path& out, std::string& error) {
    std::optional<std::uint32_t> base_id;
    if (!index_base_id(index, base_id, error)) {
        return false;
    }
    std::vector<BasePath> bases;
    if (base_id && !read_base_paths(manifest, bases, error)) {
        return false;
    }
    std::filesystem::path dir;
    if (!indices_dir(dataset_path, bases, base_id, dir, error)) {
        return false;
    }
    out = dir / pb::uuid_string(index.uuid);
    return true;
}

bool indices_dir(const std::filesystem::path& dataset_path, const std::vector<BasePath>& bases,
                 std::optional<std::uint32_t> base_id, std::filesystem::path& out, std::string& error) {
    if (!base_id) {
        out = dataset_path / "_indices";
        return true;
    }
    const BasePath* base = nullptr;
    if (!find_base(bases, *base_id, base, error)) {
        error = "index " + error;
        return false;
    }
    out = base->is_dataset_root ? std::filesystem::path(base->path) / "_indices" : std::filesystem::path(base->path);
    return true;
}

}  // namespace nano_lance
