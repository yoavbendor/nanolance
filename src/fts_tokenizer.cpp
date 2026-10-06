// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Lance's full-text analyzer (fts_tokenizer.hpp): lance-tokenizer 12.0.0's SimpleTokenizer /
// WhitespaceTokenizer / RawTokenizer, RemoveLongFilter, LowerCaser, Stemmer, StopWordFilter and
// AsciiFoldingFilter, chained as Lance 12's InvertedIndexParams::build chains them.

#include "nanolance/fts_tokenizer.hpp"

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
#include "fts_english_stem.inc"
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
           ",\"split_identifiers\":false,\"split_on_numerics\":false,\"preserve_original\":false,"
           "\"index_operators\":false}";
    return out;
}

bool Analyzer::init(const AnalyzerParams& params, std::string& error) {
    params_ = params;
    if (params.base_tokenizer != "simple" && params.base_tokenizer != "whitespace" && params.base_tokenizer != "raw") {
        error = "the '" + params.base_tokenizer + "' tokenizer is not supported (simple, whitespace and raw are)";
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

void Analyzer::emit(std::string_view word, std::uint32_t position, std::vector<Token>& out) const {
    if (params_.max_token_length && word.size() >= *params_.max_token_length) {
        return;
    }
    std::string text;
    if (!params_.lower_case) {
        text.assign(word);
    } else if (is_ascii(word)) {
        text.assign(word);
        for (auto& c : text) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    } else {
        map_chars(word, std::begin(kLower), std::end(kLower), true, text);
    }
    if (params_.stem) {
        snow::SnowballEnv env(text);
        snow::stem(env);
        text = std::move(env.current);
    }
    if (params_.remove_stop_words && stop_words_.count(text) != 0U) {
        return;
    }
    if (params_.ascii_folding && !is_ascii(text)) {
        std::string folded;
        map_chars(text, std::begin(kFold), std::end(kFold), false, folded);
        text = std::move(folded);
    }
    out.push_back(Token{std::move(text), position});
}

void Analyzer::tokenize(std::string_view text, std::vector<Token>& out) const {
    std::uint32_t position = 0;
    if (params_.base_tokenizer == "raw") {
        emit(text, 0, out);
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
