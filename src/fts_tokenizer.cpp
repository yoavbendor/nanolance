// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Lance's full-text analyzer (fts_tokenizer.hpp): lance-tokenizer 12.0.0's SimpleTokenizer /
// WhitespaceTokenizer / RawTokenizer, RemoveLongFilter, LowerCaser, Stemmer, StopWordFilter and
// AsciiFoldingFilter, chained as Lance 12's InvertedIndexParams::build chains them.

#include "nanolance/fts_tokenizer.hpp"
#include "nanolance/fts_jieba.hpp"

#include "fts_json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

namespace nano_lance::fts {
namespace {

#include "fts_tables.inc"

namespace snow {
#include "fts_snowball_env.inc"
#if defined(__GNUC__)
// The generated Snowball stemmer has labels its code never jumps to; they are its own, not ours.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-label"
#endif
#include "fts_english_stem.inc"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}  // namespace snow

// ── UTF-8 ───────────────────────────────────────────────────────────────────────────────────────

/// The code point at `text[i]` and its length in bytes (an invalid byte: itself, length 1).
std::uint32_t decode(std::string_view text, std::size_t i, std::size_t& len) {
    const auto b0 = static_cast<unsigned char>(text[i]);
    const auto cont = [&](std::size_t k) {
        return i + k < text.size() && (static_cast<unsigned char>(text[i + k]) & 0xC0U) == 0x80U;
    };
    const auto c = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + k]) & 0x3FU); };
    if (b0 < 0x80U) {
        len = 1;
        return b0;
    }
    if (b0 >= 0xC2U && b0 < 0xE0U && cont(1)) {
        len = 2;
        return ((b0 & 0x1FU) << 6U) | c(1);
    }
    if (b0 >= 0xE0U && b0 < 0xF0U && cont(1) && cont(2)) {
        len = 3;
        return ((b0 & 0x0FU) << 12U) | (c(1) << 6U) | c(2);
    }
    if (b0 >= 0xF0U && b0 < 0xF5U && cont(1) && cont(2) && cont(3)) {
        len = 4;
        return ((b0 & 0x07U) << 18U) | (c(1) << 12U) | (c(2) << 6U) | c(3);
    }
    len = 1;
    return 0xFFFDU;
}

bool is_alphanumeric(std::uint32_t cp) {
    if (cp < 0x80U) {
        return std::isalnum(static_cast<int>(cp)) != 0;
    }
    const auto* end = std::end(kAlnum);
    const auto* it = std::upper_bound(std::begin(kAlnum), end, cp,
                                      [](std::uint32_t v, const std::uint32_t(&r)[2]) { return v < r[0]; });
    return it != std::begin(kAlnum) && cp <= (*(it - 1))[1];
}

bool is_ascii_whitespace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\x0C' || c == '\r'; }

/// The mapping of `cp` in a code point table (sorted), or null.
const char* lookup(const CharMap* begin, const CharMap* end, std::uint32_t cp) {
    const auto* it = std::lower_bound(begin, end, cp, [](const CharMap& m, std::uint32_t v) { return m.cp < v; });
    return it != end && it->cp == cp ? it->to : nullptr;
}

bool is_ascii(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80U; });
}

/// Each character on its own, as `table` maps it (unmapped characters as they are; ASCII letters
/// lower-cased when `lower_ascii`).
void map_chars(std::string_view in, const CharMap* begin, const CharMap* end, bool lower_ascii, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < in.size();) {
        std::size_t len = 0;
        const auto cp = decode(in, i, len);
        if (cp < 0x80U) {
            out.push_back(lower_ascii ? static_cast<char>(std::tolower(static_cast<int>(cp))) : static_cast<char>(cp));
        } else if (const char* to = lookup(begin, end, cp); to != nullptr) {
            out += to;
        } else {
            out.append(in.substr(i, len));
        }
        i += len;
    }
}

// ── the code analyzer: CodeLexTokenizer, then WordDelimiterFilter (lance-tokenizer's
// code_tokenizer.rs and word_delimiter_filter.rs, ported token for token) ─────────────────────────

bool in_ranges(const std::uint32_t (*begin)[2], const std::uint32_t (*end)[2], std::uint32_t cp) {
    const auto* it = std::upper_bound(begin, end, cp, [](std::uint32_t v, const std::uint32_t (&r)[2]) { return v < r[0]; });
    return it != begin && cp <= (*(it - 1))[1];
}

bool is_lowercase(std::uint32_t cp) { return in_ranges(std::begin(kLowercase), std::end(kLowercase), cp); }
bool is_uppercase(std::uint32_t cp) { return in_ranges(std::begin(kUppercase), std::end(kUppercase), cp); }
bool is_ascii_digit(std::uint32_t cp) { return cp >= '0' && cp <= '9'; }

bool is_code_identifier(std::uint32_t cp) { return cp == '_' || is_alphanumeric(cp); }

bool is_code_operator_char(std::uint32_t cp) {
    switch (cp) {
        case '!': case '%': case '&': case '*': case '+': case '-': case '/': case ':': case '<': case '=':
        case '>': case '?': case '^': case '|': case '~':
            return true;
        default:
            return false;
    }
}

/// MULTI_CHAR_OPERATORS of code_tokenizer.rs (longest match wins).
constexpr std::string_view kMultiCharOperators[] = {
    ">>>=", "<<=", ">>=", "&&=", "||=", "?\?=", "**=", "//=", "===", "!==", ">>>", "<=>", "::", "->", "=>", "==",
    "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "**",
    "//", "??", ":=", "<-", "|>", "~=",
};

/// The byte length of the operator token at text[i]: its longest multi-character operator, else 1.
std::size_t operator_length(std::string_view text, std::size_t i) {
    std::size_t best = 1;
    const auto rest = text.substr(i);
    for (const auto op : kMultiCharOperators) {
        if (op.size() > best && rest.substr(0, op.size()) == op) {
            best = op.size();
        }
    }
    return best;
}

struct CodeToken {
    std::size_t from;
    std::size_t to;
    std::uint32_t position;
};

/// CodeLexTokenizer: identifiers, and with `operators` the operator tokens; positions 0, 1, ...
std::vector<CodeToken> lex_code(std::string_view text, bool operators) {
    std::vector<CodeToken> out;
    std::uint32_t position = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t len = 0;
        const auto cp = decode(text, i, len);
        std::size_t end = 0;
        if (is_code_identifier(cp)) {
            end = i + len;
            while (end < text.size()) {
                std::size_t next = 0;
                if (!is_code_identifier(decode(text, end, next))) {
                    break;
                }
                end += next;
            }
        } else if (operators && is_code_operator_char(cp)) {
            end = i + operator_length(text, i);
        } else {
            i += len;
            continue;
        }
        out.push_back(CodeToken{i, end, position++});
        i = end;
    }
    return out;
}

/// A byte range of an identifier.
struct Piece {
    std::size_t from;
    std::size_t to;
};

/// The pieces of one run of identifier characters without '_' (split_segment in word_delimiter_filter.rs).
void split_segment(std::string_view text, std::size_t start, std::size_t end, bool numerics, std::vector<Piece>& pieces) {
    struct Char {
        std::size_t offset;
        std::uint32_t cp;
    };
    std::vector<Char> chars;
    for (std::size_t i = start; i < end;) {
        std::size_t len = 0;
        const auto cp = decode(text, i, len);
        chars.push_back(Char{i, cp});
        i += len;
    }
    std::size_t piece_start = start;
    for (std::size_t idx = 1; idx < chars.size(); ++idx) {
        const auto prev = chars[idx - 1].cp;
        const auto cur = chars[idx].cp;
        const bool boundary = (numerics && is_ascii_digit(prev) != is_ascii_digit(cur)) ||
                              (is_lowercase(prev) && is_uppercase(cur)) ||
                              (is_uppercase(prev) && is_uppercase(cur) && idx + 1 < chars.size() &&
                               is_lowercase(chars[idx + 1].cp));
        if (boundary) {
            pieces.push_back(Piece{piece_start, chars[idx].offset});
            piece_start = chars[idx].offset;
        }
    }
    pieces.push_back(Piece{piece_start, end});
}

/// split_identifier in word_delimiter_filter.rs: the pieces of `text` between its underscores.
std::vector<Piece> split_identifier(std::string_view text, bool numerics) {
    std::vector<Piece> pieces;
    std::size_t segment_start = 0;
    bool in_segment = false;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t len = 0;
        const auto cp = decode(text, i, len);
        if (cp == '_') {
            if (in_segment) {
                split_segment(text, segment_start, i, numerics, pieces);
                in_segment = false;
            }
        } else if (!in_segment) {
            segment_start = i;
            in_segment = true;
        }
        i += len;
    }
    if (in_segment) {
        split_segment(text, segment_start, text.size(), numerics, pieces);
    }
    return pieces;
}

/// The code analyzer's words and positions for `text` (before the filters that follow it).
void code_words(std::string_view text, const AnalyzerParams& p,
                std::vector<std::pair<std::string_view, std::uint32_t>>& out) {
    std::uint32_t shift = 0;  // WordDelimiterFilter's position_offset: the extra pieces so far
    for (const auto& tok : lex_code(text, p.index_operators)) {
        const std::string_view word = text.substr(tok.from, tok.to - tok.from);
        const std::uint32_t position = tok.position + shift;
        const auto pieces = p.split_identifiers ? split_identifier(word, p.split_on_numerics) : std::vector<Piece>{};
        if (pieces.size() <= 1) {
            out.emplace_back(word, position);
            continue;
        }
        if (p.preserve_original) {
            out.emplace_back(word, position);
        }
        for (std::size_t k = 0; k < pieces.size(); ++k) {
            out.emplace_back(word.substr(pieces[k].from, pieces[k].to - pieces[k].from),
                             position + static_cast<std::uint32_t>(k));
        }
        shift += static_cast<std::uint32_t>(pieces.size() - 1);
    }
}

}  // namespace

bool parse_params(std::string_view text, AnalyzerParams& out, std::string& error) {
    out = AnalyzerParams{};
    json::Value root;
    if (!json::parse(text, root) || root.kind != json::Value::Object) {
        error = "malformed index params";
        return false;
    }
    for (const auto& [key, v] : root.fields) {
        const auto number = [&](std::uint32_t& to) {
            if (v.kind == json::Value::Number) {
                to = static_cast<std::uint32_t>(v.n);
            }
        };
        const auto flag = [&](bool& to) {
            if (v.kind == json::Value::Bool) {
                to = v.b;
            }
        };
        if (key == "base_tokenizer" && v.kind == json::Value::String) {
            out.base_tokenizer = v.s;
        } else if (key == "language" && v.kind == json::Value::String) {
            out.language = v.s;
        } else if (key == "lance_tokenizer" && v.kind == json::Value::String) {
            out.lance_tokenizer = v.s;
        } else if (key == "with_position") {
            flag(out.with_position);
        } else if (key == "max_token_length") {
            if (v.kind == json::Value::Null) {
                out.max_token_length.reset();
            } else if (v.kind == json::Value::Number) {
                out.max_token_length = static_cast<std::uint32_t>(v.n);
            }
        } else if (key == "lower_case") {
            flag(out.lower_case);
        } else if (key == "stem") {
            flag(out.stem);
        } else if (key == "remove_stop_words") {
            flag(out.remove_stop_words);
        } else if (key == "custom_stop_words") {
            out.custom_stop_words.reset();
            if (v.kind == json::Value::Array) {
                out.custom_stop_words.emplace();
                for (const auto& w : v.items) {
                    if (w.kind == json::Value::String) {
                        out.custom_stop_words->push_back(w.s);
                    }
                }
            }
        } else if (key == "ascii_folding") {
            flag(out.ascii_folding);
        } else if (key == "min_ngram_length") {
            number(out.min_ngram_length);
        } else if (key == "max_ngram_length") {
            number(out.max_ngram_length);
        } else if (key == "prefix_only") {
            flag(out.prefix_only);
        } else if (key == "block_size") {
            number(out.block_size);
        } else if (key == "format_version") {
            number(out.format_version);
        } else if (key == "split_identifiers") {
            flag(out.split_identifiers);
        } else if (key == "split_on_numerics") {
            flag(out.split_on_numerics);
        } else if (key == "preserve_original") {
            flag(out.preserve_original);
        } else if (key == "index_operators") {
            flag(out.index_operators);
        }
    }
    return true;
}

std::string params_json(const AnalyzerParams& p) {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    std::string out = "{\"lance_tokenizer\":" + json::quote(p.lance_tokenizer) + ",\"base_tokenizer\":" +
                      json::quote(p.base_tokenizer) + ",\"language\":" + json::quote(p.language) +
                      ",\"with_position\":" + b(p.with_position) + ",\"max_token_length\":" +
                      (p.max_token_length ? std::to_string(*p.max_token_length) : std::string("null")) +
                      ",\"lower_case\":" + b(p.lower_case) + ",\"stem\":" + b(p.stem) +
                      ",\"remove_stop_words\":" + b(p.remove_stop_words) + ",\"custom_stop_words\":";
    if (p.custom_stop_words) {
        out += "[";
        for (std::size_t i = 0; i < p.custom_stop_words->size(); ++i) {
            out += (i == 0 ? "" : ",") + json::quote((*p.custom_stop_words)[i]);
        }
        out += "]";
    } else {
        out += "null";
    }
    out += ",\"ascii_folding\":" + std::string(b(p.ascii_folding)) + ",\"min_ngram_length\":" +
           std::to_string(p.min_ngram_length) + ",\"max_ngram_length\":" + std::to_string(p.max_ngram_length) +
           ",\"prefix_only\":" + b(p.prefix_only) + ",\"block_size\":" + std::to_string(p.block_size) +
           ",\"split_identifiers\":" + b(p.split_identifiers) + ",\"split_on_numerics\":" + b(p.split_on_numerics) +
           ",\"preserve_original\":" + b(p.preserve_original) + ",\"index_operators\":" + b(p.index_operators) + "}";
    return out;
}

bool Analyzer::init(const AnalyzerParams& params, std::string& error) {
    params_ = params;
    jieba_.reset();
    const bool jieba = params.base_tokenizer == "jieba" || params.base_tokenizer.rfind("jieba/", 0) == 0;
    if (jieba) {
        // Lance's build_base_tokenizer: "jieba" is "jieba/default", a directory under the model home.
        const auto home = language_model_home();
        if (home.empty()) {
            error = "Invalid user input: unknown base tokenizer " + params.base_tokenizer;
            return false;
        }
        jieba_ = Jieba::load(home / (params.base_tokenizer == "jieba" ? "jieba/default" : params.base_tokenizer), error);
        if (jieba_ == nullptr) {
            return false;
        }
    }
    if (!jieba && params.base_tokenizer != "simple" && params.base_tokenizer != "whitespace" &&
        params.base_tokenizer != "raw" && params.base_tokenizer != "code" && params.base_tokenizer != "ngram") {
        // Lance's other base tokenizers (icu, jieba, lindera) exist, nanolance does not implement them.
        const std::string& base = params.base_tokenizer;
        const bool lance_tokenizer = base.rfind("icu", 0) == 0 || base.rfind("jieba", 0) == 0 ||
                                     base.rfind("lindera", 0) == 0;
        error = lance_tokenizer
                    ? "the '" + base + "' tokenizer is not supported (simple, whitespace, raw, code and ngram are)"
                    : "unknown base tokenizer " + base + " (simple, whitespace, raw, code and ngram are supported)";
        return false;
    }
    if (params.base_tokenizer == "ngram" && params.min_ngram_length == 0U) {
        error = "min_gram must be greater than 0";
        return false;
    }
    if (params.base_tokenizer == "ngram" && params.min_ngram_length > params.max_ngram_length) {
        error = "min_gram must not be greater than max_gram";
        return false;
    }
    if (params.base_tokenizer != "code" && (params.split_identifiers || params.split_on_numerics ||
                                            params.preserve_original || params.index_operators)) {
        error = "code analyzer flags require base_tokenizer='code'";
        return false;
    }
    if (params.lance_tokenizer != "text") {
        error = "'" + params.lance_tokenizer + "' documents are not supported (text is)";
        return false;
    }
    const bool english = params.language == "English" || params.language == "english" || params.language == "en";
    if (params.stem && !english) {
        error = "stemming " + params.language + " is not supported (English is)";
        return false;
    }
    stop_words_.clear();
    if (params.remove_stop_words) {
        if (params.custom_stop_words) {
            stop_words_.insert(params.custom_stop_words->begin(), params.custom_stop_words->end());
        } else if (english) {
            stop_words_.insert(std::begin(kEnglishStopWords), std::end(kEnglishStopWords));
        } else {
            error = "removing " + params.language + " stop words is not supported (English is)";
            return false;
        }
    }
    return true;
}

namespace {

std::string lower_case(std::string_view word) {
    std::string text;
    if (is_ascii(word)) {
        text.assign(word);
        for (auto& c : text) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    } else {
        map_chars(word, std::begin(kLower), std::end(kLower), true, text);
    }
    return text;
}

std::string ascii_fold(std::string_view text) {
    if (is_ascii(text)) {
        return std::string(text);
    }
    std::string folded;
    map_chars(text, std::begin(kFold), std::end(kFold), false, folded);
    return folded;
}

}  // namespace

std::string lower_and_fold(std::string_view word) { return ascii_fold(lower_case(word)); }

namespace {

bool ngram_alphanumeric(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/// ngram.rs ngram_to_token for three bytes: each '0'-'9' -> 1-10, the rest as byte - 'a' + 11 (u8 arithmetic,
/// as Lance's release build wraps it), base 37, first character most significant.
std::uint32_t trigram_token(const char* t) {
    std::uint32_t token = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto byte = static_cast<std::uint8_t>(t[i]);
        const auto base = byte <= '9' ? static_cast<std::uint8_t>(byte - '0')
                                      : static_cast<std::uint8_t>(static_cast<std::uint8_t>(byte - 'a') + 10U);
        const auto pos = static_cast<std::uint8_t>(base + 1U);
        token = token * 37U + pos;
    }
    return token;
}

}  // namespace

std::vector<std::uint32_t> ngram_tokens(std::string_view folded) {
    std::vector<std::uint32_t> tokens;
    for (std::size_t i = 0; i + 3 <= folded.size(); ++i) {
        if (ngram_alphanumeric(folded[i]) && ngram_alphanumeric(folded[i + 1]) && ngram_alphanumeric(folded[i + 2])) {
            tokens.push_back(trigram_token(folded.data() + i));
        }
    }
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
    return tokens;
}

namespace {

/// ngram_tokenizer.rs's UTF-8 codepoint widths, indexed by the lead byte's high nibble.
constexpr std::uint8_t kCodepointWidth[16] = {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 4};

/// NgramTokenizer's grams of `text` in Lance's order (StutteringIterator over CodepointFrontiers): every
/// gram of min_gram to max_gram characters from each start, the window sliding one character at a time.
/// `visit(from, to)` gets the gram's byte range and returns false to stop (prefix_only stops at the first
/// gram that starts after byte 0).
template <typename Visit>
void ngram_grams(std::string_view text, std::size_t min_gram, std::size_t max_gram, bool prefix_only, Visit visit) {
    std::vector<std::size_t> frontiers;  // the byte offset of each character, then text.size()
    for (std::size_t at = 0; at < text.size();) {
        frontiers.push_back(at);
        const auto width = static_cast<std::size_t>(kCodepointWidth[static_cast<unsigned char>(text[at]) >> 4U]);
        at += std::min(width, text.size() - at);
    }
    frontiers.push_back(text.size());
    // The Rust iterator's state: `memory` holds the last max_gram + 1 frontiers as a ring; the signed
    // bounds let max_gram fall below min_gram exactly as Lance's do.
    std::size_t next = 0;
    std::vector<std::size_t> memory;
    while (memory.size() < max_gram + 1 && next < frontiers.size()) {
        memory.push_back(frontiers[next++]);
    }
    long long lo = static_cast<long long>(min_gram);
    long long hi = static_cast<long long>(memory.size()) - 1;
    long long gram_len = lo;
    if (memory.size() <= min_gram) {
        lo = 1;
        hi = 0;
        gram_len = 0;
    }
    std::size_t cursor = 0;
    while (true) {
        if (gram_len > hi) {
            gram_len = lo;
            if (next < frontiers.size()) {
                memory[cursor] = frontiers[next++];
            } else {
                --hi;
            }
            cursor = (cursor + 1) % memory.size();
        }
        if (hi < lo) {
            return;
        }
        const auto from = memory[cursor];
        const auto to = memory[(cursor + static_cast<std::size_t>(gram_len)) % memory.size()];
        ++gram_len;
        if (prefix_only && from > 0) {
            return;
        }
        if (!visit(from, to)) {
            return;
        }
    }
}

}  // namespace

void Analyzer::emit(std::string_view word, std::uint32_t position, std::vector<Token>& out) const {
    if (params_.max_token_length && word.size() >= *params_.max_token_length) {
        return;
    }
    std::string text;
    if (!params_.lower_case) {
        text.assign(word);
    } else {
        text = lower_case(word);
    }
    if (params_.stem) {
        snow::SnowballEnv env(text);
        snow::stem(env);
        text = std::move(env.current);
    }
    if (params_.remove_stop_words && stop_words_.count(text) != 0U) {
        return;
    }
    if (params_.ascii_folding) {
        text = ascii_fold(text);
    }
    out.push_back(Token{std::move(text), position});
}

void Analyzer::tokenize(std::string_view text, std::vector<Token>& out) const {
    std::uint32_t position = 0;
    if (params_.base_tokenizer == "raw") {
        emit(text, 0, out);
        return;
    }
    if (params_.base_tokenizer == "ngram") {
        // Lance's NgramTokenizer emits every gram at position 0, then the same filters as any word.
        ngram_grams(text, params_.min_ngram_length, params_.max_ngram_length, params_.prefix_only,
                    [&](std::size_t from, std::size_t to) {
                        emit(text.substr(from, to - from), 0, out);
                        return true;
                    });
        return;
    }
    if (jieba_ != nullptr) {
        // Lance's JiebaTokenizer: each token at its first character's offset.
        std::vector<JiebaToken> tokens;
        jieba_->tokenize_search(text, tokens);
        for (const auto& t : tokens) {
            emit(text.substr(t.byte_start, t.byte_end - t.byte_start), t.char_start, out);
        }
        return;
    }
    if (params_.base_tokenizer == "code") {
        std::vector<std::pair<std::string_view, std::uint32_t>> words;
        code_words(text, params_, words);
        for (const auto& [word, at] : words) {
            emit(word, at, out);
        }
        return;
    }
    const bool simple = params_.base_tokenizer == "simple";
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t len = 0;
        const bool in_word = simple ? is_alphanumeric(decode(text, i, len))
                                    : (len = 1, !is_ascii_whitespace(static_cast<unsigned char>(text[i])));
        if (!in_word) {
            i += len;
            continue;
        }
        const std::size_t start = i;
        i += len;
        while (i < text.size()) {
            const bool more = simple ? is_alphanumeric(decode(text, i, len))
                                     : (len = 1, !is_ascii_whitespace(static_cast<unsigned char>(text[i])));
            if (!more) {
                break;
            }
            i += len;
        }
        emit(text.substr(start, i - start), position++, out);
    }
}

}  // namespace nano_lance::fts
