// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Internal: building one index segment over chosen fragments of a version, without committing it.
// The public builders (scalar_index.hpp, vector_search.hpp, fts_search.hpp) are this plus a commit;
// optimize_indices (index_maintenance.hpp) is this with the parameters -- and for a vector index the
// trained model -- of the segments it replaces. The kind-specific inputs are in index/scalar/scalar_segment.hpp,
// index/vector/vector_segment.hpp and index/fts/inverted_segment.hpp.

#include "lance_minimal.pb.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace nano_lance::index_build {

/// A build's progress, as Lance reports it (lance-c's LanceIndexBuildProgressCallback): `event` 0
/// starts `stage` (`total` units of `unit`, 0 / "" unknown), 1 reports `completed` units, 2 ends it.
using Progress = std::function<void(int event, const char* stage, std::uint64_t total, const char* unit,
                                    std::uint64_t completed)>;

/// Report a stage around `work` when `progress` is set.
template <typename F>
bool stage(const Progress* progress, const char* name, std::uint64_t total, const char* unit, F&& work) {
    if (progress != nullptr) {
        (*progress)(0, name, total, unit, 0);
    }
    const bool ok = work();
    if (ok && progress != nullptr) {
        (*progress)(2, name, 0, "", 0);
    }
    return ok;
}

/// Where a segment is built: on `manifest` (version `version`), over `fragments` (every fragment of
/// the version when null). The segment's files are written under _indices/<uuid>/ and its manifest
/// entry is returned in `out`; nothing is committed.
struct SegmentTarget {
    const pb::Manifest* manifest = nullptr;
    std::uint64_t version = 0;
    const std::vector<std::uint64_t>* fragments = nullptr;
    pb::IndexMetadata* out = nullptr;
    /// The fragments the entry claims; `fragments` when null.
    const std::vector<std::uint64_t>* coverage = nullptr;
    /// The segment's UUID (its directory under _indices/); a new random one when null.
    const std::array<std::uint8_t, 16>* uuid = nullptr;
    /// Where the build reports its stages; nothing when null.
    const Progress* progress = nullptr;
};

}  // namespace nano_lance::index_build
