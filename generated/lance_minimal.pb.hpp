#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nano_lance::pb {

struct DataStorageFormat {
    std::string file_format;
    std::string version;
};

struct Field {
    std::string name;
    std::string logical_type;
    std::int32_t id = 0;
    std::int32_t parent_id = -1;
    std::int32_t type = 2;
    bool nullable = true;
    std::int32_t encoding = 1;
    /// Protobuf map<string, bytes> metadata (field number 10 in Lance `file.Field`).
    std::map<std::string, std::vector<std::uint8_t>> metadata;
};

struct FileDescriptor {
    std::vector<Field> fields;
    std::uint64_t length = 0;
};

struct DataFile {
    std::string path;
    std::vector<std::int32_t> fields;
    std::vector<std::int32_t> column_indices;
    std::uint32_t file_major_version = 0;
    std::uint32_t file_minor_version = 0;
    std::uint64_t file_size_bytes = 0;
};

/// A fragment's deletion file: which of its rows Lance considers deleted.
///
/// Lives at `_deletions/{fragment_id}-{read_version}-{id}.{arrow|bin}` and comes in two shapes --
/// an Arrow IPC file of u32 row offsets when deletions are sparse, a roaring bitmap when they are
/// dense (Lance switches above 5000). `present` is false when the fragment has none.
struct DeletionFile {
    bool present = false;
    std::uint32_t file_type = 0;  // 0 = ARROW_ARRAY (.arrow), 1 = BITMAP (.bin)
    std::uint64_t read_version = 0;
    std::uint64_t id = 0;
    std::uint64_t num_deleted_rows = 0;
};

struct DataFragment {
    std::uint64_t id = 0;
    std::vector<DataFile> files;
    std::uint64_t physical_rows = 0;
    DeletionFile deletion_file;
};

struct Manifest {
    std::vector<Field> fields;
    std::vector<DataFragment> fragments;
    std::uint64_t version = 0;
    DataStorageFormat data_format;
    bool has_max_fragment_id = false;
    std::uint32_t max_fragment_id = 0;
};

struct Metadata {
    std::uint64_t page_table_position = 0;
    std::vector<std::int32_t> batch_offsets;
};

struct ColumnPage {
    std::vector<std::uint64_t> buffer_offsets;
    std::vector<std::uint64_t> buffer_sizes;
    std::uint64_t length = 0;
    std::uint64_t priority = 0;
    std::vector<std::uint8_t> encoding;
};

struct ColumnMetadata {
    std::vector<std::uint8_t> encoding;
    std::vector<ColumnPage> pages;
};

std::vector<std::uint8_t> encode_manifest(const Manifest& manifest);
bool decode_manifest(const std::vector<std::uint8_t>& bytes, Manifest& manifest);

bool decode_field(const std::vector<std::uint8_t>& bytes, Field& field);

std::vector<std::uint8_t> encode_file_descriptor(const FileDescriptor& descriptor);
bool decode_file_descriptor(const std::vector<std::uint8_t>& bytes, FileDescriptor& descriptor);

std::vector<std::uint8_t> encode_data_fragment(const DataFragment& fragment);
bool decode_data_fragment(const std::vector<std::uint8_t>& bytes, DataFragment& fragment);

std::vector<std::uint8_t> encode_data_file(const DataFile& file);
bool decode_data_file(const std::vector<std::uint8_t>& bytes, DataFile& file);

std::vector<std::uint8_t> encode_metadata(const Metadata& metadata);
bool decode_metadata(const std::vector<std::uint8_t>& bytes, Metadata& metadata);

std::vector<std::uint8_t> encode_column_metadata(const ColumnMetadata& metadata);
bool decode_column_metadata(const std::vector<std::uint8_t>& bytes, ColumnMetadata& metadata);

}  // namespace nano_lance::pb
