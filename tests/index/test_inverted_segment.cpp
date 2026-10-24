// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The INVERTED index's part of a segment build (index/fts/inverted_segment.hpp): build_inverted_segment over
// one fragment of a two-fragment dataset, uncommitted (the version is unchanged, the entry handed back
// covers that fragment, its files are in _indices/<uuid>/ of the UUID asked for); load_inverted_params gives
// the analyzer it was built with and load_inverted_rows the documents' row addresses; a second segment over
// the other fragment folds an old segment's rows in. inverted_details encodes the analyzer as Lance's
// InvertedIndexDetails, with the granularity (field 14) only for list-element documents.

#include "index/fts/inverted_segment.hpp"

#include "index_test_util.hpp"

#include "nanolance/fts_search.hpp"
#include "nanolance/manifest_reader.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

using index_test::Column;
using index_test::require;
namespace ib = nano_lance::index_build;

namespace {

const std::vector<std::string> kDocs = {"alpha beta", "beta gamma", "", "gamma delta", "delta alpha"};

void fill(ArrowArray* a, std::size_t c, std::int64_t row) {
    if (c == 0) {
        require(ArrowArrayAppendInt(a, row) == NANOARROW_OK, "id");
    } else if (row == 2) {
        require(ArrowArrayAppendNull(a, 1) == NANOARROW_OK, "a null document");
    } else {
        index_test::append_string(a, kDocs[static_cast<std::size_t>(row)]);
    }
}

}  // namespace

int main() {
    const auto path = index_test::scratch("nano_lance_inverted_segment_test");
    const std::vector<Column> columns = {{"id", Column::Kind::Int64}, {"text", Column::Kind::String}};
    index_test::write_fragment(path, false, columns, 0, 3, fill);  // fragment 0: rows 0..2 (2 is null)
    index_test::write_fragment(path, true, columns, 3, 2, fill);   // fragment 1: rows 3..4

    std::string error;
    nano_lance::pb::Manifest manifest;
    std::uint64_t version = 0;
    require(nano_lance::load_latest_manifest(path, manifest, version, error), "manifest: " + error);

    nano_lance::InvertedIndexOptions options;
    options.name = "fts";
    options.params.with_position = true;
    options.params.stem = false;
    options.params.max_token_length = 20;

    // A segment over fragment 0, not committed.
    const std::vector<std::uint64_t> first = {0};
    std::array<std::uint8_t, 16> uuid{};
    uuid[0] = 0xAB;
    uuid[15] = 0x01;
    nano_lance::pb::IndexMetadata entry;
    ib::SegmentTarget target;
    target.manifest = &manifest;
    target.version = version;
    target.fragments = &first;
    target.out = &entry;
    target.uuid = &uuid;
    require(ib::build_inverted_segment(path, "text", options, target, {}, error), "segment: " + error);
    {
        nano_lance::pb::Manifest after;
        std::uint64_t latest = 0;
        require(nano_lance::load_latest_manifest(path, after, latest, error), "manifest: " + error);
        require(latest == version && after.indices.empty(), "a segment build commits nothing");
    }
    require(entry.uuid == uuid && entry.name == "fts", "the entry has the UUID and name asked for");
    require((entry.fragment_ids == std::vector<std::uint32_t>{0}), "the entry covers fragment 0 alone");
    require(entry.dataset_version == version, "built at the version given");
    require(nano_lance::is_inverted_index_url(entry.details_type_url), "INVERTED details");
    const auto dir = path / "_indices" / nano_lance::pb::uuid_string(uuid);
    require(std::filesystem::is_regular_file(dir / "metadata.lance"), "the segment's files are under its UUID");

    nano_lance::fts::AnalyzerParams params;
    require(ib::load_inverted_params(dir, params, error), "params: " + error);
    require(params.with_position && !params.stem && params.max_token_length == 20U &&
                params.base_tokenizer == "simple",
            "the analyzer it was built with");

    std::vector<std::uint64_t> rows;
    require(ib::load_inverted_rows(dir, rows, error), "rows: " + error);
    std::sort(rows.begin(), rows.end());
    require((rows == std::vector<std::uint64_t>{0, 1}), "the documents are rows 0 and 1 (row 2 is null)");

    // A second segment over fragment 1 that keeps the first one's documents (a merge).
    const std::vector<std::uint64_t> second = {1};
    const std::vector<std::uint64_t> coverage = {0, 1};
    nano_lance::pb::IndexMetadata merged;
    ib::SegmentTarget t2;
    t2.manifest = &manifest;
    t2.version = version;
    t2.fragments = &second;
    t2.coverage = &coverage;
    t2.out = &merged;
    ib::InvertedSegmentInputs inputs;
    inputs.rows = &rows;
    require(ib::build_inverted_segment(path, "text", options, t2, inputs, error), "merged segment: " + error);
    require((merged.fragment_ids == std::vector<std::uint32_t>{0, 1}), "the merged entry claims its coverage");
    require(merged.uuid != uuid, "a new random UUID when none is asked for");
    std::vector<std::uint64_t> merged_rows;
    require(ib::load_inverted_rows(path / "_indices" / nano_lance::pb::uuid_string(merged.uuid), merged_rows, error),
            "merged rows: " + error);
    std::sort(merged_rows.begin(), merged_rows.end());
    require((merged_rows == std::vector<std::uint64_t>{0, 1, 1ULL << 32U, (1ULL << 32U) | 1U}),
            "the old documents and fragment 1's");

    // A column that is not text, and one that does not exist.
    nano_lance::pb::IndexMetadata bad;
    target.out = &bad;
    target.uuid = nullptr;
    require(!ib::build_inverted_segment(path, "id", options, target, {}, error), "an int column is refused");
    require(!ib::build_inverted_segment(path, "missing", options, target, {}, error), "a missing column is refused");
    require(!ib::load_inverted_params(path / "_indices" / "nope", params, error), "no segment, no params");

    // InvertedIndexDetails: row granularity leaves field 14 out, list-element writes it.
    const auto row_details = ib::inverted_details(params);
    const auto element_details = ib::inverted_details(params, nano_lance::DocGranularity::ListElement);
    require(!row_details.empty(), "the details are written");
    require(nano_lance::inverted_details_granularity(row_details) == nano_lance::DocGranularity::Row,
            "row details read back as row");
    require(nano_lance::inverted_details_granularity(element_details) == nano_lance::DocGranularity::ListElement,
            "element details read back as list element");
    require(nano_lance::inverted_details_granularity(entry.details_value) == nano_lance::DocGranularity::Row,
            "the segment's own details are row granularity");

    std::filesystem::remove_all(path);
    std::cout << "inverted_segment: ok (uncommitted segments, params, rows, merge, details)\n";
    return 0;
}
