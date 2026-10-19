// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/byte_source.hpp"
#include "nanolance/work_stats.hpp"
#include "nanolance/manifest_reader.hpp"

#include "nanolance/read_safety.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <sstream>
#include <limits>
#include <iomanip>

namespace nano_lance {
namespace {

/// `n` bytes at `offset` of a manifest source, counted as a read.
bool read_range(io::ByteSource& src, std::uint64_t offset, std::uint64_t n, void* dest, std::string& error) {
    work_stats::count_read(n);
    return src.read_at(offset, n, static_cast<std::uint8_t*>(dest), error);
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

constexpr std::size_t kManifestV2Digits = 20U;

/// The IndexSection message at `position` of a manifest: [u32 length][protobuf], as Lance's
/// write_protobuf lays every message out.
bool read_index_section(io::ByteSource& src, std::uint64_t position, std::vector<pb::IndexMetadata>& out,
                        std::string& error) {
    const std::uint64_t file_size = src.size();
    unsigned char len_le[4]{};
    if (position > file_size || file_size - position < 4U) {
        error = "manifest index section is past the end of the file";
        return false;
    }
    if (!read_range(src, position, 4, len_le, error)) {
        error = "failed to read the manifest index section";
        return false;
    }
    const std::uint32_t length = read_le32(len_le);
    if (length > file_size - position - 4U) {
        error = "manifest index section runs past the end of the file";
        return false;
    }
    std::vector<std::uint8_t> body(length);
    if (length != 0U && !read_range(src, position + 4U, length, body.data(), error)) {
        error = "failed to read the manifest index section";
        return false;
    }
    if (!pb::decode_index_section(body, out, error)) {
        error = "manifest index section: " + error;
        return false;
    }
    return true;
}
}  // namespace

bool parse_manifest_version(const std::string& name, std::uint64_t& version_out) {
    constexpr std::string_view kSuffix = ".manifest";
    if (name.size() <= kSuffix.size() ||
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return false;
    }
    const auto digits = name.substr(0, name.size() - kSuffix.size());
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
        return false;  // detached (`d123.manifest`) or simply not a manifest of ours
    }
    std::uint64_t parsed = 0;
    try {
        parsed = std::stoull(digits);
    } catch (...) {
        return false;
    }
    // V2 is identified by the numeric part being exactly 20 characters, which is what Lance keys on.
    version_out = digits.size() == kManifestV2Digits
                      ? std::numeric_limits<std::uint64_t>::max() - parsed
                      : parsed;
    return true;
}



std::uint64_t highest_manifest_version(const std::filesystem::path& dataset_path, std::string& error) {
    error.clear();
    std::vector<io::DirEntry> entries;
    if (!io::list_dir((dataset_path / "_versions").string(), entries, error)) {
        return 0;
    }
    std::uint64_t max_version = 0;
    for (const auto& entry : entries) {
        std::uint64_t version = 0;
        if (!entry.is_dir && parse_manifest_version(entry.name, version)) {
            max_version = std::max(max_version, version);
        }
    }
    return max_version;
}

namespace {

/// The protobuf of the manifest in `src`: the trailer says where it starts.
bool read_manifest_body(io::ByteSource& src, std::vector<std::uint8_t>& protobuf_body, std::string& error) {
    const std::uint64_t file_size = src.size();
    if (file_size < 8U + 16U) {
        error = "manifest file too small";
        return false;
    }
    unsigned char trailer[16]{};
    if (!read_range(src, file_size - 16U, 16, trailer, error)) {
        error = "failed to read manifest trailer";
        return false;
    }
    if (std::memcmp(trailer + 12, "LANC", 4) != 0) {
        error = "manifest trailer magic is not LANC";
        return false;
    }
    // The trailer's version (bytes 8..12) is the writer's, not a layout: Lance reads every manifest the
    // same way -- [u32 length][protobuf] at the trailer's position -- whatever it says. LanceDB
    // tables written by older Lance say 0.1 (newer ones 0.2).
    const std::uint64_t manifest_position = read_le64(trailer);
    if (manifest_position > file_size - 16U) {
        error = "invalid manifest_position in trailer";
        return false;
    }
    unsigned char len_le[4]{};
    if (!read_range(src, manifest_position, 4, len_le, error)) {
        error = "failed to read manifest protobuf length";
        return false;
    }
    const std::uint32_t proto_len = read_le32(len_le);
    if (proto_len == 0 || manifest_position + 4ULL + static_cast<std::uint64_t>(proto_len) > file_size - 16U) {
        error = "invalid manifest protobuf length";
        return false;
    }
    protobuf_body.resize(proto_len);
    if (!read_range(src, manifest_position + 4U, proto_len, protobuf_body.data(), error)) {
        error = "failed to read manifest protobuf";
        return false;
    }
    return true;
}

}  // namespace

bool read_manifest_file(const std::filesystem::path& manifest_path, std::vector<std::uint8_t>& protobuf_body,
                        std::string& error) {
    error.clear();
    protobuf_body.clear();
    const auto source = io::open_source(manifest_path.string(), error);
    if (!source) {
        error = "failed to open manifest file";
        return false;
    }
    return read_manifest_body(*source, protobuf_body, error);
}

std::vector<std::uint64_t> list_manifest_versions(const std::filesystem::path& dataset_path, std::string& error) {
    error.clear();
    std::vector<std::uint64_t> versions;
    std::vector<io::DirEntry> entries;
    if (!io::list_dir((dataset_path / "_versions").string(), entries, error)) {
        return {};
    }
    for (const auto& entry : entries) {
        std::uint64_t version = 0;
        if (!entry.is_dir && parse_manifest_version(entry.name, version)) {
            versions.push_back(version);
        }
    }
    std::sort(versions.begin(), versions.end());
    versions.erase(std::unique(versions.begin(), versions.end()), versions.end());
    return versions;
}

namespace {

/// Bit 8 must be set in both words or in neither; otherwise the manifest is corrupt (Lance checks this
/// before the read and the write checks).
bool check_paired_flags(const pb::Manifest& manifest, std::string& error) {
    const bool reader_bit = (manifest.reader_feature_flags & pb::kFlagMixedDataFileVersions) != 0U;
    const bool writer_bit = (manifest.writer_feature_flags & pb::kFlagMixedDataFileVersions) != 0U;
    if (reader_bit != writer_bit) {
        error = "Encountered corrupt file manifest: Manifest has only one of the mixed data-file-version reader "
                "and writer feature bits set, so its semantics are undefined";
        return false;
    }
    return true;
}

/// The manifest in `src`: its protobuf, and the indices a commit must carry on (see
/// index_maintenance.hpp). `check_flags` refuses what this build cannot read, as Lance does at open.
bool load_manifest_source(io::ByteSource& src, pb::Manifest& out, std::string& error, bool check_flags) {
    error.clear();
    out = pb::Manifest{};
    std::vector<std::uint8_t> body;
    if (!read_manifest_body(src, body, error)) {
        return false;
    }
    if (!pb::decode_manifest(body, out)) {
        error = "failed to decode manifest protobuf";
        return false;
    }
    if (check_flags && !check_manifest_readable(out, error)) {
        out = pb::Manifest{};
        return false;
    }
    // A reader does not need the indices, so one it cannot parse fails only a later commit, which
    // would otherwise drop them.
    if (out.has_index_section) {
        std::string index_error;
        if (!read_index_section(src, out.index_section, out.indices, index_error)) {
            out.indices.clear();
            out.index_section_error = index_error;
        }
    }
    // Bound element counts as defense-in-depth against a hostile manifest that packs an enormous number
    // of (possibly tiny/empty) fields/fragments/files to amplify downstream allocations.
    const auto& limits = default_read_limits();
    if (out.fields.size() > limits.max_manifest_elements ||
        out.fragments.size() > limits.max_manifest_elements || out.indices.size() > limits.max_manifest_elements) {
        error = "manifest element count exceeds safety limit";
        return false;
    }
    for (const auto& fragment : out.fragments) {
        if (fragment.files.size() > limits.max_manifest_elements) {
            error = "manifest fragment file count exceeds safety limit";
            return false;
        }
    }
    return true;
}

}  // namespace

bool load_manifest_file(const std::filesystem::path& manifest_path, pb::Manifest& out, std::string& error) {
    const auto source = io::open_source(manifest_path.string(), error);
    if (!source) {
        error = "failed to open manifest file";
        out = pb::Manifest{};
        return false;
    }
    return load_manifest_source(*source, out, error, true);
}

bool load_manifest_file_unchecked(const std::filesystem::path& manifest_path, pb::Manifest& out,
                                  std::string& error) {
    const auto source = io::open_source(manifest_path.string(), error);
    if (!source) {
        error = "failed to open manifest file";
        out = pb::Manifest{};
        return false;
    }
    return load_manifest_source(*source, out, error, false);
}

bool check_manifest_readable(const pb::Manifest& manifest, std::string& error) {
    if (!check_paired_flags(manifest, error)) {
        return false;
    }
    if ((manifest.reader_feature_flags & ~pb::kSupportedFeatureFlags) != 0U) {
        error = "Not supported: This dataset cannot be read by this version of Lance. Please upgrade Lance to "
                "read this dataset. Flags: " + std::to_string(manifest.reader_feature_flags);
        return false;
    }
    return true;
}

bool check_manifest_writable(const pb::Manifest& manifest, WriteKind kind, std::string& error) {
    if (!check_paired_flags(manifest, error)) {
        return false;
    }
    if ((manifest.writer_feature_flags & ~pb::kSupportedFeatureFlags) != 0U) {
        const std::string flags = std::to_string(manifest.writer_feature_flags);
        error = kind == WriteKind::Insert
                    ? "Not supported: This dataset cannot be written by this version of Lance. Please upgrade Lance "
                      "to write to this dataset.\n Flags: " + flags
                    : "Not supported: This dataset cannot be written by this version of Lance. Please upgrade Lance "
                      "to write this dataset. Flags: " + flags;
        return false;
    }
    return true;
}

bool load_manifest_version(const std::filesystem::path& dataset_path, std::uint64_t v, pb::Manifest& out,
                           std::string& error) {
    error.clear();
    out = pb::Manifest{};
    // The version is scheme-independent, so try both spellings rather than assuming one. A dataset
    // nanolance appended to after pylance created it can legitimately hold a mix. Each candidate is
    // opened once: a remote one costs a round trip per open.
    std::vector<std::filesystem::path> candidates;
    if ((v & (1ULL << 63U)) != 0U) {  // a detached version: `d<version>.manifest`
        candidates.push_back(dataset_path / "_versions" / ("d" + std::to_string(v) + ".manifest"));
    } else {
        candidates.push_back(dataset_path / "_versions" / (std::to_string(v) + ".manifest"));
        std::ostringstream inverted;
        inverted << std::setfill('0') << std::setw(static_cast<int>(kManifestV2Digits))
                 << (std::numeric_limits<std::uint64_t>::max() - v);
        candidates.push_back(dataset_path / "_versions" / (inverted.str() + ".manifest"));
    }
    for (const auto& candidate : candidates) {
        std::string open_error;
        const auto source = io::open_source(candidate.string(), open_error);
        if (!source) {
            if (io::exists(candidate.string())) {
                error = "failed to open manifest file";
                return false;
            }
            continue;
        }
        return load_manifest_source(*source, out, error, true);
    }
    error = "version " + std::to_string(v) + " not found";
    return false;
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
    if (!load_manifest_version(dataset_path, v, out, error)) {
        return false;
    }
    version_out = v;
    return true;
}

}  // namespace nano_lance
