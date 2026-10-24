// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// index_segments.hpp: an index built one segment at a time and committed together. On a two-fragment
// dataset, a BTREE segment is built over each fragment (nothing committed), decoded and re-encoded, merged
// into one uncommitted segment, and the two committed as one index; then the segments are listed. Lance 12's
// refusals: an empty set, two segments with one UUID, overlapping coverage, an unknown index name.

#include "nanolance/index_segments.hpp"

#include "nanolance/dataset.hpp"

#include "support.hpp"

#include <set>
#include <string>
#include <vector>

using dataset_test::require;
namespace nl = nano_lance;

namespace {

std::vector<std::uint8_t> build(const std::filesystem::path& path, std::uint64_t fragment, const std::string& name) {
    nl::IndexSegmentBuild b;
    b.type = "BTREE";
    b.name = name;
    b.fragments = std::vector<std::uint64_t>{fragment};
    std::vector<std::uint8_t> segment;
    std::string error;
    nl::SegmentErrorKind kind = nl::SegmentErrorKind::None;
    require(nl::dataset_build_index_segment(path, "id", b, segment, error, kind), "build segment: " + error);
    return segment;
}

nl::IndexSegmentInfo decode(const std::vector<std::uint8_t>& message) {
    nl::IndexSegmentInfo info;
    std::string error;
    require(nl::decode_index_segment(message, info, error), "decode segment: " + error);
    return info;
}

}  // namespace

int main() {
    const auto path = dataset_test::scratch("nano_lance_index_segments_test");
    dataset_test::write_dataset(path, {{1, 2, 3}, {4, 5, 6}});
    std::string error;
    nl::SegmentErrorKind kind = nl::SegmentErrorKind::None;
    std::uint64_t before = 0;
    require(nl::dataset_latest_version(path, before, error), "latest");

    const auto s0 = build(path, 0, "id_idx");
    const auto s1 = build(path, 1, "id_idx");
    std::uint64_t still = 0;
    require(nl::dataset_latest_version(path, still, error) && still == before, "building a segment commits nothing");

    // A segment's message, field by field, and back.
    const auto i0 = decode(s0);
    const auto i1 = decode(s1);
    require(i0.name == "id_idx" && i0.has_fragment_ids && i0.fragment_ids == std::vector<std::uint32_t>{0},
            "segment 0 covers fragment 0");
    require(i1.fragment_ids == std::vector<std::uint32_t>{1}, "segment 1 covers fragment 1");
    require(i0.uuid != i1.uuid, "each segment has its own uuid");
    require(i0.details_type_url.find("BTree") != std::string::npos, "a BTree segment says so");
    require(!i0.files.empty(), "a segment lists its files");
    const auto again = decode(nl::encode_index_segment(i0));
    require(again.uuid == i0.uuid && again.fragment_ids == i0.fragment_ids && again.fields == i0.fields &&
                again.details_type_url == i0.details_type_url && again.files == i0.files,
            "encode then decode gives the segment back");

    // Merged: one new segment over both fragments, uncommitted.
    std::vector<std::uint8_t> merged;
    require(nl::dataset_merge_index_segments(path, {s0, s1}, merged, error, kind), "merge: " + error);
    const auto im = decode(merged);
    require(std::set<std::uint32_t>(im.fragment_ids.begin(), im.fragment_ids.end()) == std::set<std::uint32_t>{0, 1},
            "the merged segment covers both fragments");
    require(im.uuid != i0.uuid && im.uuid != i1.uuid, "under a new uuid");

    // Refusals.
    std::uint64_t v = 0;
    require(!nl::dataset_commit_index_segments(path, "id_idx", "id", {}, v, error, kind) &&
                kind == nl::SegmentErrorKind::InvalidArgument,
            "an empty set is refused");
    require(!nl::dataset_commit_index_segments(path, "id_idx", "id", {s0, s0}, v, error, kind) &&
                kind == nl::SegmentErrorKind::InvalidArgument,
            "a repeated uuid is refused");
    const auto s0b = build(path, 0, "id_idx");
    require(!nl::dataset_commit_index_segments(path, "id_idx", "id", {s0, s0b}, v, error, kind) &&
                kind == nl::SegmentErrorKind::InvalidArgument,
            "overlapping coverage is refused");
    require(nl::dataset_latest_version(path, still, error) && still == before, "a refused commit commits nothing");

    // Committed together: one index of two segments.
    require(nl::dataset_commit_index_segments(path, "id_idx", "id", {s0, s1}, v, error, kind), "commit: " + error);
    require(v == before + 1, "one new version");
    std::vector<std::array<std::uint8_t, 16>> uuids;
    require(nl::dataset_index_segments(path, 0, "id_idx", uuids, error, kind), "segments: " + error);
    require(uuids.size() == 2 && std::set<std::array<std::uint8_t, 16>>(uuids.begin(), uuids.end()) ==
                                     std::set<std::array<std::uint8_t, 16>>{i0.uuid, i1.uuid},
            "the index holds both segments");
    require(!nl::dataset_index_segments(path, 0, "nope", uuids, error, kind) && kind == nl::SegmentErrorKind::NotFound,
            "an unknown index is NotFound");

    // The committed index answers a filter: the rows are the filter's.
    const std::string filter = "id = 5";
    require(dataset_test::read_column(path, "id", &filter) == std::vector<std::int64_t>{5}, "an indexed filter");

    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::cout << "index_segments: OK\n";
    return 0;
}
