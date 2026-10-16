// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/overlays.hpp"

#include "nanolance/roaring_bitmap.hpp"
#include "proto_wire.hpp"

#include <algorithm>

namespace nano_lance {

namespace {

using namespace proto_wire;

constexpr std::uint32_t kOverlaysField = 11;  // DataFragment.overlays
// DataOverlayFile
constexpr std::uint32_t kDataFileField = 1;
constexpr std::uint32_t kSharedBitmapField = 2;
constexpr std::uint32_t kCommittedVersionField = 3;
constexpr std::uint32_t kFieldCoverageField = 4;
// FieldCoverage
constexpr std::uint32_t kOffsetBitmapsField = 1;

bool decode_overlay(const std::uint8_t* data, std::size_t size, DataOverlay& out, std::string& error) {
    out = DataOverlay{};
    bool has_file = false;
    Reader r(data, size);
    Field f;
    while (r.next(f)) {
        if (f.number == kDataFileField && f.wire == 2) {
            if (!pb::decode_data_file(std::vector<std::uint8_t>(f.data, f.data + f.size), out.data_file)) {
                error = "malformed data overlay: its data file";
                return false;
            }
            has_file = true;
        } else if (f.number == kSharedBitmapField && f.wire == 2) {
            out.shared = true;
            if (!roaring::decode(f.data, f.size, out.shared_offsets, error)) {
                return false;
            }
        } else if (f.number == kCommittedVersionField && f.wire == 0) {
            out.committed_version = f.value;
        } else if (f.number == kFieldCoverageField && f.wire == 2) {
            out.shared = false;
            out.field_offsets.clear();
            Reader c(f.data, f.size);
            Field g;
            while (c.next(g)) {
                if (g.number == kOffsetBitmapsField && g.wire == 2) {
                    out.field_offsets.emplace_back();
                    if (!roaring::decode(g.data, g.size, out.field_offsets.back(), error)) {
                        return false;
                    }
                }
            }
            if (!c.ok()) {
                error = "malformed data overlay field coverage";
                return false;
            }
        }
    }
    if (!r.ok() || !has_file) {
        error = "malformed data overlay";
        return false;
    }
    return true;
}

}  // namespace

bool read_fragment_overlays(const pb::DataFragment& fragment, std::vector<DataOverlay>& out, std::string& error) {
    out.clear();
    Reader r(fragment.unknown.data(), fragment.unknown.size());
    Field f;
    while (r.next(f)) {
        if (f.number == kOverlaysField && f.wire == 2) {
            out.emplace_back();
            if (!decode_overlay(f.data, f.size, out.back(), error)) {
                return false;
            }
        }
    }
    if (!r.ok()) {
        error = "malformed fragment metadata";
        return false;
    }
    return true;
}

bool fragment_has_overlays(const pb::DataFragment& fragment) {
    Reader r(fragment.unknown.data(), fragment.unknown.size());
    Field f;
    while (r.next(f)) {
        if (f.number == kOverlaysField) {
            return true;
        }
    }
    return false;
}

bool fragment_overlaid_fields_after(const pb::DataFragment& fragment, std::uint64_t version,
                                    std::set<std::int32_t>& fields, std::string& error) {
    fields.clear();
    if (!fragment_has_overlays(fragment)) {
        return true;
    }
    std::vector<DataOverlay> overlays;
    if (!read_fragment_overlays(fragment, overlays, error)) {
        return false;
    }
    for (const auto& overlay : overlays) {
        if (overlay.committed_version > version) {
            fields.insert(overlay.data_file.fields.begin(), overlay.data_file.fields.end());
        }
    }
    return true;
}

std::set<std::uint32_t> fragments_stale_for_index(const pb::Manifest& manifest, const std::vector<std::int32_t>& fields,
                                                  std::uint64_t index_version) {
    std::set<std::uint32_t> out;
    for (const auto& fragment : manifest.fragments) {
        if (!fragment_has_overlays(fragment)) {
            continue;
        }
        std::set<std::int32_t> touched;
        std::string error;
        if (!fragment_overlaid_fields_after(fragment, index_version, touched, error) ||
            std::any_of(fields.begin(), fields.end(), [&](std::int32_t f) { return touched.count(f) != 0U; })) {
            out.insert(static_cast<std::uint32_t>(fragment.id));
        }
    }
    return out;
}

bool append_fragment_overlays(pb::DataFragment& fragment, const std::vector<std::vector<std::uint8_t>>& overlays,
                              std::uint64_t committed_version, std::string& error) {
    for (const auto& message : overlays) {
        DataOverlay overlay;
        if (!decode_overlay(message.data(), message.size(), overlay, error)) {
            return false;
        }
        if (!overlay.shared && overlay.field_offsets.size() != overlay.data_file.fields.size()) {
            error = "Invalid user input: a sparse data overlay needs one offset set per field (" +
                    std::to_string(overlay.data_file.fields.size()) + " fields, " +
                    std::to_string(overlay.field_offsets.size()) + " offset sets)";
            return false;
        }
        // The message as given, its committed_version replaced by the commit's.
        std::vector<std::uint8_t> stamped;
        Reader r(message.data(), message.size());
        Field f;
        while (r.next(f)) {
            if (f.number != kCommittedVersionField) {
                stamped.insert(stamped.end(), f.begin, f.end);
            }
        }
        put_u64_field(stamped, kCommittedVersionField, committed_version);
        put_bytes_field(fragment.unknown, kOverlaysField, stamped);
    }
    return true;
}

}  // namespace nano_lance
