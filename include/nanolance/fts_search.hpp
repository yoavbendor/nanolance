// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nanolance/fts_tokenizer.hpp"

/// Full-text search over text columns with an INVERTED index (built by Lance or by nanolance;
/// docs/FTS_INDEX.md), as Lance's `full_text_query` answers it: BM25 scores from the index's
/// statistics for the rows it covers, and for rows in fragments it does not cover, from the index's
/// statistics together with theirs (those rows are tokenized on the fly).
namespace nano_lance {

/// A full-text query: Lance's FtsQuery.
struct FtsQuery {
    enum class Kind { Match, Phrase, MultiMatch, Boost, Boolean };
    Kind kind = Kind::Match;

    /// Match / Phrase / MultiMatch: the text, tokenized as the column's index tokenizes.
    std::string text;
    /// Match / Phrase: the one column. MultiMatch: its columns (empty: every column with an INVERTED
    /// index); a row's score is the best of its columns'.
    std::vector<std::string> columns;
    std::vector<float> boosts;  // MultiMatch: per column (empty: 1)

    float boost = 1.0F;                            // Match: the score's factor
    std::optional<std::uint32_t> fuzziness = 0;    // Match: only 0 (exact) is supported
    std::uint32_t max_expansions = 50;
    std::uint32_t prefix_length = 0;
    bool and_operator = false;  // Match / MultiMatch: every term must occur (else any)
    std::uint32_t slop = 0;     // Phrase

    /// Boost: the rows of `positive` (one query); those also matching `negative` (one query) score
    /// `positive - negative_boost * negative`.
    std::vector<FtsQuery> positive;
    std::vector<FtsQuery> negative;
    float negative_boost = 0.5F;

    /// Boolean: every MUST query matches (or, with none, some SHOULD query does) and no MUST_NOT
    /// query does; the score sums the MUST and matching SHOULD scores.
    std::vector<FtsQuery> must;
    std::vector<FtsQuery> should;
    std::vector<FtsQuery> must_not;
};

/// A query in JSON, as pylance's query objects describe themselves:
///   {"match": {"column": c, "terms": t, "boost": 1.0, "fuzziness": 0, "max_expansions": 50,
///              "operator": "Or", "prefix_length": 0}}
///   {"match_phrase": {"column": c, "terms": t, "slop": 0}}
///   {"multi_match": {"query": t, "columns": [..], "boost": [..], "operator": "Or"}}
///   {"boost": {"positive": q, "negative": q, "negative_boost": 0.5}}
///   {"boolean": {"must": [q..], "should": [q..], "must_not": [q..]}}
/// A JSON string is a MultiMatch over every indexed column.
bool parse_fts_query(std::string_view json, FtsQuery& out, std::string& error);

struct FtsSearchRequest {
    bool has_version = false;
    std::uint64_t version = 0;
    FtsQuery query;
    /// The best rows to return; all matching rows when unset.
    std::optional<std::uint64_t> limit;
    /// An SQL filter: applied before the search with `prefilter`, else to the rows found.
    std::optional<std::string> filter;
    bool prefilter = false;
    /// Search only the fragments the index covers.
    bool fast_search = false;
    /// When set, search only these segments (by UUID) of the columns' indexes -- one worker's share
    /// of a distributed search -- scoring with the statistics of all their segments, so the scores
    /// are the ones a search of every segment gives. Fragments outside the segments are not searched.
    std::vector<std::array<std::uint8_t, 16>> segments;
};

struct FtsSearchResult {
    std::vector<std::uint64_t> row_ids;  // row addresses, best first (ties by row id)
    std::vector<float> scores;           // BM25
    std::vector<std::string> plan;
};

bool dataset_full_text_search(const std::filesystem::path& dataset_path, const FtsSearchRequest& request,
                              FtsSearchResult& out, std::string& error);

struct InvertedIndexOptions {
    std::string name;  // empty: <column>_idx
    /// An index of the same name is replaced; without `replace`, that is an error.
    bool replace = true;
    /// The analyzer: Lance's and LanceDB's defaults unless set. Positions (`with_position`) are not
    /// supported.
    fts::AnalyzerParams params;
};

/// Build an INVERTED index on `column` (a string column) in Lance's format -- one pylance and LanceDB
/// search as their own -- and commit it as the next version: one partition of every document with
/// tokens, token ids in order of first appearance, posting lists with Lance's block scores and
/// impact skip data.
bool dataset_create_inverted_index(const std::filesystem::path& dataset_path, const std::string& column,
                                   const InvertedIndexOptions& options, std::uint64_t& new_version,
                                   std::string& error);

/// Whether an index's details type is an INVERTED index's.
bool is_inverted_index_url(const std::string& url);

}  // namespace nano_lance
