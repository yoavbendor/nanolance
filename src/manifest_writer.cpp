#include "nano_lance_writer/manifest_writer.hpp"

#include "lance_minimal.pb.hpp"
#include "nano_lance_writer/manifest_reader.hpp"
#include "nano_lance_writer/schema_mapper.hpp"

#include <array>
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

std::uint64_t next_version(const std::filesystem::path& versions_dir) {
    std::uint64_t max_version = 0;
    std::error_code ec;
    if (!std::filesystem::exists(versions_dir, ec)) {
        return 1;
    }
    for (const auto& entry : std::filesystem::directory_iterator(versions_dir, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.size() <= 9 || name.substr(name.size() - 9) != ".manifest") {
            continue;
        }
        try {
            max_version = std::max(max_version, static_cast<std::uint64_t>(std::stoull(name.substr(0, name.size() - 9))));
        } catch (...) {
        }
    }
    return max_version + 1;
}

}  // namespace

bool write_dataset_manifest(const std::filesystem::path& dataset_path,
                            const LanceSchemaMapping& mapping,
                            const DataFileResult& data_file,
                            std::uint64_t rows,
                            bool is_append,
                            std::uint64_t& version,
                            std::string& error) {
    error.clear();
    const auto versions_dir = dataset_path / "_versions";
    std::error_code ec;
    std::filesystem::create_directories(versions_dir, ec);
    if (ec) {
        error = "failed to create _versions directory: " + ec.message();
        return false;
    }

    version = next_version(versions_dir);
    if (is_append && version == 1) {
        error = "append requested but no manifest version exists";
        return false;
    }

    pb::Manifest manifest;
    manifest.version = version;
    manifest.data_format.file_format = "lance";
    manifest.data_format.version = "2.2";
    for (const auto& mapped_field : mapping.fields) {
        pb::Field field;
        field.name = mapped_field.name;
        field.logical_type = lance_on_disk_logical_type(mapped_field.logical_type);
        field.id = mapped_field.id;
        field.parent_id = mapped_field.parent_id;
        field.type = 2;
        field.nullable = mapped_field.nullable;
        for (const auto& kv : mapped_field.metadata) {
            field.metadata[kv.first] = std::vector<std::uint8_t>(kv.second.begin(), kv.second.end());
        }
        manifest.fields.push_back(std::move(field));
    }
    pb::DataFile manifest_file;
    manifest_file.path = data_file.relative_path.filename().generic_string();
    manifest_file.file_major_version = 2;
    manifest_file.file_minor_version = 2;
    manifest_file.file_size_bytes = data_file.file_size_bytes;
    for (const auto* mapped_field : lance_physical_fields(mapping)) {
        manifest_file.fields.push_back(mapped_field->id);
        manifest_file.column_indices.push_back(mapped_field->column_index);
    }

    if (is_append) {
        pb::Manifest prior;
        std::uint64_t prior_ver = 0;
        if (!load_latest_manifest(dataset_path, prior, prior_ver, error)) {
            return false;
        }
        std::uint64_t max_frag_id = 0;
        for (const auto& fr : prior.fragments) {
            max_frag_id = std::max(max_frag_id, fr.id);
        }
        if (prior.has_max_fragment_id) {
            max_frag_id = std::max(max_frag_id, static_cast<std::uint64_t>(prior.max_fragment_id));
        }
        const std::uint64_t next_frag_id = max_frag_id + 1U;
        if (next_frag_id > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            error = "fragment id overflow for manifest append";
            return false;
        }
        manifest.fragments = prior.fragments;
        pb::DataFragment new_fragment;
        new_fragment.id = next_frag_id;
        new_fragment.physical_rows = rows;
        new_fragment.files.push_back(std::move(manifest_file));
        manifest.fragments.push_back(std::move(new_fragment));
        manifest.has_max_fragment_id = true;
        manifest.max_fragment_id = static_cast<std::uint32_t>(next_frag_id);
    } else {
        manifest.has_max_fragment_id = true;
        manifest.max_fragment_id = 0;
        pb::DataFragment fragment;
        fragment.id = 0;
        fragment.physical_rows = rows;
        fragment.files.push_back(std::move(manifest_file));
        manifest.fragments.push_back(std::move(fragment));
    }

    const auto manifest_bytes = pb::encode_manifest(manifest);

    const auto temp_path = versions_dir / (std::to_string(version) + ".manifest.tmp");
    const auto final_path = versions_dir / (std::to_string(version) + ".manifest");
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open manifest temp file";
        return false;
    }
    write_le32(out, 0);
    const std::uint64_t manifest_position = 4;
    write_le32(out, static_cast<std::uint32_t>(manifest_bytes.size()));
    out.write(reinterpret_cast<const char*>(manifest_bytes.data()), static_cast<std::streamsize>(manifest_bytes.size()));
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

}  // namespace nano_lance
