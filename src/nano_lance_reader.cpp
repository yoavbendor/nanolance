#include "nano_lance_writer/nano_lance_reader.h"

#include "lance_minimal.pb.hpp"
#include "nano_lance_writer/manifest_reader.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace {

void set_error(char* error_message, size_t error_message_capacity, const std::string& msg) {
    if (error_message != nullptr && error_message_capacity > 0U) {
        std::strncpy(error_message, msg.c_str(), error_message_capacity - 1U);
        error_message[error_message_capacity - 1U] = '\0';
    }
}

char* dup_cstr(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1U));
    if (p == nullptr) {
        return nullptr;
    }
    std::memcpy(p, s.c_str(), s.size() + 1U);
    return p;
}

uint8_t* dup_bytes(const std::vector<std::uint8_t>& v) {
    if (v.empty()) {
        auto* p = static_cast<uint8_t*>(std::malloc(1U));
        if (p != nullptr) {
            p[0] = 0;
        }
        return p;
    }
    auto* p = static_cast<uint8_t*>(std::malloc(v.size()));
    if (p == nullptr) {
        return nullptr;
    }
    std::memcpy(p, v.data(), v.size());
    return p;
}

void free_metadata_entries(NanoLanceReaderMetadataEntry* entries, size_t len) {
    if (entries == nullptr) {
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        std::free(entries[i].key);
        std::free(entries[i].value_bytes);
    }
    std::free(entries);
}

void free_field(NanoLanceReaderField* f) {
    if (f == nullptr) {
        return;
    }
    std::free(f->name);
    std::free(f->logical_type);
    free_metadata_entries(f->metadata, f->metadata_len);
}

void free_data_file(NanoLanceReaderDataFile* df) {
    if (df == nullptr) {
        return;
    }
    std::free(df->path);
    std::free(df->field_ids);
    std::free(df->column_indices);
}

void free_fragment(NanoLanceReaderFragment* fr) {
    if (fr == nullptr) {
        return;
    }
    for (size_t i = 0; i < fr->files_len; ++i) {
        free_data_file(&fr->files[i]);
    }
    std::free(fr->files);
}

bool fill_metadata(const std::map<std::string, std::vector<std::uint8_t>>& meta_map, NanoLanceReaderField* out_field,
                   char* error_message, size_t error_message_capacity) {
    out_field->metadata = nullptr;
    out_field->metadata_len = 0;
    if (meta_map.empty()) {
        return true;
    }
    auto* entries = static_cast<NanoLanceReaderMetadataEntry*>(std::calloc(meta_map.size(), sizeof(NanoLanceReaderMetadataEntry)));
    if (entries == nullptr) {
        set_error(error_message, error_message_capacity, "out of memory allocating metadata");
        return false;
    }
    size_t i = 0;
    for (const auto& kv : meta_map) {
        entries[i].key = dup_cstr(kv.first);
        entries[i].value_len = kv.second.size();
        entries[i].value_bytes = dup_bytes(kv.second);
        if (entries[i].key == nullptr || entries[i].value_bytes == nullptr) {
            for (size_t j = 0; j <= i; ++j) {
                std::free(entries[j].key);
                std::free(entries[j].value_bytes);
            }
            std::free(entries);
            set_error(error_message, error_message_capacity, "out of memory duplicating metadata");
            return false;
        }
        ++i;
    }
    out_field->metadata = entries;
    out_field->metadata_len = meta_map.size();
    return true;
}

bool fill_from_manifest(const nano_lance::pb::Manifest& manifest, std::uint64_t manifest_version,
                          NanoLanceDatasetMetadata* out, char* error_message, size_t error_message_capacity) {
    std::memset(out, 0, sizeof(NanoLanceDatasetMetadata));
    out->manifest_version = manifest_version;
    out->file_format = dup_cstr(manifest.data_format.file_format);
    out->format_version = dup_cstr(manifest.data_format.version);
    if (out->file_format == nullptr || out->format_version == nullptr) {
        nano_lance_dataset_metadata_free(out);
        set_error(error_message, error_message_capacity, "out of memory duplicating format strings");
        return false;
    }
    out->has_max_fragment_id = manifest.has_max_fragment_id;
    out->max_fragment_id = manifest.max_fragment_id;

    for (const auto& frag : manifest.fragments) {
        out->total_physical_rows += frag.physical_rows;
    }

    if (!manifest.fields.empty()) {
        out->fields = static_cast<NanoLanceReaderField*>(std::calloc(manifest.fields.size(), sizeof(NanoLanceReaderField)));
        if (out->fields == nullptr) {
            nano_lance_dataset_metadata_free(out);
            set_error(error_message, error_message_capacity, "out of memory allocating fields");
            return false;
        }
        out->fields_len = manifest.fields.size();
        for (size_t i = 0; i < manifest.fields.size(); ++i) {
            const auto& sf = manifest.fields[i];
            NanoLanceReaderField& df = out->fields[i];
            df.id = sf.id;
            df.parent_id = sf.parent_id;
            df.type_field = sf.type;
            df.encoding = sf.encoding;
            df.nullable = sf.nullable;
            df.name = dup_cstr(sf.name);
            df.logical_type = dup_cstr(sf.logical_type);
            if (df.name == nullptr || df.logical_type == nullptr) {
                nano_lance_dataset_metadata_free(out);
                set_error(error_message, error_message_capacity, "out of memory duplicating field names");
                return false;
            }
            if (!fill_metadata(sf.metadata, &df, error_message, error_message_capacity)) {
                nano_lance_dataset_metadata_free(out);
                return false;
            }
        }
    }

    if (!manifest.fragments.empty()) {
        out->fragments =
            static_cast<NanoLanceReaderFragment*>(std::calloc(manifest.fragments.size(), sizeof(NanoLanceReaderFragment)));
        if (out->fragments == nullptr) {
            nano_lance_dataset_metadata_free(out);
            set_error(error_message, error_message_capacity, "out of memory allocating fragments");
            return false;
        }
        out->fragments_len = manifest.fragments.size();
        for (size_t fi = 0; fi < manifest.fragments.size(); ++fi) {
            const auto& sf = manifest.fragments[fi];
            NanoLanceReaderFragment& df = out->fragments[fi];
            df.id = sf.id;
            df.physical_rows = sf.physical_rows;
            if (!sf.files.empty()) {
                df.files = static_cast<NanoLanceReaderDataFile*>(std::calloc(sf.files.size(), sizeof(NanoLanceReaderDataFile)));
                if (df.files == nullptr) {
                    nano_lance_dataset_metadata_free(out);
                    set_error(error_message, error_message_capacity, "out of memory allocating data files");
                    return false;
                }
                df.files_len = sf.files.size();
                for (size_t di = 0; di < sf.files.size(); ++di) {
                    const auto& sfile = sf.files[di];
                    NanoLanceReaderDataFile& dfile = df.files[di];
                    dfile.path = dup_cstr(sfile.path);
                    if (dfile.path == nullptr) {
                        nano_lance_dataset_metadata_free(out);
                        set_error(error_message, error_message_capacity, "out of memory duplicating data file path");
                        return false;
                    }
                    dfile.file_size_bytes = sfile.file_size_bytes;
                    dfile.file_major_version = sfile.file_major_version;
                    dfile.file_minor_version = sfile.file_minor_version;
                    if (!sfile.fields.empty()) {
                        dfile.field_ids = static_cast<int32_t*>(std::malloc(sfile.fields.size() * sizeof(int32_t)));
                        if (dfile.field_ids == nullptr) {
                            nano_lance_dataset_metadata_free(out);
                            set_error(error_message, error_message_capacity, "out of memory allocating field_ids");
                            return false;
                        }
                        dfile.field_ids_len = sfile.fields.size();
                        std::memcpy(dfile.field_ids, sfile.fields.data(), sfile.fields.size() * sizeof(int32_t));
                    }
                    if (!sfile.column_indices.empty()) {
                        dfile.column_indices =
                            static_cast<int32_t*>(std::malloc(sfile.column_indices.size() * sizeof(int32_t)));
                        if (dfile.column_indices == nullptr) {
                            nano_lance_dataset_metadata_free(out);
                            set_error(error_message, error_message_capacity, "out of memory allocating column_indices");
                            return false;
                        }
                        dfile.column_indices_len = sfile.column_indices.size();
                        std::memcpy(dfile.column_indices, sfile.column_indices.data(),
                                    sfile.column_indices.size() * sizeof(int32_t));
                    }
                }
            }
        }
    }

    return true;
}

}  // namespace

extern "C" {

void nano_lance_dataset_metadata_free(NanoLanceDatasetMetadata* metadata) {
    if (metadata == nullptr) {
        return;
    }
    std::free(metadata->file_format);
    std::free(metadata->format_version);
    for (size_t i = 0; i < metadata->fields_len; ++i) {
        free_field(&metadata->fields[i]);
    }
    std::free(metadata->fields);
    for (size_t i = 0; i < metadata->fragments_len; ++i) {
        free_fragment(&metadata->fragments[i]);
    }
    std::free(metadata->fragments);
    std::memset(metadata, 0, sizeof(NanoLanceDatasetMetadata));
}

int nano_lance_dataset_read_latest(const char* dataset_path, NanoLanceDatasetMetadata* out, char* error_message,
                                   size_t error_message_capacity) {
    if (dataset_path == nullptr || dataset_path[0] == '\0' || out == nullptr) {
        set_error(error_message, error_message_capacity, "invalid argument");
        return NANO_LANCE_READER_INVALID_ARGUMENT;
    }
    std::memset(out, 0, sizeof(NanoLanceDatasetMetadata));
    std::string err;
    nano_lance::pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!nano_lance::load_latest_manifest(dataset_path, manifest, version, err)) {
        set_error(error_message, error_message_capacity, err);
        return NANO_LANCE_READER_IO_ERROR;
    }
    if (!fill_from_manifest(manifest, version, out, error_message, error_message_capacity)) {
        return NANO_LANCE_READER_IO_ERROR;
    }
    return NANO_LANCE_READER_OK;
}

}  // extern "C"
