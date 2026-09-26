// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/dataset.hpp"

#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <chrono>

namespace nano_lance {
namespace {

DatasetVersionInfo version_info(const pb::Manifest& manifest) {
    DatasetVersionInfo info;
    info.version = manifest.version;
    info.timestamp_ns = manifest.has_timestamp
                            ? manifest.timestamp_seconds * 1000000000LL + static_cast<std::int64_t>(manifest.timestamp_nanos)
                            : 0;
    info.tag = manifest.tag;
    info.writer_library = manifest.writer_library;
    info.writer_version = manifest.writer_version;
    return info;
}

bool load(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version, pb::Manifest& manifest,
          std::string& error) {
    if (has_version) {
        return load_manifest_version(dataset_path, version, manifest, error);
    }
    std::uint64_t latest = 0;
    return load_latest_manifest(dataset_path, manifest, latest, error);
}

/// Publish `manifest` as the next version, stamped now and by nanolance.
bool publish_next(const std::filesystem::path& dataset_path, pb::Manifest manifest, std::uint64_t& new_version,
                  std::string& error) {
    std::uint64_t latest = 0;
    if (!dataset_latest_version(dataset_path, latest, error)) {
        return false;
    }
    manifest.version = latest + 1U;
    manifest.has_timestamp = true;
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    manifest.timestamp_seconds = static_cast<std::int64_t>(nanos / 1000000000LL);
    manifest.timestamp_nanos = static_cast<std::int32_t>(nanos % 1000000000LL);
    manifest.writer_library = "nanolance";
    manifest.writer_version = nanolance_writer_version();
    manifest.transaction_file.clear();
    manifest.tag.clear();
    new_version = manifest.version;
    return publish_manifest(dataset_path, manifest, error);
}

}  // namespace

bool dataset_latest_version(const std::filesystem::path& dataset_path, std::uint64_t& out, std::string& error) {
    out = highest_manifest_version(dataset_path, error);
    if (!error.empty()) {
        return false;
    }
    if (out == 0U) {
        error = "Dataset at path " + dataset_path.string() + " was not found";
        return false;
    }
    return true;
}

bool dataset_info(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                  DatasetInfo& out, std::string& error) {
    out = DatasetInfo{};
    if (!dataset_latest_version(dataset_path, out.latest_version, error)) {
        return false;
    }
    pb::Manifest manifest;
    if (!load(dataset_path, has_version, version, manifest, error)) {
        return false;
    }
    out.version = version_info(manifest);
    if (out.version.version == 0U) {
        out.version.version = has_version ? version : out.latest_version;
    }
    out.data_storage_version = manifest.data_format.version;
    out.has_max_fragment_id = manifest.has_max_fragment_id;
    out.max_fragment_id = manifest.max_fragment_id;
    out.config = manifest.config;
    out.table_metadata = manifest.table_metadata;
    for (const auto& kv : manifest.schema_metadata) {
        out.schema_metadata[kv.first] = std::string(kv.second.begin(), kv.second.end());
    }
    out.reader_feature_flags = manifest.reader_feature_flags;
    out.writer_feature_flags = manifest.writer_feature_flags;
    for (const auto& fragment : manifest.fragments) {
        DatasetFragmentInfo info;
        info.id = fragment.id;
        info.physical_rows = fragment.physical_rows;
        if (fragment.deletion_file.present) {
            if (fragment.deletion_file.num_deleted_rows > fragment.physical_rows) {
                error = "fragment claims more deleted rows than it holds";
                return false;
            }
            info.deleted_rows = fragment.deletion_file.num_deleted_rows;
            info.has_deletion_file = true;
            info.deletion_file = "_deletions/" + std::to_string(fragment.id) + "-" +
                                 std::to_string(fragment.deletion_file.read_version) + "-" +
                                 std::to_string(fragment.deletion_file.id) +
                                 (fragment.deletion_file.file_type == 1U ? ".bin" : ".arrow");
        }
        for (const auto& file : fragment.files) {
            DatasetFile f;
            f.path = file.path;
            f.fields = file.fields;
            f.major_version = file.file_major_version;
            f.minor_version = file.file_minor_version;
            f.size_bytes = file.file_size_bytes;
            info.files.push_back(std::move(f));
        }
        out.fragments.push_back(std::move(info));
    }
    return true;
}

bool dataset_versions(const std::filesystem::path& dataset_path, std::vector<DatasetVersionInfo>& out,
                      std::string& error) {
    out.clear();
    const auto versions = list_manifest_versions(dataset_path, error);
    if (!error.empty()) {
        return false;
    }
    if (versions.empty()) {
        error = "Dataset at path " + dataset_path.string() + " was not found";
        return false;
    }
    for (const auto v : versions) {
        pb::Manifest manifest;
        if (!load_manifest_version(dataset_path, v, manifest, error)) {
            return false;
        }
        auto info = version_info(manifest);
        info.version = v;
        out.push_back(std::move(info));
    }
    return true;
}

bool dataset_restore(const std::filesystem::path& dataset_path, std::uint64_t version, std::uint64_t& new_version,
                     std::string& error) {
    pb::Manifest manifest;
    if (!load_manifest_version(dataset_path, version, manifest, error)) {
        return false;
    }
    // max_fragment_id only ever grows: a fragment id used by any version is never reused.
    pb::Manifest latest;
    std::uint64_t latest_version = 0;
    if (!load_latest_manifest(dataset_path, latest, latest_version, error)) {
        return false;
    }
    if (latest.has_max_fragment_id) {
        manifest.has_max_fragment_id = true;
        manifest.max_fragment_id = std::max(manifest.max_fragment_id, latest.max_fragment_id);
    }
    return publish_next(dataset_path, std::move(manifest), new_version, error);
}

bool dataset_update_config(const std::filesystem::path& dataset_path,
                           const std::map<std::string, std::string>& upsert, const std::vector<std::string>& remove,
                           std::uint64_t& new_version, std::string& error) {
    pb::Manifest manifest;
    std::uint64_t latest = 0;
    if (!load_latest_manifest(dataset_path, manifest, latest, error)) {
        return false;
    }
    for (const auto& kv : upsert) {
        manifest.config[kv.first] = kv.second;
    }
    for (const auto& key : remove) {
        manifest.config.erase(key);
    }
    return publish_next(dataset_path, std::move(manifest), new_version, error);
}

bool dataset_update_table_metadata(const std::filesystem::path& dataset_path,
                                   const std::map<std::string, std::string>& values, bool replace,
                                   std::uint64_t& new_version, std::string& error) {
    pb::Manifest manifest;
    std::uint64_t latest = 0;
    if (!load_latest_manifest(dataset_path, manifest, latest, error)) {
        return false;
    }
    if (replace) {
        manifest.table_metadata.clear();
    }
    for (const auto& kv : values) {
        manifest.table_metadata[kv.first] = kv.second;
    }
    return publish_next(dataset_path, std::move(manifest), new_version, error);
}

bool dataset_update_schema_metadata(const std::filesystem::path& dataset_path,
                                    const std::map<std::string, std::string>& values, bool replace,
                                    std::uint64_t& new_version, std::string& error) {
    pb::Manifest manifest;
    std::uint64_t latest = 0;
    if (!load_latest_manifest(dataset_path, manifest, latest, error)) {
        return false;
    }
    if (replace) {
        manifest.schema_metadata.clear();
    }
    for (const auto& kv : values) {
        manifest.schema_metadata[kv.first] = std::vector<std::uint8_t>(kv.second.begin(), kv.second.end());
    }
    return publish_next(dataset_path, std::move(manifest), new_version, error);
}

}  // namespace nano_lance
