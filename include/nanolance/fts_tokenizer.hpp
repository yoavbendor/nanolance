// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/// Lance's full-text analyzer, token for token: the text is split into words (`simple`: runs of
/// Unicode alphanumerics; `whitespace`: runs between ASCII whitespace; `raw`: the whole text), and
/// each word goes through, in this order, the long-token filter (dropped at max_token_length bytes
/// or more), lower-casing, English stemming (Snowball 3.1.1, as Lance's frostem), stop-word removal
/// (after stemming) and ASCII folding. Positions count the words before any is dropped.
///
/// The Unicode tables and the stemmer are generated from the Rust crates Lance runs
/// (tools/fts_tables, tools/fts_stem_translate.py), so the tokens are Lance's to the byte.
namespace nano_lance::fts {

/// An INVERTED index's analyzer settings: what Lance keeps as `params` in the index's metadata.
/// The defaults are Lance's and LanceDB's.
struct AnalyzerParams {
    std::string base_tokenizer = "simple";
    std::string language = "English";
    bool with_position = false;
    std::optional<std::uint32_t> max_token_length = 40;
    bool lower_case = true;
    bool stem = true;
    bool remove_stop_words = true;
    std::optional<std::vector<std::string>> custom_stop_words;
    bool ascii_folding = true;
    std::uint32_t min_ngram_length = 3;
    std::uint32_t max_ngram_length = 3;
    bool prefix_only = false;
    std::uint32_t block_size = 128;
    /// The on-disk FTS format (1, 2 or 3) a new index is written in; 0 = the default for block_size
    /// (2 at 128, 3 at 256). Read from the `format_version` key of an index's params JSON, never
    /// written into `params` (Lance keeps the format in metadata.lance and the index details).
    std::uint32_t format_version = 0;
    std::string lance_tokenizer = "text";
    /// The code analyzer (base_tokenizer "code", Lance's CodeTokenizerConfig): identifiers split into
    /// subwords, numerics as boundaries, the identifier kept too, operator tokens such as "::" and "->".
    /// Written in the params JSON as they are (false for the text analyzer).
    bool split_identifiers = false;
    bool split_on_numerics = false;
    bool preserve_original = false;
    bool index_operators = false;
};

/// The format version an index with `params` is (or would be) written in.
inline std::uint32_t resolved_format_version(const AnalyzerParams& params) {
    if (params.format_version != 0U) {
        return params.format_version;
    }
    return params.block_size == 256U ? 3U : 2U;
}

/// Lance's params JSON (metadata.lance's `params`). Fields it leaves out keep their defaults.
bool parse_params(std::string_view json, AnalyzerParams& out, std::string& error);

/// The params as Lance writes them.
std::string params_json(const AnalyzerParams& params);

struct Token {
    std::string text;
    std::uint32_t position = 0;
};

class Analyzer {
public:
    /// False (with `error`) for settings nanolance's analyzer does not implement: base tokenizers
    /// other than simple / whitespace / raw / code, stemming or built-in stop words in a language other than
    /// English, JSON documents.
    bool init(const AnalyzerParams& params, std::string& error);

    /// `text`'s tokens, appended to `out`.
    void tokenize(std::string_view text, std::vector<Token>& out) const;

    const AnalyzerParams& params() const { return params_; }

private:
    void emit(std::string_view word, std::uint32_t position, std::vector<Token>& out) const;

    AnalyzerParams params_;
    std::unordered_set<std::string> stop_words_;
};

}  // namespace nano_lance::fts
