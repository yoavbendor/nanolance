// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Data overlays: files that supply new values for some cells of a fragment without rewriting its data
// files. Which overlay covers a cell, which fields a commit touched, and which fragments an index must
// stop answering for are all decided here from the overlay messages, so each rule is pinned on messages
// built by hand (dense and sparse coverage, a restamped version, malformed input).

#include "lance_minimal.pb.hpp"
#include "nanolance/overlays.hpp"
#include "nanolance/roaring_bitmap.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace pb = nano_lance::pb;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;

void varint(Bytes& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void key(Bytes& out, std::uint32_t field, std::uint32_t wire) {
    varint(out, (static_cast<std::uint64_t>(field) << 3U) | wire);
}

void bytes_field(Bytes& out, std::uint32_t field, const Bytes& value) {
    key(out, field, 2);
    varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void uint_field(Bytes& out, std::uint32_t field, std::uint64_t value) {
    key(out, field, 0);
    varint(out, value);
}

// A DataOverlayFile: its data file (field 1), then either one shared offset bitmap (field 2) or one
// FieldCoverage (field 4) holding a bitmap per field, then the committed version (field 3).
Bytes overlay_message(const pb::DataFile& file, const std::vector<std::vector<std::uint32_t>>& offsets, bool sparse,
                      std::uint64_t committed_version) {
    Bytes msg;
    bytes_field(msg, 1, pb::encode_data_file(file));
    if (!sparse) {
        bytes_field(msg, 2, nano_lance::roaring::encode(offsets.front()));
    } else {
        Bytes coverage;
        for (const auto& per_field : offsets) {
            bytes_field(coverage, 1, nano_lance::roaring::encode(per_field));
        }
        bytes_field(msg, 4, coverage);
    }
    uint_field(msg, 3, committed_version);
    return msg;
}

pb::DataFile data_file(const std::string& path, std::vector<std::int32_t> fields) {
    pb::DataFile file;
    file.path = path;
    file.fields = std::move(fields);
    return file;
}

}  // namespace

void require(bool ok, const std::string& msg, const std::string& detail) {
    require(ok, msg + detail);
}

int main() {
    // A fragment with no overlays: none, and reading them gives none.
    {
        pb::DataFragment plain;
        plain.id = 0;
        require(!nano_lance::fragment_has_overlays(plain), "a plain fragment has no overlays");
        std::vector<nano_lance::DataOverlay> overlays;
        std::string error;
        require(nano_lance::read_fragment_overlays(plain, overlays, error) && overlays.empty(),
                "a plain fragment reads no overlays");
    }

    // A dense overlay: one offset set shared by every field, restamped with the commit's version.
    pb::DataFragment fragment;
    fragment.id = 1;
    {
        const auto message = overlay_message(data_file("ov1.lance", {1, 2}), {{0, 3}}, false, 1);
        std::string error;
        require(nano_lance::append_fragment_overlays(fragment, {message}, 5, error), "append dense: ", error);
        require(nano_lance::fragment_has_overlays(fragment), "the fragment has an overlay now");

        std::vector<nano_lance::DataOverlay> overlays;
        require(nano_lance::read_fragment_overlays(fragment, overlays, error), "read dense: ", error);
        require(overlays.size() == 1, "one overlay was appended");
        const auto& o = overlays.front();
        require(o.shared, "a dense overlay is shared");
        require(o.committed_version == 5, "the commit's version replaces the message's (1)");
        require(o.data_file.path == "ov1.lance" && o.data_file.fields == std::vector<std::int32_t>({1, 2}),
                "the overlay's data file is read back");
        const std::vector<std::uint32_t> both = {0, 3};
        require(o.offsets_for(0) == both && o.offsets_for(1) == both, "a shared overlay covers every field alike");
    }

    // A sparse overlay: a set of offsets per field, and a newer version.
    {
        const auto message = overlay_message(data_file("ov2.lance", {3}), {{1, 6}}, true, 1);
        std::string error;
        require(nano_lance::append_fragment_overlays(fragment, {message}, 9, error), "append sparse: ", error);

        std::vector<nano_lance::DataOverlay> overlays;
        require(nano_lance::read_fragment_overlays(fragment, overlays, error), "read sparse: ", error);
        require(overlays.size() == 2, "overlays come back in list order (oldest first)");
        const auto& sparse = overlays[1];
        require(!sparse.shared, "a field-coverage overlay is sparse");
        require(sparse.committed_version == 9, "the sparse overlay is stamped with its commit's version");
        const std::vector<std::uint32_t> covered = {1, 6};
        require(sparse.offsets_for(0) == covered, "a sparse overlay's offsets for its field");
    }

    // Which fields the fragment's overlays touch after a version: those committed later only.
    {
        std::set<std::int32_t> fields;
        std::string error;
        require(nano_lance::fragment_overlaid_fields_after(fragment, 4, fields, error), "fields after 4: ", error);
        require(fields == std::set<std::int32_t>({1, 2, 3}), "every overlay committed after version 4 counts");
        require(nano_lance::fragment_overlaid_fields_after(fragment, 5, fields, error), "fields after 5: ", error);
        require(fields == std::set<std::int32_t>({3}), "only the overlay committed after version 5 counts");
        require(nano_lance::fragment_overlaid_fields_after(fragment, 9, fields, error), "fields after 9: ", error);
        require(fields.empty(), "nothing is committed after the newest overlay");
    }

    // Stale fragments of an index: those whose overlays after the index's version touch one of its fields.
    {
        pb::Manifest manifest;

        pb::DataFragment zero;  // an overlay on field 3, committed at 9
        zero.id = 0;
        std::string error;
        require(nano_lance::append_fragment_overlays(
                    zero, {overlay_message(data_file("z.lance", {3}), {{2}}, false, 1)}, 9, error),
                "append for fragment 0: ", error);

        pb::DataFragment one;  // no overlays
        one.id = 1;

        pb::DataFragment two;  // an overlay on fields 1 and 2, committed at 5
        two.id = 2;
        require(nano_lance::append_fragment_overlays(
                    two, {overlay_message(data_file("t.lance", {1, 2}), {{0}}, false, 1)}, 5, error),
                "append for fragment 2: ", error);

        pb::DataFragment broken;  // an overlay message that does not decode: counts as stale
        broken.id = 3;
        bytes_field(broken.unknown, 11, Bytes{0xFF, 0xFF});

        manifest.fragments = {zero, one, two, broken};

        require(nano_lance::fragments_stale_for_index(manifest, {3}, 6) == std::set<std::uint32_t>({0, 3}),
                "fragment 0 changed field 3 after version 6 (and 3 is malformed)");
        require(nano_lance::fragments_stale_for_index(manifest, {1}, 4) == std::set<std::uint32_t>({2, 3}),
                "fragment 2 changed field 1 after version 4");
        require(nano_lance::fragments_stale_for_index(manifest, {1}, 5) == std::set<std::uint32_t>({3}),
                "an overlay committed AT the index's version is not after it");
        require(nano_lance::fragments_stale_for_index(manifest, {2}, 9) == std::set<std::uint32_t>({3}),
                "a field no overlay touched after the index is not stale");
    }

    // Refused: a sparse overlay without one offset set per field, and a message with no data file.
    {
        pb::DataFragment target;
        target.id = 4;
        std::string error;
        const auto mismatched = overlay_message(data_file("m.lance", {1, 2}), {{0}}, true, 1);
        require(!nano_lance::append_fragment_overlays(target, {mismatched}, 3, error), "a sparse overlay needs its sets");
        require(error.find("needs one offset set per field") != std::string::npos, "the refusal says so: ", error);
        require(target.unknown.empty(), "a refused overlay is not appended");

        Bytes no_file;
        bytes_field(no_file, 2, nano_lance::roaring::encode({0}));
        require(!nano_lance::append_fragment_overlays(target, {no_file}, 3, error), "an overlay with no data file");
        require(error == "malformed data overlay", "the malformed overlay error: ", error);
    }

    std::cout << "overlays: dense and sparse coverage, versions, stale fragments, malformed input OK\n";
    return 0;
}
