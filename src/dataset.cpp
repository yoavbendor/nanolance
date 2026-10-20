// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/dataset.hpp"
#include "nanolance/base_paths.hpp"
#include "nanolance/dataset_commit.hpp"

#include "nanolance/manifest_reader.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/deletion_vector.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/path_safety.hpp"

#include "lance_minimal.pb.hpp"

#include "commit_conflicts.hpp"
#include "dataset_operation.hpp"
#include "dataset_transaction_ops.hpp"
#include "transaction_file.hpp"

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
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
    info.transaction_file = manifest.transaction_file;
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

std::atomic<std::uint64_t> g_rebased{0};
std::atomic<std::uint64_t> g_retried{0};
std::atomic<std::uint64_t> g_refused{0};

enum class Outcome { Compatible, Refused, Failed };

/// "commit conflict: ..." for a retryable verdict: the operation runs again on the newer version.
std::string retryable_error(std::uint64_t version, const std::string& ours, const std::string& theirs) {
    return "commit conflict: Retryable commit conflict for version " + std::to_string(version) + ": This " + ours +
           " transaction was preempted by concurrent transaction " + theirs + " at version " +
           std::to_string(version) + ". Please retry.";
}

/// The whole of a transaction file: `_transactions/{name}`. False when it cannot be read.
bool read_transaction_file(const std::filesystem::path& dataset_path, const std::string& name, OpBytes& bytes) {
    std::ifstream in(dataset_path / "_transactions" / name, std::ios::binary);
    if (!in) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

/// The bytes of the transaction that made `version`, as Lance reads it: its transaction file when that
/// is named and readable, else the inline section of its manifest (field 21, which pylance 12 writes
/// beside the file). False with `reason` when neither is there.
bool version_transaction_bytes(const std::filesystem::path& dataset_path, std::uint64_t version,
                               const pb::Manifest& manifest, OpBytes& bytes, std::string& reason) {
    if (!manifest.transaction_file.empty() && read_transaction_file(dataset_path, manifest.transaction_file, bytes)) {
        return true;
    }
    if (manifest.has_transaction_section) {
        std::string error;
        if (read_inline_transaction(dataset_path, version, manifest, bytes, error)) {
            return true;
        }
        reason = "an unreadable transaction section";
        return false;
    }
    reason = manifest.transaction_file.empty() ? "no transaction file" : "an unreadable transaction file";
    return false;
}

/// The operation a version committed, from its transaction (see version_transaction_bytes). False with
/// `reason` when it has none, or it cannot be read or decoded.
bool concurrent_operation(const std::filesystem::path& dataset_path, std::uint64_t version,
                          const pb::Manifest& manifest, Operation& op, std::string& reason) {
    OpBytes bytes;
    if (!version_transaction_bytes(dataset_path, version, manifest, bytes, reason)) {
        return false;
    }
    std::uint32_t field = 0;
    OpBytes message;
    std::string why;
    if (!transaction_file_operation(bytes, field, message, why)) {
        reason = "an unreadable transaction file";
        return false;
    }
    if (field == kCloneField) {
        op.kind = kCloneField;  // compatible with everything: no decoder needed
        return true;
    }
    if (!decode_operation(field, message, op, why)) {
        reason = "an operation nanolance cannot read";
        return false;
    }
    return true;
}

bool same_deletion_file(const pb::DeletionFile& a, const pb::DeletionFile& b) {
    return a.present == b.present && a.file_type == b.file_type && a.read_version == b.read_version && a.id == b.id &&
           a.num_deleted_rows == b.num_deleted_rows && a.unknown == b.unknown;
}

/// Lance's finish_delete_update: a Delete or Update rebased over a concurrent change to the deletion file of a
/// fragment it deletes from. The rows it deleted (`affected`) must not be deleted already in the latest
/// version (retryable, Lance's message); its deletion file becomes the latest's plus its rows, and a fragment
/// left with no rows is removed. Fragments whose deletion file no one changed keep the change's own.
bool merge_deletion_vectors(const std::filesystem::path& dataset_path, const pb::Manifest& base,
                            const pb::Manifest& latest, std::uint64_t latest_version, const AffectedRows& affected,
                            const std::string& theirs_name, Operation& mine, std::string& error) {
    const std::string ours_name = operation_name(mine.kind);
    // Lance's check for one fragment this change deletes from: a deletion file the other change changed, and
    // rows of its own among the rows already deleted there, conflict. `merged` gets the union of the two.
    const auto merge_fragment = [&](std::uint64_t id, std::vector<std::uint32_t>& merged) -> bool {
        const auto was = std::find_if(base.fragments.begin(), base.fragments.end(),
                                      [&](const pb::DataFragment& f) { return f.id == id; });
        const auto now = std::find_if(latest.fragments.begin(), latest.fragments.end(),
                                      [&](const pb::DataFragment& f) { return f.id == id; });
        if (was == base.fragments.end() || now == latest.fragments.end()) {
            error = retryable_error(latest_version, ours_name, theirs_name);
            return false;
        }
        merged.clear();
        if (same_deletion_file(was->deletion_file, now->deletion_file)) {
            return true;  // nothing changed here since the base: the change's own deletion file stands
        }
        std::vector<std::uint32_t> existing;
        if (!read_manifest_deletion_vector(dataset_path, latest, id, now->deletion_file, existing, error)) {
            return false;
        }
        std::vector<std::uint32_t> ours_rows;
        if (const auto rows = affected.find(id); rows != affected.end()) {
            ours_rows = rows->second;
        }
        std::sort(ours_rows.begin(), ours_rows.end());
        ours_rows.erase(std::unique(ours_rows.begin(), ours_rows.end()), ours_rows.end());
        std::vector<std::uint32_t> both;
        std::set_intersection(existing.begin(), existing.end(), ours_rows.begin(), ours_rows.end(),
                              std::back_inserter(both));
        if (!both.empty()) {
            std::string addresses;
            for (std::size_t i = 0; i < both.size() && i < 5; ++i) {
                addresses += (i == 0 ? "" : ", ") + std::to_string((id << 32U) | both[i]);
            }
            error = "commit conflict: This " + ours_name + " transaction was preempted by concurrent transaction " +
                    theirs_name + " at version " + std::to_string(latest_version) + " (both modified rows at addresses [" +
                    addresses + "]). Please retry.";
            return false;
        }
        std::set_union(existing.begin(), existing.end(), ours_rows.begin(), ours_rows.end(), std::back_inserter(merged));
        return true;
    };
    for (const auto id : mine.removed_ids) {
        std::vector<std::uint32_t> merged;
        if (!merge_fragment(id, merged)) {
            return false;  // a fragment removed here, deleted from by the other change: its rows overlap
        }
    }
    for (auto it = mine.updated_fragments.begin(); it != mine.updated_fragments.end();) {
        std::vector<std::uint32_t> merged;
        if (!merge_fragment(it->id, merged)) {
            return false;
        }
        if (merged.empty()) {
            ++it;  // (an empty union: the fragment's deletion file is the change's own)
            continue;
        }
        const auto now = std::find_if(latest.fragments.begin(), latest.fragments.end(),
                                      [&](const pb::DataFragment& f) { return f.id == it->id; });
        if (merged.size() >= now->physical_rows) {
            mine.removed_ids.push_back(it->id);  // every row deleted: the fragment goes
            it = mine.updated_fragments.erase(it);
            continue;
        }
        if (!write_deletion_file(dataset_path, it->id, latest_version, merged, it->deletion_file, error)) {
            return false;
        }
        ++it;
    }
    return true;
}

/// Lance's verdict on `ours`, the change made from `base` (read at `read_version`), against each version
/// committed after it up to `latest`, in order; the first verdict that is not compatible decides.
Outcome classify_concurrent(const std::filesystem::path& dataset_path, const pb::Manifest& base,
                            const pb::Manifest& ours, std::uint64_t read_version, std::uint64_t latest,
                            const AffectedRows* affected, std::string& error, Operation& mine,
                            std::vector<Operation>& theirs_ops) {
    const auto [ours_field, ours_message] = derive_transaction_operation(base, ours);
    std::string why;
    if (!decode_operation(ours_field, ours_message, mine, why)) {
        error = retryable_error(read_version + 1U, operation_name(ours_field), "an operation");
        return Outcome::Refused;
    }
    const std::string ours_name = operation_name(mine.kind);
    for (std::uint64_t version = read_version + 1U; version <= latest; ++version) {
        pb::Manifest committed;
        if (!load_manifest_version(dataset_path, version, committed, error)) {
            return Outcome::Failed;
        }
        Operation theirs;
        std::string reason;
        if (!concurrent_operation(dataset_path, version, committed, theirs, reason)) {
            error = retryable_error(version, ours_name, "an operation (" + reason + ")");
            return Outcome::Refused;
        }
        const std::string theirs_name = theirs.kind == kCloneField ? "Clone" : operation_name(theirs.kind);
        switch (affected != nullptr ? check_txn_with_affected_rows(mine, theirs, base) : check_txn(mine, theirs)) {
            case Verdict::Compatible:
                break;
            case Verdict::Retryable:
                error = retryable_error(version, ours_name, theirs_name);
                return Outcome::Refused;
            case Verdict::Incompatible:
                error = "Incompatible transaction: This " + ours_name + " transaction is incompatible with concurrent "
                        "transaction " + theirs_name + " at version " + std::to_string(version) + ".";
                return Outcome::Refused;
        }
        theirs_ops.push_back(std::move(theirs));
    }
    return Outcome::Compatible;
}

}  // namespace

CommitConflictStats commit_conflict_stats() {
    CommitConflictStats out;
    out.rebased = g_rebased.load(std::memory_order_relaxed);
    out.retried = g_retried.load(std::memory_order_relaxed);
    out.refused = g_refused.load(std::memory_order_relaxed);
    return out;
}

void reset_commit_conflict_stats() {
    g_rebased.store(0, std::memory_order_relaxed);
    g_retried.store(0, std::memory_order_relaxed);
    g_refused.store(0, std::memory_order_relaxed);
}

void count_commit_retry() {
    g_retried.fetch_add(1, std::memory_order_relaxed);
}

/// Publish `manifest` as the version after the one it was read at, stamped now and by nanolance.
/// Committing as the version after the one it was READ at -- not after whatever is latest by now -- is
/// what keeps a concurrent change from being lost: if another writer took that version meanwhile, the
/// publish refuses (commit conflict) instead of stacking a stale manifest on top of it.
bool commit_next_version(const std::filesystem::path& dataset_path, pb::Manifest manifest, std::uint64_t& new_version,
                         std::string& error, const AffectedRows* affected) {
    std::uint64_t read_version = manifest.version;
    if (read_version == 0U && !dataset_latest_version(dataset_path, read_version, error)) {
        return false;
    }
    // Refused before anything is written. The message names the base's writer word, as Lance does: this
    // commit may have set a known bit of its own (a deletion file) on the copy it publishes.
    if (!check_manifest_writable(manifest, WriteKind::Commit, error)) {
        const std::string own_error = error;
        pb::Manifest base;
        std::string base_error;
        if (!load_manifest_version(dataset_path, read_version, base, base_error)) {
            error = base_error;
            return false;
        }
        if (!check_manifest_writable(base, WriteKind::Commit, error)) {
            return false;
        }
        error = own_error;
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
        if (!check_manifest_writable(latest, WriteKind::Commit, error)) {
            return false;
        }
        // Lance's verdict on every version committed since: a refusal (incompatible, or retryable with the
        // operation run again) is the error; when all are compatible the change is rebased onto the latest.
        Operation mine;
        std::vector<Operation> theirs;
        if (classify_concurrent(dataset_path, base, manifest, read_version, latest_version, affected, error, mine,
                                theirs) != Outcome::Compatible) {
            if (error.rfind("Incompatible transaction:", 0) == 0) {
                g_refused.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        if (affected != nullptr && (mine.kind == kDelete || mine.kind == kUpdate)) {
            // The concurrent change the merge reports: the last one, as Lance names it.
            std::string theirs_name;
            if (!theirs.empty()) {
                theirs_name = theirs.back().kind == kCloneField ? "Clone" : operation_name(theirs.back().kind);
            }
            if (!merge_deletion_vectors(dataset_path, base, latest, latest_version, *affected, theirs_name, mine,
                                        error)) {
                return false;
            }
        }
        pb::Manifest rebased;
        if (!rebase_operation(latest, manifest, mine, theirs, latest_version + 1U, rebased, error)) {
            return false;  // a conflict the operation runs again for (retry_on_conflict), or not a conflict
        }
        // A merged deletion file is a deletion file of the version: its flags, as apply_deletions sets them.
        const bool deletions = std::any_of(rebased.fragments.begin(), rebased.fragments.end(),
                                           [](const pb::DataFragment& f) { return f.deletion_file.present; });
        if (deletions) {
            rebased.reader_feature_flags |= pb::kFlagDeletionFiles;
            rebased.writer_feature_flags |= pb::kFlagDeletionFiles;
        }
        // The change keeps its own stamp and its own operation tag, which the transaction file is derived from.
        rebased.version = latest_version + 1U;
        rebased.has_timestamp = manifest.has_timestamp;
        rebased.timestamp_seconds = manifest.timestamp_seconds;
        rebased.timestamp_nanos = manifest.timestamp_nanos;
        rebased.writer_library = manifest.writer_library;
        rebased.writer_version = manifest.writer_version;
        rebased.operation = manifest.operation;
        rebased.restored_version = manifest.restored_version;
        rebased.reserved_fragments = manifest.reserved_fragments;
        rebased.first_new_fragment = mine.kind == kAppend ? latest.fragments.size() : manifest.first_new_fragment;
        rebased.transaction_file.clear();
        rebased.tag.clear();
        new_version = rebased.version;
        if (publish_manifest(dataset_path, rebased, error)) {
            g_rebased.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (!is_conflict(error)) {
            return false;
        }
        back_off(attempt);
    }
    return false;
}

bool load_read_version(const std::filesystem::path& dataset_path, ReadVersion read_version, pb::Manifest& manifest,
                       std::uint64_t& version, std::string& error) {
    if (!read_version) {
        return load_latest_manifest(dataset_path, manifest, version, error);
    }
    version = *read_version;
    if (load_manifest_version(dataset_path, version, manifest, error)) {
        return true;
    }
    // Cleaned up since the handle was opened (auto cleanup removes old versions): the change is made from
    // the latest, as it was before read versions, instead of failing on a version that no longer exists.
    if (error.find("not found") == std::string::npos) {
        return false;
    }
    return load_latest_manifest(dataset_path, manifest, version, error);
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

bool dataset_transaction(const std::filesystem::path& dataset_path, std::uint64_t version, OpBytes& bytes,
                         bool& found, std::string& error) {
    found = false;
    bytes.clear();
    pb::Manifest manifest;
    if (!load_manifest_version(dataset_path, version, manifest, error)) {
        return false;
    }
    // A named transaction file that is gone counts as none, as it always has; an inline section is read.
    if (!manifest.transaction_file.empty() && read_transaction_file(dataset_path, manifest.transaction_file, bytes)) {
        found = true;
        return true;
    }
    if (!manifest.has_transaction_section) {
        return true;
    }
    if (!read_inline_transaction(dataset_path, version, manifest, bytes, error)) {
        return false;
    }
    found = true;
    return true;
}

namespace {

/// Lance's must_recalculate_fragment_bitmap: an index whose coverage is unknown, or that an older Lance
/// (before 0.8.15) wrote, is rebuilt by the next commit.
bool bitmap_needs_recalculation(const pb::IndexMetadata& index, const pb::Manifest& manifest) {
    if (!index.has_fragment_bitmap) {
        return true;
    }
    if (manifest.writer_library != "lance") {
        return false;
    }
    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;
    if (std::sscanf(manifest.writer_version.c_str(), "%u.%u.%u", &major, &minor, &patch) != 3) {
        return true;
    }
    return major == 0 && (minor < 8 || (minor == 8 && patch < 15));
}

}  // namespace

bool dataset_needs_migration(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                             bool& out, std::string& error) {
    pb::Manifest manifest;
    if (!load(dataset_path, has_version, version, manifest, error)) {
        return false;
    }
    // Lance's manifest_needs_migration. Not checked: physical_rows and deletion num_deleted_rows being
    // absent; the reader does not keep their presence, and every Lance writes them.
    out = manifest.writer_library.empty() ||
          std::any_of(manifest.indices.begin(), manifest.indices.end(),
                      [&](const pb::IndexMetadata& index) { return bitmap_needs_recalculation(index, manifest); });
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
    if (!read_base_paths(manifest, out.base_paths, error) || !read_manifest_branch(manifest, out.branch, error)) {
        return false;
    }
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
            if (!data_file_base_id(file, f.base_id, error)) {
                return false;
            }
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

namespace {

bool restore_once(const std::filesystem::path& dataset_path, std::uint64_t version, std::uint64_t& new_version,
                  std::string& error, ReadVersion read_version) {
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
    // The restore is made from the version the caller read (latest unless given): the other writers'
    // commits since are checked against it, as for any other change.
    pb::Manifest base;
    std::uint64_t base_version = 0;
    if (!load_read_version(dataset_path, read_version, base, base_version, error)) {
        return false;
    }
    if (latest.has_max_fragment_id) {
        manifest.has_max_fragment_id = true;
        manifest.max_fragment_id = std::max(manifest.max_fragment_id, latest.max_fragment_id);
    }
    // Row ids are a high-water mark too: rewinding would hand ids already used to new rows. And stable
    // row ids cannot be turned off again, or row addresses would collide with the ids handed out.
    if ((latest.reader_feature_flags & pb::kFlagStableRowIds) != 0U &&
        (manifest.reader_feature_flags & pb::kFlagStableRowIds) == 0U) {
        error = "Cannot restore version " + std::to_string(version) +
                ": stable row ids were enabled after it, and turning them back off would let row addresses "
                "collide with ids this table has already used";
        return false;
    }
    manifest.next_row_id = std::max(manifest.next_row_id, latest.next_row_id);
    manifest.version = base_version;  // committed on top of the version it was made from
    manifest.operation = pb::Manifest::Operation::Restore;
    manifest.restored_version = version;
    return commit_next_version(dataset_path, std::move(manifest), new_version, error);
}

}  // namespace

bool dataset_restore(const std::filesystem::path& dataset_path, std::uint64_t version, std::uint64_t& new_version,
                     std::string& error, ReadVersion read_version) {
    // Made from the version the caller read; a retryable conflict restores again from the latest.
    return retry_from_read_version(
        read_version,
        [&](ReadVersion from) { return restore_once(dataset_path, version, new_version, error, from); }, error);
}

bool dataset_update_config(const std::filesystem::path& dataset_path,
                           const std::map<std::string, std::string>& upsert, const std::vector<std::string>& remove,
                           std::uint64_t& new_version, std::string& error, ReadVersion read_version) {
    // Re-run from the latest after a retryable conflict, as Lance's retries do.
    ReadVersion from = read_version;
    return retry_on_conflict(
        [&] {
            pb::Manifest manifest;
            std::uint64_t version = 0;
            if (!load_read_version(dataset_path, std::exchange(from, std::nullopt), manifest, version, error)) {
                return false;
            }
            for (const auto& kv : upsert) {
                manifest.config[kv.first] = kv.second;
            }
            for (const auto& key : remove) {
                manifest.config.erase(key);
            }
            return commit_next_version(dataset_path, std::move(manifest), new_version, error);
        },
        error);
}

bool dataset_update_table_metadata(const std::filesystem::path& dataset_path,
                                   const std::map<std::string, std::string>& values, bool replace,
                                   std::uint64_t& new_version, std::string& error, ReadVersion read_version) {
    ReadVersion from = read_version;
    return retry_on_conflict(
        [&] {
            pb::Manifest manifest;
            std::uint64_t version = 0;
            if (!load_read_version(dataset_path, std::exchange(from, std::nullopt), manifest, version, error)) {
                return false;
            }
            if (replace) {
                manifest.table_metadata.clear();
            }
            for (const auto& kv : values) {
                manifest.table_metadata[kv.first] = kv.second;
            }
            return commit_next_version(dataset_path, std::move(manifest), new_version, error);
        },
        error);
}

bool dataset_update_schema_metadata(const std::filesystem::path& dataset_path,
                                    const std::map<std::string, std::string>& values, bool replace,
                                    std::uint64_t& new_version, std::string& error, ReadVersion read_version) {
    ReadVersion from = read_version;
    return retry_on_conflict(
        [&] {
            pb::Manifest manifest;
            std::uint64_t version = 0;
            if (!load_read_version(dataset_path, std::exchange(from, std::nullopt), manifest, version, error)) {
                return false;
            }
            if (replace) {
                manifest.schema_metadata.clear();
            }
            for (const auto& kv : values) {
                manifest.schema_metadata[kv.first] = std::vector<std::uint8_t>(kv.second.begin(), kv.second.end());
            }
            return commit_next_version(dataset_path, std::move(manifest), new_version, error);
        },
        error);
}

bool dataset_fields(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                    std::vector<DatasetField>& out, std::string& error) {
    out.clear();
    pb::Manifest manifest;
    if (!load(dataset_path, has_version, version, manifest, error)) {
        return false;
    }
    for (const auto& field : manifest.fields) {
        DatasetField f;
        f.id = field.id;
        f.parent_id = field.parent_id;
        f.name = field.name;
        f.logical_type = field.logical_type;
        f.nullable = field.nullable;
        f.encoding = field.encoding;
        for (const auto& [k, v] : field.metadata) {
            f.metadata[k] = std::string(v.begin(), v.end());
        }
        out.push_back(std::move(f));
    }
    return true;
}

bool dataset_update_field_metadata(const std::filesystem::path& dataset_path,
                                   const std::map<std::int32_t, FieldMetadataUpdate>& updates,
                                   std::uint64_t& new_version, std::string& error) {
    pb::Manifest manifest;
    std::uint64_t latest = 0;
    if (!load_latest_manifest(dataset_path, manifest, latest, error)) {
        return false;
    }
    for (const auto& [id, update] : updates) {
        const auto field = std::find_if(manifest.fields.begin(), manifest.fields.end(),
                                        [id = id](const pb::Field& f) { return f.id == id; });
        if (field == manifest.fields.end()) {
            error = "Field with id " + std::to_string(id) + " not found";
            return false;
        }
        if (update.replace) {
            field->metadata.clear();
        }
        for (const auto& [key, value] : update.entries) {
            if (value) {
                field->metadata[key] = std::vector<std::uint8_t>(value->begin(), value->end());
            } else if (!update.replace) {
                field->metadata.erase(key);
            }
        }
    }
    return commit_next_version(dataset_path, std::move(manifest), new_version, error);
}

bool dataset_data_stats(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                        std::vector<std::pair<std::int32_t, std::uint64_t>>& out, std::string& error) {
    out.clear();
    pb::Manifest manifest;
    if (!load(dataset_path, has_version, version, manifest, error)) {
        return false;
    }
    std::map<std::int32_t, std::uint64_t> bytes;
    for (const auto& field : manifest.fields) {
        bytes[field.id] = 0;
        out.emplace_back(field.id, 0);
    }
    std::vector<BasePath> bases;
    if (!read_base_paths(manifest, bases, error)) {
        return false;
    }
    for (const auto& fragment : manifest.fragments) {
        for (const auto& file : fragment.files) {
            // Lance opens the files that hold a field of the schema.
            std::map<std::uint32_t, std::int32_t> column_field;
            bool in_schema = false;
            for (std::size_t i = 0; i < file.fields.size(); ++i) {
                in_schema = in_schema || bytes.count(file.fields[i]) != 0U;
                const std::int32_t column = i < file.column_indices.size() ? file.column_indices[i]
                                            : file.column_indices.empty() ? static_cast<std::int32_t>(i) : -1;
                if (column >= 0) {
                    column_field[static_cast<std::uint32_t>(column)] = file.fields[i];
                }
            }
            if (!in_schema || file.file_major_version < 2U) {
                continue;  // Lance keeps no statistics for legacy (v1) files
            }
            std::filesystem::path data_dir;
            if (!data_file_dir(dataset_path, bases, file, data_dir, error)) {
                return false;
            }
            const auto jailed = safe_join_under(data_dir, file.path);
            if (!jailed) {
                error = "data file path escapes the dataset: " + file.path;
                return false;
            }
            LanceFileInfo info;
            ArrowSchema schema{};
            if (!lance_file_info(*jailed, info, schema, error)) {
                return false;
            }
            if (schema.release != nullptr) {
                schema.release(&schema);
            }
            std::int32_t current = 0;
            for (std::size_t c = 0; c < info.pages.size(); ++c) {
                const auto it = column_field.find(static_cast<std::uint32_t>(c));
                if (it != column_field.end()) {
                    current = it->second;
                }
                std::uint64_t size = 0;
                for (const auto& page : info.pages[c]) {
                    for (const auto& buffer : page.buffers) {
                        size += buffer.second;
                    }
                }
                const auto b = bytes.find(current);
                if (b != bytes.end()) {
                    b->second += size;
                }
            }
        }
    }
    for (auto& entry : out) {
        entry.second = bytes[entry.first];
    }
    return true;
}

}  // namespace nano_lance
