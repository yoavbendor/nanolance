#pragma once

#include "nanolance/column_values.hpp"
#include "nanolance/schema_mapper.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct ArrowArray;

namespace nano_lance {

/// Materialized external blob v2 descriptor (`kind = 3`) before Lance packed encoding.
struct BlobV2ExternalDescriptor {
    std::uint8_t kind = 3;
    std::uint64_t position = 0;
    std::uint64_t size = 0;
    std::uint32_t blob_id = 0;
    std::string blob_uri;
};

/// Validate one write-side blob row (`data`, `uri`, `position`, `size`) as external-only.
bool preprocess_blob_v2_external_row(const ArrowArray& data,
                                     const ArrowArray& uri,
                                     const ArrowArray& position,
                                     const ArrowArray& size,
                                     std::int64_t row_index,
                                     BlobV2ExternalDescriptor& out,
                                     std::string& error);

/// Pack one external descriptor into the Lance 2.2 blob v2 per-row record bytes.
std::vector<std::uint8_t> blob_v2_pack_descriptor_row(const BlobV2ExternalDescriptor& descriptor);

/// Unpack one per-row record (inverse of `blob_v2_pack_descriptor_row`) for tests.
bool blob_v2_unpack_descriptor_row(const std::vector<std::uint8_t>& row_bytes,
                                     BlobV2ExternalDescriptor& out,
                                     std::string& error);

/// Rewrite `mapping` in-place for Lance file/manifest descriptors: one physical `lance.blob.v2`
/// column with materialized children (kind, position, size, blob_id, blob_uri). Call only after
/// the last `append_batch_column_values` (e.g. at commit); Arrow ingest layout is no longer read.
bool finalize_blob_v2_schema_for_write(LanceSchemaMapping& mapping, std::string& error);

/// Manifest/field metadata key holding the newline-joined URI dictionary (nanolance extension).
/// Presence of this key marks a blob column as dictionary-encoded; readers resolve each row's
/// `uri` from `dictionary[blob_id]` instead of the (empty) inline URI. NOT readable by stock Lance.
constexpr const char* kBlobV2UriDictMetadataKey = "nanolance:blob_uri_dict";

/// Serialize / parse the URI dictionary for storage in field metadata (newline-joined).
std::string blob_v2_serialize_uri_dictionary(const std::vector<std::string>& dictionary);
std::vector<std::string> blob_v2_parse_uri_dictionary(const std::string& serialized);

/// Append packed external-only blob rows from the Arrow struct column for `blob_field`.
/// When `dictionary_mode` is true, distinct URIs are deduplicated into `out.blob_v2.uri_dictionary`
/// and each packed row stores `blob_id` = dictionary index with an empty inline URI (smaller, but
/// produces a nanolance-only layout). When false, URIs are stored inline per row (Lance-compatible).
bool append_blob_v2_batch_column_values(const ArrowArray& batch,
                                         const LanceSchemaMapping& mapping,
                                         const LanceField& blob_field,
                                         bool dictionary_mode,
                                         ColumnValues& out,
                                         std::string& error);

/// PageLayout encoding bytes for Lance 2.2 external blob v2 packed struct (matches Lance reference).
const std::vector<std::uint8_t>& blob_v2_page_layout_encoding();

/// `PageLayout` column encoding payload for `encode_direct_encoding` (strips outer protobuf wrapper).
const std::vector<std::uint8_t>& blob_v2_column_page_encoding();

/// Build Lance control buffer for packed external blob row boundaries.
std::vector<std::uint8_t> blob_v2_build_control_buffer(const std::vector<std::uint32_t>& row_packed_sizes);

/// Parse a Lance blob v2 control buffer into per-row packed sizes (`num_rows` must match the on-disk fragment row count).
bool blob_v2_control_buffer_to_row_sizes(const std::vector<std::uint8_t>& control, std::uint64_t num_rows,
                                           std::vector<std::uint32_t>& row_packed_sizes, std::string& error);

/// Find top-level `lance.blob.v2` extension struct in a mapped schema, if present.
const LanceField* find_blob_v2_parent(const LanceSchemaMapping& mapping);

}  // namespace nano_lance
