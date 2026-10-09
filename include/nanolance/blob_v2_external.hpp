// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "nanolance/column_values.hpp"
#include "nanolance/schema_mapper.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <vector>

struct ArrowArray;
struct ArrowSchema;
struct ArrowArrayView;

namespace nano_lance {

// ---- Columnar encapsulation of the lance.blob.v2 external reference column ----------------------------
// A blob.v2 reference is, semantically, just (position, size, uri). Its Arrow *representation* — a struct
// with children data/uri/position/size, `data` nulled for external rows, the lance.blob.v2 extension tag,
// the child order — is a Lance implementation detail and belongs here, not in the producer. The producer
// fills (position, size) as a soatins `soa<{position,size}, N>` and hands the two columns + the shared URI
// to the builder; nanolance owns the rest. The view is the read-side twin: it resolves the children once
// so callers never index `children[k]` or look up "position"/"size"/"uri" by name.

/// Build the external-only `payload_ref` blob.v2 struct array from parallel position/size columns sharing
/// one URI (the per-window external-file case). `n` rows; `shared_uri` is repeated to every row.
bool build_blob_v2_external_array(const std::uint64_t* positions, const std::uint64_t* sizes, std::size_t n,
                                  const char* shared_uri, ArrowArray& out_array, std::string& error);

/// Non-owning read view over a read-back `payload_ref` blob.v2 struct (the struct's schema + array view).
/// Resolves position/size/uri by name on init(); accessors then read row values with no per-call lookup.
class BlobV2ColumnView {
public:
    bool init(const ArrowSchema& payload_ref_schema, const ArrowArrayView& payload_ref_view,
              std::string& error);
    std::int64_t size() const { return len_; }
    std::uint64_t position(std::int64_t row) const;
    std::uint64_t byte_size(std::int64_t row) const;
    void uri(std::int64_t row, const char** data, std::int64_t* size) const;

private:
    const ArrowArrayView* pos_ = nullptr;
    const ArrowArrayView* size_ = nullptr;
    const ArrowArrayView* uri_ = nullptr;
    std::int64_t len_ = 0;
};

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

/// One row of a legacy (v1) blob column -- (position, size) of bytes in `out.blob_v2.data_file` --
/// appended as a descriptor row of kind inline, so every blob read shape serves it. `null` marks
/// the row null (format 2.1's reading of a non-zero position with no size).
void blob_append_legacy_row(ColumnValues& out, std::uint64_t position, std::uint64_t size, bool null);

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

/// Lance's Blob v2 storage kinds (the descriptor's `kind`).
constexpr std::uint8_t kBlobKindInline = 0;     // in the data file, at `position`
constexpr std::uint8_t kBlobKindPacked = 1;     // in a sidecar file shared with other blobs, at `position`
constexpr std::uint8_t kBlobKindDedicated = 2;  // a sidecar file of its own
constexpr std::uint8_t kBlobKindExternal = 3;   // at `blob_uri`, `position`
/// Writer-side only, never on disk: a blob given as bytes, placed (inline, packed or dedicated) when
/// its data file is written. The row's trailing bytes -- a URI's place -- are the blob itself.
constexpr std::uint8_t kBlobKindPendingData = 0xFF;

/// Lance's storage thresholds for a Blob v2 field, from its field metadata
/// (`lance-encoding:blob-{inline,dedicated,pack-file}-size-threshold`): a blob of more than `inline_max`
/// bytes goes to a packed sidecar, of more than `dedicated_above` to a sidecar of its own, and a
/// packed sidecar holds at most `pack_file_max` bytes.
struct BlobV2Thresholds {
    std::uint64_t inline_max = 64U * 1024U;
    std::uint64_t dedicated_above = 4U * 1024U * 1024U;
    std::uint64_t pack_file_max = 1024U * 1024U * 1024U;
};

/// The field's thresholds, refused as Lance refuses them (not a number; zero where zero is not allowed).
bool blob_v2_thresholds(const LanceField& field, BlobV2Thresholds& out, std::string& error);

/// Whether `values` holds a blob given as bytes, still to be placed.
bool blob_v2_has_pending(const ColumnValues& values);

/// Places a data file's pending blobs, as Lance's blob preprocessor does: each inline blob's bytes are
/// written to `out` (the data file, before any column) at a 64-byte boundary, and packed and dedicated
/// ones to sidecar files beside it, ids counting from 1 per data file. One per data file.
class BlobV2Placer {
public:
    BlobV2Placer(std::filesystem::path data_file, std::uint64_t pack_file_override)
        : data_file_(std::move(data_file)), pack_override_(pack_file_override) {}
    /// `values` with every pending row replaced by its stored descriptor.
    bool place(const ColumnValues& values, const BlobV2Thresholds& thresholds, std::ostream& out,
               ColumnValues& placed, std::string& error);
    /// Closes the open packed sidecar, if any.
    bool finish(std::string& error);

private:
    bool write_sidecar(std::uint32_t id, const std::uint8_t* data, std::uint64_t n, std::string& error);
    std::filesystem::path data_file_;
    std::uint64_t pack_override_ = 0;
    std::uint32_t next_id_ = 1;
    std::uint32_t pack_id_ = 0;  // 0: no pack open
    std::uint64_t pack_size_ = 0;
    std::uint64_t pack_max_ = 0;
    std::ofstream pack_;
};

/// A Blob v2 descriptor page's PageLayout (the inner ColumnEncoding message): Lance's FullZip of
/// variable-width packed descriptors, with a definition level per row when the page has nulls.
std::vector<std::uint8_t> blob_v2_descriptor_page_encoding(std::uint64_t rows, bool nullable);

/// Where a blob's bytes are: a local file or an external URI, and the range within it.
struct BlobV2Location {
    std::string file;  // local path, or the URI for an external blob
    bool external = false;
    std::uint64_t position = 0;
    std::uint64_t size = 0;
};

/// The sidecar file Lance keeps packed and dedicated blobs in, beside `data_file`:
/// <data dir>/<data file stem>/<blob id, bit-reversed, as 32 binary digits>.blob.
std::filesystem::path blob_v2_sidecar_path(const std::filesystem::path& data_file, std::uint32_t blob_id);

/// Resolve a descriptor read from `data_file` to where its bytes are.
bool blob_v2_locate(const BlobV2ExternalDescriptor& descriptor, const std::filesystem::path& data_file,
                    BlobV2Location& out, std::string& error);

/// Read `length` bytes of the blob at `location`, starting `offset` bytes into it.
bool blob_v2_read(const BlobV2Location& location, std::uint64_t offset, std::uint64_t length,
                  std::vector<std::uint8_t>& out, std::string& error);

/// Find top-level `lance.blob.v2` extension struct in a mapped schema, if present.
const LanceField* find_blob_v2_parent(const LanceSchemaMapping& mapping);

/// The ids of every top-level `lance.blob.v2` struct, in schema order.
std::vector<std::int32_t> blob_v2_parent_ids(const LanceSchemaMapping& mapping);

}  // namespace nano_lance
