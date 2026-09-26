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
    /// Wire bytes of the fields this codec does not model (dictionary, storage class, ...), kept so a
    /// schema read from another writer's manifest is written back unchanged.
    std::vector<std::uint8_t> unknown;
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
    std::vector<std::uint8_t> unknown;  // see Field::unknown
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
    std::vector<std::uint8_t> unknown;  // see Field::unknown
};

struct DataFragment {
    std::uint64_t id = 0;
    std::vector<DataFile> files;
    std::uint64_t physical_rows = 0;
    DeletionFile deletion_file;
    std::vector<std::uint8_t> unknown;  // see Field::unknown (row id sequences, version metadata, ...)
};

struct Manifest {
    std::vector<Field> fields;
    std::vector<DataFragment> fragments;
    std::uint64_t version = 0;
    DataStorageFormat data_format;
    bool has_max_fragment_id = false;
    std::uint32_t max_fragment_id = 0;
    /// Arrow schema metadata (field 5).
    std::map<std::string, std::vector<std::uint8_t>> schema_metadata;
    /// When the version was committed (field 7, a google.protobuf.Timestamp).
    bool has_timestamp = false;
    std::int64_t timestamp_seconds = 0;
    std::int32_t timestamp_nanos = 0;
    std::string tag;                        // field 8
    std::uint64_t reader_feature_flags = 0;  // field 9
    std::uint64_t writer_feature_flags = 0;  // field 10
    std::string transaction_file;            // field 12
    std::string writer_library;              // field 13.1
    std::string writer_version;              // field 13.2
    std::uint64_t next_row_id = 0;           // field 14
    std::map<std::string, std::string> config;          // field 16
    std::map<std::string, std::string> table_metadata;  // field 19
    /// Wire bytes of the other fields (base paths, branch, ...). The ones that point INTO the manifest
    /// file this was read from (version_aux_data, index_section, transaction_section) are not kept:
    /// they would point at the wrong bytes of a new file.
    std::vector<std::uint8_t> unknown;
};

/// Reader feature flags, as Lance defines them (Manifest.reader_feature_flags).
constexpr std::uint64_t kFlagDeletionFiles = 1U << 0U;
constexpr std::uint64_t kFlagStableRowIds = 1U << 1U;
constexpr std::uint64_t kFlagUseV2Format = 1U << 2U;
constexpr std::uint64_t kFlagTableConfig = 1U << 3U;
constexpr std::uint64_t kFlagMultipleBasePaths = 1U << 4U;

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
