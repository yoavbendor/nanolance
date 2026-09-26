// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/manifest_writer.hpp"

#include "lance_minimal.pb.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/version.hpp"
#include <sstream>
#include <iomanip>
#include "nanolance/schema_mapper.hpp"

#include <array>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <limits>
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
/// dataset unreadable by the tool that created it. A fresh dataset keeps nanolance's own V1 naming.
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

    const auto manifest_bytes = pb::encode_manifest(manifest);
    bool existing_is_v2 = false;
    (void)next_version(versions_dir, existing_is_v2);
    const auto name = manifest_filename(manifest.version, existing_is_v2);
    const auto temp_path = versions_dir / (name + ".tmp");
    const auto final_path = versions_dir / name;
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open manifest temp file";
        return false;
    }
    write_le32(out, 0);
    const std::uint64_t manifest_position = 4;
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

    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        error = "failed to atomically publish manifest: " + ec.message();
        return false;
    }
    return true;
}

namespace {

pb::Field manifest_field(const LanceField& mapped_field) {
    pb::Field field;
    field.name = mapped_field.name;
    field.logical_type = lance_on_disk_logical_type(mapped_field.logical_type);
    field.id = mapped_field.id;
    field.parent_id = mapped_field.parent_id;
    field.type = 2;
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

const char* nanolance_writer_version() {
    return nanolance::library_version();
}

bool commit_dataset_version(const std::filesystem::path& dataset_path, const LanceSchemaMapping& mapping,
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
        if (mode == CommitMode::Append && (prior.writer_feature_flags & pb::kFlagStableRowIds) != 0U) {
            // Every fragment of such a dataset carries its row ids; ours would carry none.
            error = "appending to a dataset with stable row ids is not supported";
            return false;
        }
    }

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
    }
    for (const auto& fragment : fragments) {
        if (next_id > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            error = "fragment id overflow";
            return false;
        }
        pb::DataFragment added;
        added.id = next_id++;
        added.physical_rows = fragment.rows;
        added.files.push_back(manifest_data_file(mapping, fragment.data_file));
        manifest.fragments.push_back(std::move(added));
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
