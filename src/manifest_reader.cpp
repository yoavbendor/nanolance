// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/manifest_reader.hpp"

#include "nanolance/read_safety.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace nano_lance {
namespace {

bool read_exact(std::istream& in, void* dest, std::size_t n) {
    return static_cast<bool>(in.read(static_cast<char*>(dest), static_cast<std::streamsize>(n)));
}

std::uint32_t read_le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::uint64_t read_le64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8U * static_cast<unsigned>(i));
    }
    return v;
}

}  // namespace

std::uint64_t highest_manifest_version(const std::filesystem::path& dataset_path, std::string& error) {
    error.clear();
    const auto versions_dir = dataset_path / "_versions";
    std::error_code ec;
    if (!std::filesystem::exists(versions_dir, ec)) {
        if (ec) {
            error = "failed to stat _versions: " + ec.message();
            return 0;
        }
        return 0;
    }
    std::uint64_t max_version = 0;
    for (const auto& entry : std::filesystem::directory_iterator(versions_dir, ec)) {
        if (ec) {
            error = "failed to iterate _versions: " + ec.message();
            return 0;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.size() <= 9 || name.substr(name.size() - 9) != ".manifest") {
            continue;
        }
        try {
            max_version =
                std::max(max_version, static_cast<std::uint64_t>(std::stoull(name.substr(0, name.size() - 9))));
        } catch (...) {
        }
    }
    return max_version;
}

bool read_manifest_file(const std::filesystem::path& manifest_path, std::vector<std::uint8_t>& protobuf_body,
                        std::string& error) {
    error.clear();
    protobuf_body.clear();
    std::ifstream in(manifest_path, std::ios::binary);
    if (!in) {
        error = "failed to open manifest file";
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto end_pos = in.tellg();
    if (end_pos < static_cast<std::streamoff>(8 + 16)) {
        error = "manifest file too small";
        return false;
    }
    const auto file_size = static_cast<std::uint64_t>(end_pos);
    in.seekg(static_cast<std::streamoff>(file_size - 16U), std::ios::beg);
    unsigned char trailer[16]{};
    if (!read_exact(in, trailer, 16)) {
        error = "failed to read manifest trailer";
        return false;
    }
    if (std::memcmp(trailer + 12, "LANC", 4) != 0) {
        error = "manifest trailer magic is not LANC";
        return false;
    }
    const std::uint16_t minor = static_cast<std::uint16_t>(trailer[10] | (static_cast<unsigned>(trailer[11]) << 8U));
    if (minor != 2U) {
        error = "unsupported manifest minor version (expected 2)";
        return false;
    }
    const std::uint64_t manifest_position = read_le64(trailer);
    if (manifest_position > file_size - 16U) {
        error = "invalid manifest_position in trailer";
        return false;
    }
    in.seekg(static_cast<std::streamoff>(manifest_position), std::ios::beg);
    unsigned char len_le[4]{};
    if (!read_exact(in, len_le, 4)) {
        error = "failed to read manifest protobuf length";
        return false;
    }
    const std::uint32_t proto_len = read_le32(len_le);
    if (proto_len == 0 || static_cast<std::uint64_t>(manifest_position) + 4ULL + static_cast<std::uint64_t>(proto_len) >
                             file_size - 16U) {
        error = "invalid manifest protobuf length";
        return false;
    }
    protobuf_body.resize(proto_len);
    if (!read_exact(in, protobuf_body.data(), proto_len)) {
        error = "failed to read manifest protobuf";
        return false;
    }
    return true;
}

bool load_latest_manifest(const std::filesystem::path& dataset_path, pb::Manifest& out, std::uint64_t& version_out,
                          std::string& error) {
    error.clear();
    out = pb::Manifest{};
    const std::uint64_t v = highest_manifest_version(dataset_path, error);
    if (!error.empty()) {
        return false;
    }
    if (v == 0) {
        error = "no manifest found under _versions";
        return false;
    }
    const auto manifest_path = dataset_path / "_versions" / (std::to_string(v) + ".manifest");
    std::vector<std::uint8_t> body;
    if (!read_manifest_file(manifest_path, body, error)) {
        return false;
    }
    if (!pb::decode_manifest(body, out)) {
        error = "failed to decode manifest protobuf";
        return false;
    }
    // Bound element counts as defense-in-depth against a hostile manifest that packs an enormous number
    // of (possibly tiny/empty) fields/fragments/files to amplify downstream allocations.
    const auto& limits = default_read_limits();
    if (out.fields.size() > limits.max_manifest_elements ||
        out.fragments.size() > limits.max_manifest_elements) {
        error = "manifest element count exceeds safety limit";
        return false;
    }
    for (const auto& fragment : out.fragments) {
        if (fragment.files.size() > limits.max_manifest_elements) {
            error = "manifest fragment file count exceeds safety limit";
            return false;
        }
    }
    version_out = v;
    return true;
}

}  // namespace nano_lance
