// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Index segments: the physical pieces of a logical index. An index can be built one segment at a
// time -- on other machines, over disjoint fragments (index_build.hpp builds one without committing
// it) -- and the segments committed together, as Lance's commit_existing_index_segments does.

#include "nanolance/fts_search.hpp"
#include "nanolance/vector_search.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nano_lance {

/// What kind of failure a segment operation met, as Lance would report it.
enum class SegmentErrorKind {
    None,
    InvalidArgument,  // the segment set, or a call's arguments
    Index,            // the column is missing, or the name belongs to an index on another column
    NotFound,         // no such index
    Other,            // I/O and the like
};

/// Commit `segments` (each an encoded IndexMetadata message from an uncommitted build) as the
/// logical index `name` on `column`, in one new version. As Lance 12 does it:
///
/// - the set must be non-empty, with distinct UUIDs, disjoint fragment coverage, one index type,
///   every segment keyed on `column` and built at or before the current version; vector segments
///   that will coexist must agree on metric, dimension, sub-index and quantizer;
/// - coverage of fragments that no longer hold the column's data as the segment saw it is dropped;
/// - an existing segment of `name` whose fragments are all covered by the set is replaced, one
///   covering none of them is kept, one covered in part is an error; a different index type
///   replaces the whole index and must cover every fragment.
bool dataset_commit_index_segments(const std::filesystem::path& dataset_path, const std::string& name,
                                   const std::string& column, const std::vector<std::vector<std::uint8_t>>& segments,
                                   std::uint64_t& new_version, std::string& error, SegmentErrorKind& kind);

/// The segments of the index `name` at `version` (0: the latest), in manifest order. An unknown
/// name is an error (NotFound).
bool dataset_index_segments(const std::filesystem::path& dataset_path, std::uint64_t version, const std::string& name,
                            std::vector<std::array<std::uint8_t, 16>>& out, std::string& error,
                            SegmentErrorKind& kind);

/// One segment's IndexMetadata message, field by field.
struct IndexSegmentInfo {
    std::array<std::uint8_t, 16> uuid{};
    std::string name;
    std::vector<std::int32_t> fields;
    std::uint64_t dataset_version = 0;
    bool has_fragment_ids = false;  // false: coverage unknown (a legacy segment)
    std::vector<std::uint32_t> fragment_ids;
    std::uint32_t index_version = 0;
    std::uint64_t created_at = 0;  // milliseconds since the epoch; 0 unset
    std::string details_type_url;  // e.g. "/lance.table.BTreeIndexDetails"; empty: no details
    std::vector<std::uint8_t> details_value;
    std::vector<std::pair<std::string, std::uint64_t>> files;  // relative to _indices/<uuid>/, with sizes
};

bool decode_index_segment(const std::vector<std::uint8_t>& message, IndexSegmentInfo& out, std::string& error);
std::vector<std::uint8_t> encode_index_segment(const IndexSegmentInfo& segment);

/// The index details of segment `uuid` (on `column`), from its files, as Lance infers them for a
/// segment committed without: BTree, Bitmap, LabelList (a bitmap over a list column), Inverted (with
/// its analyzer settings) or Vector.
bool infer_index_segment_details(const std::filesystem::path& dataset_path, const std::string& column,
                                 const std::array<std::uint8_t, 16>& uuid, std::string& type_url,
                                 std::vector<std::uint8_t>& value, std::string& error);

/// One segment to build over chosen fragments of a version, uncommitted (Lance's
/// create_index_uncommitted).
struct IndexSegmentBuild {
    /// BTREE, BITMAP, LABEL_LIST, INVERTED, IVF_FLAT, IVF_PQ or IVF_HNSW_SQ.
    std::string type;
    /// Empty: Lance's default, <column>_idx (then _2, _3, ... past other indexes of that name).
    std::string name;
    std::uint64_t version = 0;  // 0: the latest
    std::optional<std::vector<std::uint64_t>> fragments;  // none: every fragment
    std::optional<std::array<std::uint8_t, 16>> uuid;      // none: a random one
    InvertedIndexOptions inverted;                         // INVERTED: the analyzer
    VectorIndexOptions vector;                             // vector types: the parameters
    /// A vector index: a model trained elsewhere -- IVF centroids [partitions][dim], and for
    /// IVF_PQ the codebook, Lance's flat [num_sub_vectors][2^num_bits][dim / num_sub_vectors].
    std::vector<float> ivf_centroids;
    std::vector<float> pq_codebook;
};

/// Build one segment and return its encoded IndexMetadata; its files are under _indices/<uuid>/
/// and nothing is committed. As Lance 12: the name must not belong to an index already in the
/// version, and BTREE and LABEL_LIST segments take no caller UUID.
bool dataset_build_index_segment(const std::filesystem::path& dataset_path, const std::string& column,
                                 const IndexSegmentBuild& build, std::vector<std::uint8_t>& segment,
                                 std::string& error, SegmentErrorKind& kind);

/// Merge uncommitted segments of one index type into one, uncommitted (Lance's
/// merge_existing_index_segments): a new segment, under a new UUID, over the union of their
/// fragments at the oldest of their versions. Vector segments must share their model (IVF centroids
/// and quantizer); a single vector segment is returned as it is.
bool dataset_merge_index_segments(const std::filesystem::path& dataset_path,
                                  const std::vector<std::vector<std::uint8_t>>& segments,
                                  std::vector<std::uint8_t>& merged, std::string& error, SegmentErrorKind& kind);

/// A trained IVF (and PQ) model (Lance's IndicesBuilder.train_ivf / train_pq).
struct TrainedVectorModel {
    std::size_t dim = 0;
    std::size_t partitions = 0;
    std::vector<float> centroids;  // [partitions][dim]
    std::vector<float> codebook;   // IVF_PQ: [num_sub_vectors][2^num_bits][dim / num_sub_vectors]
};

/// Train IVF centroids (`options.type` IVF_FLAT), or with `ivf_centroids` given a PQ codebook on
/// their residuals (IVF_PQ), from the vectors of `fragments` (all when null) of `version` (0 the
/// latest). Float16, float32 and float64 vectors.
bool dataset_train_vector_model(const std::filesystem::path& dataset_path, std::uint64_t version,
                                const std::string& column, const VectorIndexOptions& options,
                                const std::vector<std::uint64_t>* fragments, const std::vector<float>* ivf_centroids,
                                TrainedVectorModel& out, std::string& error);

}  // namespace nano_lance
