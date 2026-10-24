// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/index_maintenance.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_set>

namespace nano_lance {
namespace {

constexpr const char* kFragReuseIndex = "__lance_frag_reuse";
constexpr const char* kMemWalIndex = "__lance_mem_wal";

}  // namespace

bool is_system_index(const pb::IndexMetadata& index) {
    return index.name == kFragReuseIndex || index.name == kMemWalIndex;
}

void retain_relevant_indices(std::vector<pb::IndexMetadata>& indices, const std::vector<pb::Field>& fields,
                             const std::vector<pb::DataFragment>& fragments) {
    std::unordered_set<std::int32_t> field_ids;
    for (const auto& f : fields) {
        field_ids.insert(f.id);
    }
    indices.erase(std::remove_if(indices.begin(), indices.end(),
                                 [&](const pb::IndexMetadata& index) {
                                     return !is_system_index(index) &&
                                            !std::all_of(index.fields.begin(), index.fields.end(),
                                                         [&](std::int32_t id) { return field_ids.count(id) != 0U; });
                                 }),
                  indices.end());

    std::set<std::uint32_t> existing;
    for (const auto& f : fragments) {
        existing.insert(static_cast<std::uint32_t>(f.id));
    }
    const auto covers_nothing = [&](const pb::IndexMetadata& index) {
        return std::none_of(index.fragment_ids.begin(), index.fragment_ids.end(),
                            [&](std::uint32_t id) { return existing.count(id) != 0U; });
    };
    std::map<std::string, std::vector<std::size_t>> by_name;  // segments with a known coverage
    std::vector<bool> keep(indices.size(), false);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        // Unknown coverage is not empty coverage; and the fragment-reuse index is Lance's own.
        if (indices[i].name == kFragReuseIndex || !indices[i].has_fragment_bitmap) {
            keep[i] = true;
        } else {
            by_name[indices[i].name].push_back(i);
        }
    }
    for (const auto& [name, segments] : by_name) {
        if (segments.size() == 1U) {
            keep[segments.front()] = true;
            continue;
        }
        bool any = false;
        for (const auto i : segments) {
            if (!covers_nothing(indices[i])) {
                keep[i] = true;
                any = true;
            }
        }
        if (!any) {
            const auto oldest = *std::min_element(segments.begin(), segments.end(), [&](std::size_t a, std::size_t b) {
                return indices[a].dataset_version < indices[b].dataset_version;
            });
            keep[oldest] = true;
        }
    }
    std::vector<pb::IndexMetadata> kept;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (keep[i]) {
            kept.push_back(std::move(indices[i]));
        }
    }
    indices = std::move(kept);
}

void drop_fragments_from_indices(std::vector<pb::IndexMetadata>& indices,
                                 const std::vector<std::uint64_t>& fragment_ids) {
    const std::set<std::uint64_t> gone(fragment_ids.begin(), fragment_ids.end());
    for (auto& index : indices) {
        if (!index.has_fragment_bitmap) {
            continue;
        }
        const auto before = index.fragment_ids.size();
        index.fragment_ids.erase(std::remove_if(index.fragment_ids.begin(), index.fragment_ids.end(),
                                                [&](std::uint32_t id) { return gone.count(id) != 0U; }),
                                 index.fragment_ids.end());
        if (index.fragment_ids.size() != before) {
            index.fragment_bitmap_changed = true;
        }
    }
}

std::uint64_t first_fragment_id_after_indices(const std::vector<pb::IndexMetadata>& indices) {
    std::uint64_t next = 0;
    for (const auto& index : indices) {
        if (!index.fragment_ids.empty()) {
            next = std::max<std::uint64_t>(next, static_cast<std::uint64_t>(index.fragment_ids.back()) + 1U);
        }
    }
    return next;
}

}  // namespace nano_lance
