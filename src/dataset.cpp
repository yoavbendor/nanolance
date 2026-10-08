// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/dataset.hpp"
#include "nanolance/dataset_commit.hpp"

#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <set>
#include <thread>

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

bool same_file(const pb::DataFile& a, const pb::DataFile& b) {
    return a.path == b.path && a.fields == b.fields && a.column_indices == b.column_indices;
}

bool same_fragment(const pb::DataFragment& a, const pb::DataFragment& b) {
    if (a.id != b.id || a.physical_rows != b.physical_rows || a.files.size() != b.files.size() ||
        a.deletion_file.present != b.deletion_file.present ||
        (a.deletion_file.present && (a.deletion_file.id != b.deletion_file.id ||
                                     a.deletion_file.read_version != b.deletion_file.read_version))) {
        return false;
    }
    for (std::size_t i = 0; i < a.files.size(); ++i) {
        if (!same_file(a.files[i], b.files[i])) {
            return false;
        }
    }
    return true;
}

bool same_fields(const std::vector<pb::Field>& a, const std::vector<pb::Field>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].parent_id != b[i].parent_id || a[i].name != b[i].name ||
            a[i].logical_type != b[i].logical_type || a[i].nullable != b[i].nullable) {
            return false;
        }
    }
    return true;
}

std::set<std::array<std::uint8_t, 16>> index_uuids(const pb::Manifest& m) {
    std::set<std::array<std::uint8_t, 16>> out;
    for (const auto& i : m.indices) {
        out.insert(i.uuid);
    }
    return out;
}

/// Rebase `ours` -- a change made from `base` -- onto `latest`, which other writers committed in the
/// meantime, as Lance's conflict resolution does for compatible transactions: when they only added
/// fragments (every fragment of `base` unchanged, the same schema, indices and config), the change
/// reads as made after theirs. Their new fragments join ours, and fragments ours added are numbered
/// after theirs (with the indices that cover them). False: a real conflict.
bool rebase(const pb::Manifest& base, const pb::Manifest& latest, pb::Manifest& ours) {
    if (!same_fields(base.fields, latest.fields) || index_uuids(base) != index_uuids(latest) ||
        base.config != latest.config || base.table_metadata != latest.table_metadata ||
        base.schema_metadata != latest.schema_metadata ||
        base.reader_feature_flags != latest.reader_feature_flags) {
        return false;
    }
    std::map<std::uint64_t, const pb::DataFragment*> theirs;
    for (const auto& f : latest.fragments) {
        theirs[f.id] = &f;
    }
    std::set<std::uint64_t> in_base;
    for (const auto& f : base.fragments) {
        const auto it = theirs.find(f.id);
        if (it == theirs.end() || !same_fragment(f, *it->second)) {
            return false;  // they changed or removed a fragment this change was made from
        }
        in_base.insert(f.id);
    }
    std::uint64_t next = latest.has_max_fragment_id ? latest.max_fragment_id + 1ULL : 0ULL;
    for (const auto& f : latest.fragments) {
        next = std::max(next, f.id + 1U);
    }
    // Fragments this change added: numbered after every fragment the dataset has had.
    std::map<std::uint32_t, std::uint32_t> renumber;
    for (auto& f : ours.fragments) {
        if (in_base.count(f.id) == 0U) {
            renumber[static_cast<std::uint32_t>(f.id)] = static_cast<std::uint32_t>(next);
            f.id = next++;
        }
    }
    for (auto& index : ours.indices) {
        bool changed = false;
        for (auto& id : index.fragment_ids) {
            const auto it = renumber.find(id);
            if (it != renumber.end()) {
                id = it->second;
                changed = true;
            }
        }
        if (changed) {
            std::sort(index.fragment_ids.begin(), index.fragment_ids.end());
            index.fragment_bitmap_changed = true;
        }
    }
    for (const auto& f : latest.fragments) {
        if (in_base.count(f.id) == 0U) {
            ours.fragments.push_back(f);  // theirs
        }
    }
    std::stable_sort(ours.fragments.begin(), ours.fragments.end(),
                     [](const pb::DataFragment& a, const pb::DataFragment& b) { return a.id < b.id; });
    std::uint64_t max_id = latest.has_max_fragment_id ? latest.max_fragment_id : 0U;
    for (const auto& f : ours.fragments) {
        max_id = std::max(max_id, f.id);
    }
    ours.has_max_fragment_id = ours.has_max_fragment_id || latest.has_max_fragment_id || !ours.fragments.empty();
    ours.max_fragment_id = static_cast<std::uint32_t>(max_id);
    ours.next_row_id = std::max(ours.next_row_id, latest.next_row_id);
    if (std::any_of(ours.fragments.begin(), ours.fragments.end(),
                    [](const pb::DataFragment& f) { return f.deletion_file.present; })) {
        ours.reader_feature_flags |= pb::kFlagDeletionFiles;
        ours.writer_feature_flags |= pb::kFlagDeletionFiles;
    }
    ours.version = latest.version;
    return true;
}

bool is_conflict(const std::string& error) {
    return error.rfind("commit conflict", 0) == 0;
}

/// A short, growing, jittered pause between attempts, so racing writers spread out.
void back_off(int attempt) {
    thread_local std::mt19937 rng{std::random_device{}()};
    const int ceiling = std::min(50, 1 << std::min(attempt, 5));
    std::this_thread::sleep_for(std::chrono::milliseconds(std::uniform_int_distribution<int>(1, ceiling)(rng)));
}

constexpr int kCommitAttempts = 20;  // Lance's default conflict_retries

}  // namespace

/// Publish `manifest` as the version after the one it was read at, stamped now and by nanolance.
/// Committing as the version after the one it was READ at -- not after whatever is latest by now -- is
/// what keeps a concurrent change from being lost: if another writer took that version meanwhile, the
/// publish refuses (commit conflict) instead of stacking a stale manifest on top of it.
bool commit_next_version(const std::filesystem::path& dataset_path, pb::Manifest manifest, std::uint64_t& new_version,
                         std::string& error) {
    std::uint64_t read_version = manifest.version;
    if (read_version == 0U && !dataset_latest_version(dataset_path, read_version, error)) {
        return false;
    }
    manifest.version = read_version + 1U;
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
    if (publish_manifest(dataset_path, manifest, error)) {
        return true;
    }
    if (!is_conflict(error)) {
        return false;
    }
    // Another writer took the version. Made from `base`, the change may still apply after theirs.
    const std::string first_error = error;
    pb::Manifest base;
    if (!load_manifest_version(dataset_path, read_version, base, error)) {
        error = first_error;
        return false;
    }
    for (int attempt = 0; attempt < kCommitAttempts; ++attempt) {
        pb::Manifest latest;
        std::uint64_t latest_version = 0;
        error.clear();
        if (!load_latest_manifest(dataset_path, latest, latest_version, error)) {
            return false;
        }
        pb::Manifest rebased = manifest;
        if (!rebase(base, latest, rebased)) {
            error = first_error;
            return false;
        }
        rebased.version = latest_version + 1U;
        new_version = rebased.version;
        if (publish_manifest(dataset_path, rebased, error)) {
            return true;
        }
        if (!is_conflict(error)) {
            return false;
        }
        back_off(attempt);
    }
    return false;
}

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
    manifest.version = latest_version;  // committed on top of the latest, which it replaces
    manifest.operation = pb::Manifest::Operation::Restore;
    manifest.restored_version = version;
    return commit_next_version(dataset_path, std::move(manifest), new_version, error);
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
    return commit_next_version(dataset_path, std::move(manifest), new_version, error);
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
    return commit_next_version(dataset_path, std::move(manifest), new_version, error);
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
    return commit_next_version(dataset_path, std::move(manifest), new_version, error);
}

}  // namespace nano_lance
