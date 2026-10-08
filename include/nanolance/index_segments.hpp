// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Index segments: the physical pieces of a logical index. An index can be built one segment at a
// time -- on other machines, over disjoint fragments (index_build.hpp builds one without committing
// it) -- and the segments committed together, as Lance's commit_existing_index_segments does.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
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

}  // namespace nano_lance
