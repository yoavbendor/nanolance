// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The scalar index's part of a segment build (index/scalar/scalar_segment.hpp): uncommitted BTREE and NGRAM
// segments over one fragment of a two-fragment dataset (the version unchanged, the entry handed back with
// the fragment, details type and files of Lance's layout); load_ngram_postings reads the NGRAM segment's
// posting lists back (each trigram of the lower-cased values to its rows, token 0 the null rows), and a
// segment over the other fragment merges an old segment's postings in.

#include "index/scalar/scalar_segment.hpp"

#include "index_test_util.hpp"

#include "nanolance/fts_tokenizer.hpp"
#include "nanolance/manifest_reader.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using index_test::Column;
using index_test::require;
namespace ib = nano_lance::index_build;

namespace {

const std::vector<std::string> kValues = {"Hello", "yellow", "", "hello world", "mellow"};

void fill(ArrowArray* a, std::size_t c, std::int64_t row) {
    if (c == 0) {
        require(ArrowArrayAppendInt(a, row * 10) == NANOARROW_OK, "id");
    } else if (row == 2) {
        require(ArrowArrayAppendNull(a, 1) == NANOARROW_OK, "null");
    } else {
        index_test::append_string(a, kValues[static_cast<std::size_t>(row)]);
    }
}

std::set<std::string> file_names(const nano_lance::pb::IndexMetadata& e) {
    std::set<std::string> out;
    for (const auto& f : e.files) {
        out.insert(f.path);
    }
    return out;
}

}  // namespace

int main() {
    const auto path = index_test::scratch("nano_lance_scalar_segment_test");
    const std::vector<Column> columns = {{"id", Column::Kind::Int64}, {"s", Column::Kind::String}};
    index_test::write_fragment(path, false, columns, 0, 3, fill);
    index_test::write_fragment(path, true, columns, 3, 2, fill);

    std::string error;
    nano_lance::pb::Manifest manifest;
    std::uint64_t version = 0;
    require(nano_lance::load_latest_manifest(path, manifest, version, error), "manifest: " + error);
    const std::vector<std::uint64_t> first = {0};
    const std::vector<std::uint64_t> second = {1};

    // A BTREE segment over fragment 0.
    {
        nano_lance::pb::IndexMetadata entry;
        ib::SegmentTarget target;
        target.manifest = &manifest;
        target.version = version;
        target.fragments = &first;
        target.out = &entry;
        nano_lance::ScalarIndexOptions options;
        options.name = "id_bt";
        require(ib::build_scalar_segment(path, "id", nano_lance::ScalarIndexType::BTree, options, target, {}, error),
                "btree segment: " + error);
        require(entry.details_type_url == "/lance.table.BTreeIndexDetails", "BTree details");
        require((entry.fragment_ids == std::vector<std::uint32_t>{0}) && entry.name == "id_bt", "its coverage, name");
        require((file_names(entry) == std::set<std::string>{"page_lookup.lance", "page_data.lance"}),
                "Lance's two BTree files");
    }

    // An NGRAM segment over fragment 0, read back.
    ib::NGramPostings postings;
    {
        nano_lance::pb::IndexMetadata entry;
        ib::SegmentTarget target;
        target.manifest = &manifest;
        target.version = version;
        target.fragments = &first;
        target.out = &entry;
        nano_lance::ScalarIndexOptions options;
        require(ib::build_scalar_segment(path, "s", nano_lance::ScalarIndexType::NGram, options, target, {}, error),
                "ngram segment: " + error);
        require(entry.details_type_url == "/lance.table.NGramIndexDetails", "NGram details");
        require((file_names(entry) == std::set<std::string>{"ngram_postings.lance"}), "one postings file");
        const auto dir = path / "_indices" / nano_lance::pb::uuid_string(entry.uuid);
        require(ib::load_ngram_postings(dir, postings, error), "postings: " + error);

        // "Hello" and "yellow" share "ell" and "llo"; "hel" is only in row 0 (case folded).
        const auto token = [](const std::string& tri) {
            const auto t = nano_lance::fts::ngram_tokens(tri);
            require(t.size() == 1U, "one trigram token for " + tri);
            return t.front();
        };
        require((postings.at(token("ell")) == std::vector<std::uint64_t>{0, 1}), "'ell' is in rows 0 and 1");
        require((postings.at(token("hel")) == std::vector<std::uint64_t>{0}), "'hel' is in row 0, lower-cased");
        require(postings.count(token("wor")) == 0U, "fragment 1's trigrams are not in fragment 0's segment");
        require(postings.count(0U) == 1U && (postings.at(0U) == std::vector<std::uint64_t>{2}), "token 0: null row 2");
    }

    // A segment over fragment 1 merged with the old postings.
    {
        nano_lance::pb::IndexMetadata entry;
        const std::vector<std::uint64_t> coverage = {0, 1};
        ib::SegmentTarget target;
        target.manifest = &manifest;
        target.version = version;
        target.fragments = &second;
        target.coverage = &coverage;
        target.out = &entry;
        ib::ScalarSegmentInputs inputs;
        inputs.postings = &postings;
        nano_lance::ScalarIndexOptions options;
        require(ib::build_scalar_segment(path, "s", nano_lance::ScalarIndexType::NGram, options, target, inputs, error),
                "merged ngram segment: " + error);
        require((entry.fragment_ids == std::vector<std::uint32_t>{0, 1}), "the merged coverage");
        ib::NGramPostings merged;
        require(ib::load_ngram_postings(path / "_indices" / nano_lance::pb::uuid_string(entry.uuid), merged, error),
                "merged postings: " + error);
        const auto ell = nano_lance::fts::ngram_tokens("ell").front();
        const std::uint64_t f1 = 1ULL << 32U;
        require((merged.at(ell) == std::vector<std::uint64_t>{0, 1, f1, f1 | 1U}),
                "'ell' in both fragments' rows: hello world and mellow too");
    }

    // Nothing was committed.
    {
        nano_lance::pb::Manifest after;
        std::uint64_t latest = 0;
        require(nano_lance::load_latest_manifest(path, after, latest, error), "manifest: " + error);
        require(latest == version && after.indices.empty(), "segment builds commit nothing");
    }
    require(!ib::load_ngram_postings(path / "_indices" / "none", postings, error), "no segment, no postings");

    std::filesystem::remove_all(path);
    std::cout << "scalar_segment: ok (BTREE and NGRAM segments, postings, merge)\n";
    return 0;
}
