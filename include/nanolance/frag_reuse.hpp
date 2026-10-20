// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nano_lance {

/// Lance's fragment-reuse index (`__lance_frag_reuse`, written by a deferred compaction): how the rows a
/// compaction rewrote moved. Indexes built before it keep their old row addresses; this maps each old
/// address to where the row is now, or says the row is gone. Versions are applied oldest first, as Lance
/// does (lance-table system_index/frag_reuse.rs, lance-core utils/row_addr_remap.rs).
class FragReuse {
public:
    /// The fragment-reuse index of `manifest` (the newest one). `out` is null when there is none.
    static bool load(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                     std::shared_ptr<const FragReuse>& out, std::string& error);

    /// Maps the old address `address` through every version. False: the row was deleted by a compaction.
    /// True: `out` is its address now (`address` itself when no version moved it).
    bool resolve(std::uint64_t address, std::uint64_t& out) const;

    /// Whether any version rewrote rows of fragment `fragment` (an address in it may have moved).
    bool touches(std::uint32_t fragment) const;

    /// An index's fragment coverage as Lance reads it: each version's rewrite group that the coverage held
    /// whole swaps its old fragments for its new ones (a group it held only in part is dropped from it, so
    /// its rows are scanned). `ids` is ascending and distinct on return.
    void remap_coverage(std::vector<std::uint32_t>& ids) const;

    /// Whether the index holds no versions.
    bool empty() const { return steps_.empty(); }

    /// Decodes `details` (a FragmentReuseIndexDetails message) into a remap; `dataset_path` and `uuid` locate
    /// its external content, when it has any.
    static bool from_details(const std::filesystem::path& dataset_path, const pb::IndexMetadata& index,
                             std::shared_ptr<const FragReuse>& out, std::string& error);

    struct NewRange {
        std::uint32_t fragment = 0;
        std::uint64_t rows_before = 0;  // rewritten rows the group wrote before this fragment
        std::uint64_t physical = 0;
    };
    struct Group {
        std::vector<NewRange> new_ranges;  // the new fragments, in write order (empty ones dropped)
        std::vector<std::uint32_t> old_ids;  // the old fragments the group rewrote
        std::vector<std::uint32_t> new_ids;  // every new fragment it wrote
    };
    struct OldFragment {
        std::size_t group = 0;
        std::uint64_t rows_before = 0;  // surviving rows of the group's earlier old fragments
        std::uint64_t physical = 0;
        std::vector<std::uint32_t> kept;  // the offsets that survived, ascending
    };
    struct Step {  // one FRI version
        std::vector<Group> groups;
        std::map<std::uint32_t, OldFragment> frags;  // by old fragment id
    };

private:
    std::vector<Step> steps_;
};

/// One rewrite group of a deferred compaction, as the index records it.
struct FragReuseWriteGroup {
    /// The surviving rows of the old fragments, as old row addresses (fragment id << 32 | offset), ascending.
    std::vector<std::uint64_t> changed_row_addrs;
    /// Each old and new fragment: id, physical rows, deleted rows.
    std::vector<std::array<std::uint64_t, 3>> old_fragments;
    std::vector<std::array<std::uint64_t, 3>> new_fragments;
};

/// The `__lance_frag_reuse` index entry after a deferred compaction at `version`: `prior` (null: none yet)
/// with one more version of `groups` appended, covering `coverage` (ascending). Written inline, as Lance
/// writes it below 200 KB. False with `error` when `prior` keeps its versions outside the manifest.
bool fragment_reuse_index_entry(const pb::IndexMetadata* prior, std::uint64_t version,
                                const std::vector<FragReuseWriteGroup>& groups,
                                const std::vector<std::uint32_t>& coverage, pb::IndexMetadata& out,
                                std::string& error);

}  // namespace nano_lance
