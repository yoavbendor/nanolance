// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/manifest_writer.hpp"
#include "nanolance/row_ids.hpp"

#include "lance_minimal.pb.hpp"
#include "nanolance/index_maintenance.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/version.hpp"
#include "nanolance/dataset_refs.hpp"
#include "transaction_file.hpp"
#include <sstream>
#include <iomanip>
#include "nanolance/schema_mapper.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <limits>
#include <random>
#include <thread>
#include <utility>

namespace nano_lance {
namespace {

void write_le16(std::ostream& out, std::uint16_t value) {
    const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU)};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_le32(std::ostream& out, std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xFFU),
        static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU),
        static_cast<char>((value >> 24U) & 0xFFU),
    };
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_le64(std::ostream& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        const auto byte = static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU);
        out.write(&byte, 1);
    }
}

/// Lance's V2 manifest filename: `u64::MAX - version`, zero-padded to 20 digits.
std::string manifest_filename(std::uint64_t version, bool v2) {
    if ((version & (1ULL << 63U)) != 0U) {
        return "d" + std::to_string(version) + ".manifest";  // a detached version, outside the lineage
    }
    if (!v2) {
        return std::to_string(version) + ".manifest";
    }
    std::ostringstream name;
    name << std::setfill('0') << std::setw(20)
         << (std::numeric_limits<std::uint64_t>::max() - version);
    return name.str() + ".manifest";
}

/// Next version, and which naming scheme the directory already uses.
///
/// The scheme matters as much as the number: stock Lance REFUSES to open a `_versions` directory
/// holding both schemes ("Found multiple manifest naming schemes in the same directory"). So an
/// append onto a pylance dataset has to keep writing pylance's names, or nanolance would make the
/// dataset unreadable by the tool that created it. A fresh dataset gets V2 names, as Lance's default.
std::uint64_t next_version(const std::filesystem::path& versions_dir, bool& v2_out) {
    std::uint64_t max_version = 0;
    v2_out = false;
    std::error_code ec;
    if (!std::filesystem::exists(versions_dir, ec)) {
        return 1;
    }
    for (const auto& entry : std::filesystem::directory_iterator(versions_dir, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        // Shared with the reader on purpose: this used to be a private copy that parsed the digits
        // literally, so appending to a pylance dataset (whose manifests are named u64::MAX - version)
        // numbered the new manifest u64::MAX -- which reads back as version 0, behind everything.
        const auto name = entry.path().filename().string();
        std::uint64_t version = 0;
        if (parse_manifest_version(name, version)) {
            max_version = std::max(max_version, version);
            // 20 digits + ".manifest" is V2; see manifest_filename above.
            if (name.size() == 20U + 9U) {
                v2_out = true;
            }
        }
    }
    return max_version + 1;
}

}  // namespace

namespace {

/// A Lance file version as one number -- 1 for v1, 20 to 23 for 2.0 to 2.3 -- from a data file's
/// major/minor (Lance's from_data_file_numbers), or 0 when it is none of these.
int data_file_version(std::uint32_t major, std::uint32_t minor) {
    if (major == 0U && minor <= 2U) {
        return 1;
    }
    if ((major == 0U && minor == 3U) || (major == 2U && minor <= 3U)) {
        return major == 0U ? 20 : 20 + static_cast<int>(minor);
    }
    return 0;
}

/// The same from the manifest's data storage version ("2.1", or "legacy" / "0.x" for v1).
int storage_version(const std::string& version) {
    if (version == "legacy" || version.rfind("0.", 0) == 0) {
        return 1;
    }
    if (version.size() == 3U && version[0] == '2' && version[1] == '.' && version[2] >= '0' && version[2] <= '3') {
        return 20 + (version[2] - '0');
    }
    return 0;
}

/// nanolance writes 2.2 files, into datasets of any 2.x version. Lance reads data files of another
/// 2.x version than the manifest's default only when the manifest says so (kFlagMixedDataFileVersions,
/// which Lance's own commits set the same way); v1 and v2 files never mix.
bool check_data_file_versions(pb::Manifest& manifest, std::string& error) {
    const int default_version = storage_version(manifest.data_format.version);
    if (default_version == 0) {
        return true;  // a version this build does not know: left as it is
    }
    bool v1 = default_version == 1;
    bool v2 = default_version != 1;
    bool other = false;
    for (const auto& fragment : manifest.fragments) {
        for (const auto& file : fragment.files) {
            const int version = data_file_version(file.file_major_version, file.file_minor_version);
            v1 = v1 || version == 1;
            v2 = v2 || version >= 20;
            other = other || (version != 0 && version != default_version);
        }
    }
    if (v1 && v2) {
        error = "the dataset is in Lance's v1 (legacy) format, which nanolance does not write";
        return false;
    }
    if (other) {
        manifest.reader_feature_flags |= pb::kFlagMixedDataFileVersions;
        manifest.writer_feature_flags |= pb::kFlagMixedDataFileVersions;
    }
    return true;
}

}  // namespace

bool publish_manifest(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                      std::string& error) {
    error.clear();
    const auto versions_dir = dataset_path / "_versions";
    std::error_code ec;
    std::filesystem::create_directories(versions_dir, ec);
    if (ec) {
        error = "failed to create _versions directory: " + ec.message();
        return false;
    }

    if (!manifest.index_section_error.empty()) {
        // Committing would drop indices this version has but could not be read.
        error = "the dataset's indices could not be read, and a commit would drop them: " +
                manifest.index_section_error;
        return false;
    }
    // The indices go first, [u32 length][IndexSection] as Lance writes them, and the manifest points
    // at them (field 6). Without indices there is no section, and no stale pointer either.
    std::vector<std::uint8_t> index_section;
    pb::Manifest written = manifest;
    if (!check_data_file_versions(written, error)) {
        return false;
    }
    // The transaction first, as Lance writes it: the manifest names it, and Lance reads it to check
    // a concurrent commit of its own against this one.
    std::string transaction;
    if (!write_transaction_file(dataset_path, written, transaction, error)) {
        return false;
    }
    written.transaction_file = transaction;
    struct DropUnlessCommitted {
        const std::filesystem::path& path;
        const std::string& name;
        bool committed = false;
        ~DropUnlessCommitted() {
            if (!committed) {
                remove_transaction_file(path, name);
            }
        }
    } drop_transaction{dataset_path, transaction};
    written.has_index_section = !manifest.indices.empty();
    std::uint64_t manifest_position = 4;
    if (written.has_index_section) {
        index_section = pb::encode_index_section(manifest.indices);
        written.index_section = 4;
        manifest_position = 4U + 4U + index_section.size();
    }
    const auto manifest_bytes = pb::encode_manifest(written);
    bool existing_is_v2 = false;
    const bool first = next_version(versions_dir, existing_is_v2) == 1U;
    // A new dataset gets V2 names, Lance's default (enable_v2_manifest_paths); an existing one keeps its
    // scheme, since Lance refuses a _versions directory that mixes the two.
    const auto name = manifest_filename(manifest.version, first || existing_is_v2);
    // The temp name is the writer's own: two writers racing for one version must not share it, or
    // one's link could publish the other's manifest (and report success for a change it lost).
    static std::atomic<std::uint64_t> counter{0};
    const auto unique = std::to_string(std::random_device{}()) + "-" + std::to_string(counter.fetch_add(1U));
    const auto temp_path = versions_dir / (name + "." + unique + ".tmp");
    const auto final_path = versions_dir / name;
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open manifest temp file";
        return false;
    }
    write_le32(out, 0);
    if (written.has_index_section) {
        write_le32(out, static_cast<std::uint32_t>(index_section.size()));
        out.write(reinterpret_cast<const char*>(index_section.data()), static_cast<std::streamsize>(index_section.size()));
    }
    write_le32(out, static_cast<std::uint32_t>(manifest_bytes.size()));
    out.write(reinterpret_cast<const char*>(manifest_bytes.data()),
              static_cast<std::streamsize>(manifest_bytes.size()));
    write_le64(out, manifest_position);
    write_le16(out, 0);
    write_le16(out, 2);
    out.write("LANC", 4);
    out.close();
    if (!out) {
        error = "failed to write manifest temp file";
        return false;
    }

    // Publish without replacing: a hard link fails if the name exists, so a version another writer
    // committed meanwhile is never overwritten (rename would silently replace it). File systems
    // without hard links fall back to rename.
    std::filesystem::create_hard_link(temp_path, final_path, ec);
    if (!ec) {
        drop_transaction.committed = true;
        std::filesystem::remove(temp_path, ec);
        if ((manifest.version & (1ULL << 63U)) == 0U) {
            run_auto_cleanup(dataset_path, manifest.version, manifest.config);
        }
        return true;
    }
    if (std::filesystem::exists(final_path)) {
        std::filesystem::remove(temp_path, ec);
        error = "commit conflict: version " + std::to_string(manifest.version) + " was committed concurrently";
        return false;
    }
    ec.clear();
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        error = "failed to atomically publish manifest: " + ec.message();
        return false;
    }
    drop_transaction.committed = true;
    run_auto_cleanup(dataset_path, manifest.version, manifest.config);
    return true;
}



namespace {

pb::Field manifest_field(const LanceField& mapped_field) {
    pb::Field field;
    field.name = mapped_field.name;
    field.logical_type = lance_field_disk_logical_type(mapped_field);
    field.id = mapped_field.id;
    field.parent_id = mapped_field.parent_id;
    field.type = 2;
    field.encoding = mapped_field.dictionary_index_format.empty() ? lance_on_disk_field_encoding(mapped_field.logical_type) : 3;
    field.nullable = mapped_field.nullable;
    for (const auto& kv : mapped_field.metadata) {
        // Per-file encoding choices live in each data file's own schema; the dataset's is logical.
        if (kv.first == "nanolance:packing" || kv.first == "nanolance:const-value") {
            continue;
        }
        field.metadata[kv.first] = std::vector<std::uint8_t>(kv.second.begin(), kv.second.end());
    }
    return field;
}

pb::DataFile manifest_data_file(const LanceSchemaMapping& mapping, const DataFileResult& data_file) {
    pb::DataFile file;
    file.path = data_file.relative_path.filename().generic_string();
    file.file_major_version = 2;
    file.file_minor_version = 2;
    file.file_size_bytes = data_file.file_size_bytes;
    for (const auto* mapped_field : lance_physical_fields(mapping)) {
        file.fields.push_back(mapped_field->id);
        file.column_indices.push_back(mapped_field->column_index);
    }
    return file;
}

}  // namespace

pb::Field make_manifest_field(const LanceField& field) {
    return manifest_field(field);
}

pb::DataFragment make_data_fragment(const LanceSchemaMapping& mapping, const NewFragment& fragment, std::uint64_t id) {
    pb::DataFragment out;
    out.id = id;
    out.physical_rows = fragment.rows;
    out.files.push_back(manifest_data_file(mapping, fragment.data_file));
    return out;
}

const char* nanolance_writer_version() {
    return nanolance::library_version();
}

namespace {

bool commit_dataset_version_once(const std::filesystem::path& dataset_path, const LanceSchemaMapping& mapping,
                                 const std::vector<NewFragment>& fragments, CommitMode mode, std::uint64_t& version,
                                 std::string& error, const CommitExtras& extras);

}  // namespace

bool commit_dataset_version(const std::filesystem::path& dataset_path, const LanceSchemaMapping& mapping,
                            const std::vector<NewFragment>& fragments, CommitMode mode, std::uint64_t& version,
                            std::string& error, const CommitExtras& extras) {
    // An append reads nothing of the dataset but its schema, and an overwrite nothing at all, so
    // when another writer commits first either still applies after it: built again on the newer
    // version and retried, as Lance retries compatible transactions.
    for (int attempt = 0;; ++attempt) {
        if (commit_dataset_version_once(dataset_path, mapping, fragments, mode, version, error, extras)) {
            return true;
        }
        if (mode == CommitMode::Create || error.rfind("commit conflict", 0) != 0 || attempt + 1 >= 20) {
            return false;
        }
        thread_local std::mt19937 rng{std::random_device{}()};
        const int ceiling = std::min(50, 1 << std::min(attempt, 5));
        std::this_thread::sleep_for(std::chrono::milliseconds(std::uniform_int_distribution<int>(1, ceiling)(rng)));
    }
}

namespace {

bool commit_dataset_version_once(const std::filesystem::path& dataset_path, const LanceSchemaMapping& mapping,
                                 const std::vector<NewFragment>& fragments, CommitMode mode, std::uint64_t& version,
                                 std::string& error, const CommitExtras& extras) {
    error.clear();
    const auto versions_dir = dataset_path / "_versions";
    std::error_code ec;
    std::filesystem::create_directories(versions_dir, ec);
    if (ec) {
        error = "failed to create _versions directory: " + ec.message();
        return false;
    }

    bool unused_v2 = false;
    version = next_version(versions_dir, unused_v2);
    const bool exists = version > 1U;
    if (mode == CommitMode::Create && exists) {
        error = "dataset already exists: " + dataset_path.string();
        return false;
    }
    if (mode == CommitMode::Append && !exists) {
        error = "append requested but no manifest version exists";
        return false;
    }

    pb::Manifest prior;
    if (exists) {
        std::uint64_t prior_version = 0;
        if (!load_latest_manifest(dataset_path, prior, prior_version, error)) {
            return false;
        }
    }
    const bool prior_stable = exists && (prior.reader_feature_flags & pb::kFlagStableRowIds) != 0U;
    if (extras.stable_row_ids && exists && mode == CommitMode::Append && !prior_stable) {
        error = "This dataset was not created with the stable row ids feature.  Please run "
                "`migrate_to_stable_row_ids` before attempting to use stable row ids";
        return false;
    }
    const bool stable = prior_stable || (extras.stable_row_ids && mode != CommitMode::Append);

    pb::Manifest manifest;
    manifest.version = version;
    manifest.data_format.file_format = "lance";
    manifest.data_format.version = "2.2";
    manifest.has_timestamp = true;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    manifest.timestamp_seconds = static_cast<std::int64_t>(nanos / 1000000000LL);
    manifest.timestamp_nanos = static_cast<std::int32_t>(nanos % 1000000000LL);
    manifest.writer_library = "nanolance";
    manifest.writer_version = nanolance_writer_version();
    if (exists) {
        // What belongs to the table rather than to its data survives every commit.
        manifest.config = prior.config;
        manifest.table_metadata = prior.table_metadata;
        manifest.unknown = prior.unknown;
        manifest.next_row_id = prior.next_row_id;
    }
    for (const auto& kv : extras.table_metadata) {
        manifest.table_metadata[kv.first] = kv.second;
    }
    manifest.transaction_properties = extras.transaction_properties;
    if (!exists) {
        for (const auto& kv : extras.initial_config) {
            manifest.config[kv.first] = kv.second;
        }
    }

    std::uint64_t next_id = 0;
    if (mode == CommitMode::Append) {
        // The prior schema, as read -- its field records carry what this codec does not model.
        manifest.fields = prior.fields;
        manifest.schema_metadata = prior.schema_metadata;
        manifest.data_format = prior.data_format;
        manifest.reader_feature_flags = prior.reader_feature_flags;
        manifest.writer_feature_flags = prior.writer_feature_flags;
        manifest.fragments = prior.fragments;
        manifest.operation = pb::Manifest::Operation::Append;
        manifest.first_new_fragment = prior.fragments.size();
        // An append keeps every index as it is; the new fragments are simply not covered.
        manifest.indices = prior.indices;
        manifest.index_section_error = prior.index_section_error;
        next_id = first_fragment_id_after_indices(prior.indices);
        for (const auto& fr : prior.fragments) {
            next_id = std::max(next_id, fr.id + 1U);
        }
        if (prior.has_max_fragment_id) {
            next_id = std::max(next_id, static_cast<std::uint64_t>(prior.max_fragment_id) + 1U);
        }
    } else {
        for (const auto& mapped_field : mapping.fields) {
            manifest.fields.push_back(manifest_field(mapped_field));
        }
        if (exists && prior.has_max_fragment_id) {
            next_id = static_cast<std::uint64_t>(prior.max_fragment_id) + 1U;
        }
    }
    if (extras.schema_metadata != nullptr) {
        manifest.schema_metadata = *extras.schema_metadata;
    }
    if (mode == CommitMode::Overwrite || mode == CommitMode::Create) {
        next_id = exists && prior.has_max_fragment_id ? next_id : 0U;
        manifest.operation = pb::Manifest::Operation::Overwrite;
    }
    std::uint64_t next_row_id = exists ? prior.next_row_id : 0U;
    for (const auto& fragment : fragments) {
        if (next_id > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            error = "fragment id overflow";
            return false;
        }
        pb::DataFragment added;
        added.id = next_id++;
        added.physical_rows = fragment.rows;
        added.files.push_back(manifest_data_file(mapping, fragment.data_file));
        if (stable) {
            // Row ids from the table's high-water mark, and every row stamped with this version.
            FragmentRowMeta meta;
            meta.has_row_ids = true;
            meta.row_ids = RowIdSequence::range(next_row_id, fragment.rows).encode();
            next_row_id += fragment.rows;
            if (fragment.rows != 0U) {
                meta.has_created = meta.has_last_updated = true;
                meta.created = meta.last_updated = RowVersionSequence::uniform(fragment.rows, version).encode();
            }
            write_fragment_row_meta(added, meta);
        }
        manifest.fragments.push_back(std::move(added));
    }
    if (stable) {
        manifest.next_row_id = next_row_id;
        manifest.reader_feature_flags |= pb::kFlagStableRowIds;
        manifest.writer_feature_flags |= pb::kFlagStableRowIds;
    }
    std::uint64_t max_id = 0;
    bool any = false;
    bool deletions = false;
    for (const auto& fr : manifest.fragments) {
        max_id = std::max(max_id, fr.id);
        any = true;
        deletions = deletions || fr.deletion_file.present;
    }
    if (exists && prior.has_max_fragment_id) {
        max_id = std::max(max_id, static_cast<std::uint64_t>(prior.max_fragment_id));
        any = true;
    }
    if (any) {
        manifest.has_max_fragment_id = true;
        manifest.max_fragment_id = static_cast<std::uint32_t>(max_id);
    }
    if (deletions) {
        manifest.reader_feature_flags |= pb::kFlagDeletionFiles;
        manifest.writer_feature_flags |= pb::kFlagDeletionFiles;
    }
    return publish_manifest(dataset_path, manifest, error);
}

}  // namespace

bool write_dataset_manifest(const std::filesystem::path& dataset_path,
                            const LanceSchemaMapping& mapping,
                            const DataFileResult& data_file,
                            std::uint64_t rows,
                            bool is_append,
                            std::uint64_t& version,
                            std::string& error) {
    // A commit that is not an append replaces whatever is there: what the writer has always done.
    return commit_dataset_version(dataset_path, mapping, {NewFragment{data_file, rows}},
                                  is_append ? CommitMode::Append : CommitMode::Overwrite, version, error);
}

}  // namespace nano_lance

