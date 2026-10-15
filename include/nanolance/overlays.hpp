// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

/// Data overlay files (Lance's DataOverlayFile, `DataFragment.overlays` = field 11): files that supply
/// new values for some (row offset, field) cells of a fragment without rewriting its data files. A
/// cell's value is the newest overlay's that covers it (higher `committed_version`, then later in the
/// list), else the data file's; a deleted row's overlay values are dead. Within an overlay's value
/// column, a covered offset's value sits at its rank among the field's covered offsets.
namespace nano_lance {

struct DataOverlay {
    pb::DataFile data_file;
    /// Dense coverage: one offset set for every field of `data_file` (shared_offset_bitmap = 2).
    bool shared = true;
    std::vector<std::uint32_t> shared_offsets;              // ascending
    /// Sparse coverage: per field of `data_file.fields`, in order (field_coverage = 4).
    std::vector<std::vector<std::uint32_t>> field_offsets;  // each ascending
    std::uint64_t committed_version = 0;

    /// The offsets covered for `data_file.fields[index]`.
    const std::vector<std::uint32_t>& offsets_for(std::size_t index) const {
        return shared || index >= field_offsets.size() ? shared_offsets : field_offsets[index];
    }
};

/// Manifest reader / writer feature flag for overlays (DataOverlay files present).
constexpr std::uint64_t kFlagDataOverlays = 64U;

/// The overlays of `fragment`, in list order (oldest first). False and `error` on a malformed message.
bool read_fragment_overlays(const pb::DataFragment& fragment, std::vector<DataOverlay>& out, std::string& error);

/// Whether `fragment` has any overlay.
bool fragment_has_overlays(const pb::DataFragment& fragment);

/// The field ids `fragment`'s overlays committed after `version` touch (an index built at `version`
/// does not know their values).
bool fragment_overlaid_fields_after(const pb::DataFragment& fragment, std::uint64_t version,
                                    std::set<std::int32_t>& fields, std::string& error);

/// The fragments of `manifest` whose overlays, committed after `index_version`, touch one of `fields`:
/// an index built at that version has stale values for them, so it must not answer for them (they are
/// searched as if unindexed). Malformed overlays count as stale.
std::set<std::uint32_t> fragments_stale_for_index(const pb::Manifest& manifest, const std::vector<std::int32_t>& fields,
                                                  std::uint64_t index_version);

/// Append overlay messages (lance.table.DataOverlayFile, as a DataOverlay operation carries them) to
/// `fragment`, each stamped with `committed_version`. A sparse one needs one offset set per field;
/// offsets past the fragment's rows are kept (as Lance does: they never match a row).
bool append_fragment_overlays(pb::DataFragment& fragment, const std::vector<std::vector<std::uint8_t>>& overlays,
                              std::uint64_t committed_version, std::string& error);

}  // namespace nano_lance
