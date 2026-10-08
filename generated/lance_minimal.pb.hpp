#pragma once

#include <array>
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
    /// The schema's own metadata (Schema field 5): what Lance's index files keep about themselves
    /// (a BTree's page size, a bitmap index's statistics).
    std::map<std::string, std::vector<std::uint8_t>> schema_metadata;
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

/// One index of a dataset version (table.proto IndexMetadata), as the manifest file's index section
/// holds it. Only what the writer must adjust is decoded; the message is otherwise kept as read, so
/// an index type this codec has never heard of survives a commit byte for byte.
struct IndexMetadata {
    std::vector<std::uint8_t> raw;     // the message as read
    std::vector<std::int32_t> fields;  // 2: the field ids it covers
    std::string name;                  // 3
    std::uint64_t dataset_version = 0; // 4: the version it was built at
    /// 5: the fragments it covers (a Roaring bitmap on disk), ascending. Absent: coverage unknown.
    bool has_fragment_bitmap = false;
    std::vector<std::uint32_t> fragment_ids;
    /// The writer changed `fragment_ids`: field 5 is written anew rather than copied from `raw`.
    bool fragment_bitmap_changed = false;
    std::array<std::uint8_t, 16> uuid{};  // 1: UUID{bytes=1}
    std::string details_type_url;         // 6: the index_details Any's type, e.g. "/lance.table.BTreeIndexDetails"
    std::vector<std::uint8_t> details_value;  // 6: the index_details Any's message
    std::uint32_t index_version = 0;      // 7
    std::uint64_t created_at = 0;         // 8: milliseconds since the epoch
    struct File {
        std::string path;  // relative to _indices/<uuid>/
        std::uint64_t size = 0;
    };
    std::vector<File> files;  // 10
};

/// A new index's message, field for field as Lance writes one; `raw` and the decoded fields agree.
IndexMetadata make_index_metadata(const std::array<std::uint8_t, 16>& uuid, const std::vector<std::int32_t>& fields,
                                  const std::string& name, std::uint64_t dataset_version,
                                  const std::vector<std::uint32_t>& fragment_ids, const std::string& details_type_url,
                                  std::uint32_t index_version, std::uint64_t created_at,
                                  const std::vector<IndexMetadata::File>& files,
                                  const std::vector<std::uint8_t>& details_value = {});

/// One IndexMetadata message: read (`raw` keeps the bytes), and written (`raw` itself unless the
/// writer changed the coverage).
bool decode_index_message(const std::uint8_t* data, std::size_t size, IndexMetadata& index, std::string& error);
std::vector<std::uint8_t> encode_index_message(const IndexMetadata& index);

/// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", the directory name of an index under _indices/.
std::string uuid_string(const std::array<std::uint8_t, 16>& uuid);

/// An IndexSection message: the indices, each re-encoded only where the writer changed it.
bool decode_index_section(const std::vector<std::uint8_t>& bytes, std::vector<IndexMetadata>& out,
                          std::string& error);
std::vector<std::uint8_t> encode_index_section(const std::vector<IndexMetadata>& indices);

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
    /// Field 6: where the index section sits in the manifest file. Read with the manifest (see
    /// load_manifest_version, which fills `indices` from it); written by publish_manifest, which
    /// writes `indices` into the new file and points this at them.
    bool has_index_section = false;
    std::uint64_t index_section = 0;
    std::vector<IndexMetadata> indices;
    /// Set when the index section could not be read: `indices` is then empty and a commit refuses,
    /// rather than publish a version that silently lost them.
    std::string index_section_error;
    /// Wire bytes of the other fields (base paths, branch, ...). The ones that point INTO the manifest
    /// file this was read from (version_aux_data, index_section, transaction_section) are not kept:
    /// they would point at the wrong bytes of a new file.
    std::vector<std::uint8_t> unknown;

    /// Not on the wire: what a commit of this manifest does, for its transaction file, when the change
    /// from the version before does not say it alone (an overwrite or a restore replaces every
    /// fragment, as a compaction of all of them does). `Derive`: read it off the change.
    enum class Operation { Derive, Append, Overwrite, Restore, Rewrite, Update, Reserve };
    Operation operation = Operation::Derive;
    std::uint64_t restored_version = 0;  // Restore: the version restored
    std::size_t first_new_fragment = 0;  // Append: fragments from here on are the new ones
    std::uint32_t reserved_fragments = 0;  // Reserve: fragment ids reserved (max_fragment_id raised by it)
    /// Not on the wire: the commit's transaction properties (its message among them), for its
    /// transaction file.
    std::map<std::string, std::string> transaction_properties;
};

/// Reader feature flags, as Lance defines them (Manifest.reader_feature_flags).
constexpr std::uint64_t kFlagDeletionFiles = 1U << 0U;
constexpr std::uint64_t kFlagStableRowIds = 1U << 1U;
constexpr std::uint64_t kFlagUseV2Format = 1U << 2U;
constexpr std::uint64_t kFlagTableConfig = 1U << 3U;
constexpr std::uint64_t kFlagMultipleBasePaths = 1U << 4U;
/// Data files of several 2.x versions in one dataset: Lance 12 sets it (reader and writer) on a
/// commit that adds files of another version than the manifest's default, and refuses to read such a
/// dataset without it.
constexpr std::uint64_t kFlagMixedDataFileVersions = 1U << 8U;

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
    /// The column's own encoding (an Any: /lance.encodings.ColumnEncoding in format 2.0, where it
    /// marks blob columns).
    std::vector<std::uint8_t> encoding;
    std::vector<ColumnPage> pages;
    /// Column-level buffers (format 2.0 can keep a dictionary there); none in 2.1 files.
    std::vector<std::uint64_t> buffer_offsets;
    std::vector<std::uint64_t> buffer_sizes;
};

std::vector<std::uint8_t> encode_manifest(const Manifest& manifest);
bool decode_manifest(const std::vector<std::uint8_t>& bytes, Manifest& manifest);

bool decode_field(const std::vector<std::uint8_t>& bytes, Field& field);

std::vector<std::uint8_t> encode_file_descriptor(const FileDescriptor& descriptor);
bool decode_file_descriptor(const std::vector<std::uint8_t>& bytes, FileDescriptor& descriptor);

std::vector<std::uint8_t> encode_data_fragment(const DataFragment& fragment);
std::vector<std::uint8_t> encode_field(const Field& field);
bool decode_data_fragment(const std::vector<std::uint8_t>& bytes, DataFragment& fragment);

std::vector<std::uint8_t> encode_data_file(const DataFile& file);
bool decode_data_file(const std::vector<std::uint8_t>& bytes, DataFile& file);

std::vector<std::uint8_t> encode_metadata(const Metadata& metadata);
bool decode_metadata(const std::vector<std::uint8_t>& bytes, Metadata& metadata);

std::vector<std::uint8_t> encode_column_metadata(const ColumnMetadata& metadata);
bool decode_column_metadata(const std::vector<std::uint8_t>& bytes, ColumnMetadata& metadata);

}  // namespace nano_lance::pb
