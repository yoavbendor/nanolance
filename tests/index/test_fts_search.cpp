// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Full-text search (nanolance/fts_search.hpp) over a small two-fragment dataset: an INVERTED index with
// positions built by dataset_create_inverted_index (its manifest entry and its files read back), then
// match (OR and AND), phrase, boolean and boost queries answered from it, a limit, a row appended after
// the build found by the flat search of the unindexed fragment, and parse_fts_query's JSON forms and
// refusals. Rows are checked by row address, scores only for order and sign.

#include "nanolance/fts_search.hpp"

#include "index_test_util.hpp"

#include "nanolance/manifest_reader.hpp"
#include "nanolance/scalar_index.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using index_test::Column;
using index_test::require;

namespace {

const std::vector<std::string> kDocs = {
    "the quick brown fox",            // 0
    "a lazy dog sleeps",              // 1
    "quick thinking saves the day",   // 2
    "brown bread and butter",         // 3
    "the fox and the dog are friends",// 4
    "nothing to see here",            // 5
};

const std::vector<Column> kColumns = {{"id", Column::Kind::Int64}, {"text", Column::Kind::String}};

void fill(ArrowArray* a, std::size_t c, std::int64_t row, const std::vector<std::string>& docs) {
    if (c == 0) {
        require(ArrowArrayAppendInt(a, row) == NANOARROW_OK, "id");
    } else {
        index_test::append_string(a, docs[static_cast<std::size_t>(row)]);
    }
}

// Row address of document `i` (fragment 0 holds 0..2, fragment 1 holds 3..5, fragment 2 the appended one).
std::uint64_t addr(std::uint64_t fragment, std::uint64_t offset) { return (fragment << 32U) | offset; }
std::uint64_t addr_of(std::size_t i) { return i < 3 ? addr(0, i) : (i < 6 ? addr(1, i - 3) : addr(2, i - 6)); }

std::set<std::uint64_t> rows_of(const std::vector<std::size_t>& docs) {
    std::set<std::uint64_t> out;
    for (const auto d : docs) {
        out.insert(addr_of(d));
    }
    return out;
}

nano_lance::FtsSearchResult search(const std::filesystem::path& path, const nano_lance::FtsQuery& q,
                                   std::optional<std::uint64_t> limit = std::nullopt) {
    nano_lance::FtsSearchRequest request;
    request.query = q;
    request.limit = limit;
    nano_lance::FtsSearchResult out;
    std::string error;
    require(nano_lance::dataset_full_text_search(path, request, out, error), "search: " + error);
    require(out.row_ids.size() == out.scores.size(), "one score per row");
    for (std::size_t i = 1; i < out.scores.size(); ++i) {
        require(out.scores[i - 1] >= out.scores[i], "rows are best first");
    }
    for (const auto s : out.scores) {
        require(s > 0.0F, "a BM25 score is positive");
    }
    return out;
}

std::set<std::uint64_t> as_set(const nano_lance::FtsSearchResult& r) { return {r.row_ids.begin(), r.row_ids.end()}; }

nano_lance::FtsQuery match(const std::string& text, bool all = false) {
    nano_lance::FtsQuery q;
    q.kind = nano_lance::FtsQuery::Kind::Match;
    q.text = text;
    q.columns = {"text"};
    q.and_operator = all;
    return q;
}

}  // namespace

int main() {
    const auto path = index_test::scratch("nano_lance_fts_search_test");
    index_test::write_fragment(path, false, kColumns, 0, 3,
                               [](ArrowArray* a, std::size_t c, std::int64_t r) { fill(a, c, r, kDocs); });
    index_test::write_fragment(path, true, kColumns, 3, 3,
                               [](ArrowArray* a, std::size_t c, std::int64_t r) { fill(a, c, r, kDocs); });

    std::string error;
    nano_lance::InvertedIndexOptions options;
    options.params.with_position = true;
    options.params.stem = false;
    options.params.remove_stop_words = false;
    std::uint64_t version = 0;
    require(nano_lance::dataset_create_inverted_index(path, "text", options, version, error), "build: " + error);

    // The manifest entry: one INVERTED segment over both fragments, its files listed and present.
    {
        nano_lance::pb::Manifest manifest;
        std::uint64_t latest = 0;
        require(nano_lance::load_latest_manifest(path, manifest, latest, error), "manifest: " + error);
        require(latest == version, "the build is the latest version");
        require(manifest.indices.size() == 1U, "one index entry");
        const auto& index = manifest.indices.front();
        require(index.name == "text_idx", "the default name is <column>_idx");
        require(nano_lance::is_inverted_index_url(index.details_type_url), "the details are InvertedIndexDetails");
        require(nano_lance::inverted_details_granularity(index.details_value) == nano_lance::DocGranularity::Row,
                "row granularity");
        require((index.fragment_ids == std::vector<std::uint32_t>{0, 1}), "it covers both fragments");
        std::set<std::string> names;
        const auto dir = path / "_indices" / nano_lance::pb::uuid_string(index.uuid);
        for (const auto& f : index.files) {
            names.insert(f.path);
            require(std::filesystem::file_size(dir / f.path) == f.size, "the listed size of " + f.path);
        }
        require((names == std::set<std::string>{"metadata.lance", "part_0_tokens.lance", "part_0_docs.lance",
                                                "part_0_invert.lance"}),
                "Lance's four files: metadata and one partition's tokens, docs and postings");
        std::vector<nano_lance::IndexInfo> infos;
        require(nano_lance::dataset_list_indices(path, false, 0, infos, error), "list: " + error);
        require(infos.size() == 1U && infos[0].type == "Inverted", "listed as an Inverted index");
    }

    // Match, OR: any term; AND: every term.
    require(as_set(search(path, match("fox"))) == rows_of({0, 4}), "fox");
    require(as_set(search(path, match("quick dog"))) == rows_of({0, 1, 2, 4}), "quick OR dog");
    require(as_set(search(path, match("fox dog", true))) == rows_of({4}), "fox AND dog");
    require(search(path, match("zebra")).row_ids.empty(), "a term no document has");
    {
        // "brown" alone is in 0 and 3; "brown fox" scores 0 above 3 (it has both terms).
        const auto r = search(path, match("brown fox"));
        require(!r.row_ids.empty() && r.row_ids.front() == addr_of(0), "the document with both terms ranks first");
        require(search(path, match("the"), 1).row_ids.size() == 1U, "a limit keeps the best row");
    }

    // Phrase: the words adjacent and in order.
    {
        nano_lance::FtsQuery q = match("quick brown");
        q.kind = nano_lance::FtsQuery::Kind::Phrase;
        require(as_set(search(path, q)) == rows_of({0}), "the phrase 'quick brown'");
        q.text = "brown quick";
        require(search(path, q).row_ids.empty(), "the words out of order are no phrase");
        q.text = "fox dog";
        q.slop = 2;
        require(as_set(search(path, q)) == rows_of({4}), "a phrase with slop");
    }

    // Boolean: MUST and MUST_NOT; Boost: a negative query lowers, never removes.
    {
        nano_lance::FtsQuery b;
        b.kind = nano_lance::FtsQuery::Kind::Boolean;
        b.must = {match("the")};
        b.must_not = {match("dog")};
        require(as_set(search(path, b)) == rows_of({0, 2}), "the AND NOT dog");
        nano_lance::FtsQuery boost;
        boost.kind = nano_lance::FtsQuery::Kind::Boost;
        boost.positive = {match("fox")};
        boost.negative = {match("dog")};
        boost.negative_boost = 0.5F;
        const auto r = search(path, boost);
        require(as_set(r) == rows_of({0, 4}), "boost keeps the positive rows");
        require(r.row_ids.front() == addr_of(0), "the row also matching the negative query ranks lower");
    }

    // A row appended after the build: in a fragment the index does not cover, found by the flat search.
    {
        std::vector<std::string> docs = kDocs;
        docs.push_back("a fox in the snow");
        index_test::write_fragment(path, true, kColumns, 6, 1,
                                   [&](ArrowArray* a, std::size_t c, std::int64_t r) { fill(a, c, r, docs); });
        require(as_set(search(path, match("fox"))) == rows_of({0, 4, 6}), "the unindexed row is found too");
        nano_lance::FtsSearchRequest request;
        request.query = match("fox");
        request.fast_search = true;
        nano_lance::FtsSearchResult out;
        require(nano_lance::dataset_full_text_search(path, request, out, error), "fast search: " + error);
        require(as_set(out) == rows_of({0, 4}), "fast_search searches only the indexed fragments");
    }

    // parse_fts_query: pylance's JSON forms, and refusals.
    {
        nano_lance::FtsQuery q;
        require(nano_lance::parse_fts_query(R"({"match": {"column": "text", "terms": "fox dog", "operator": "And"}})",
                                            q, error),
                "match JSON: " + error);
        require(q.kind == nano_lance::FtsQuery::Kind::Match && q.and_operator && q.text == "fox dog" &&
                    q.columns == std::vector<std::string>{"text"},
                "a match query with AND");
        require(as_set(search(path, q)) == rows_of({4}), "the parsed query answers as the built one");
        require(nano_lance::parse_fts_query(R"({"match_phrase": {"column": "text", "terms": "quick brown", "slop": 1}})",
                                            q, error),
                "phrase JSON: " + error);
        require(q.kind == nano_lance::FtsQuery::Kind::Phrase && q.slop == 1U, "a phrase with its slop");
        require(nano_lance::parse_fts_query(
                    R"({"boolean": {"must": [{"match": {"column": "text", "terms": "the"}}], "should": [], "must_not": []}})",
                    q, error),
                "boolean JSON: " + error);
        require(q.kind == nano_lance::FtsQuery::Kind::Boolean && q.must.size() == 1U, "a boolean query");
        require(nano_lance::parse_fts_query(R"("fox")", q, error), "a bare string: " + error);
        require(q.kind == nano_lance::FtsQuery::Kind::MultiMatch && q.text == "fox", "a string is a MultiMatch");
        require(!nano_lance::parse_fts_query("{not json", q, error) && !error.empty(), "malformed JSON is refused");
        require(!nano_lance::parse_fts_query(R"({"nonsense": {}})", q, error), "an unknown query kind is refused");
    }

    // A column with no index and no such column.
    {
        nano_lance::FtsSearchRequest request;
        request.query = match("fox");
        request.query.columns = {"missing"};
        nano_lance::FtsSearchResult out;
        require(!nano_lance::dataset_full_text_search(path, request, out, error), "a missing column is an error");
    }

    std::filesystem::remove_all(path);
    std::cout << "fts_search: ok (build, match, phrase, boolean, boost, unindexed rows, JSON queries)\n";
    return 0;
}
