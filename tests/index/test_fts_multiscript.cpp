// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The full-text analyzer's settings for text in many scripts (AnalyzerParams: unicode_normalize, remove_marks,
// arabic_normalize, ascii_folding_keep_original): pointed and unpointed Hebrew and voweled and plain Arabic make
// one token; a decomposed é is é before the words are split; Romanian's cedilla letters are the comma ones; the
// Arabic letter forms unify only when asked; a word with diacritics is indexed folded and as written at one
// position (and an ASCII word once); the settings round-trip through the params JSON, which names the tokenizer
// nanolance/<base> only when one is on; with every setting off the tokens are Lance's, unchanged.

#include "nanolance/fts_tokenizer.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fts = nano_lance::fts;

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

fts::AnalyzerParams plain() {
    fts::AnalyzerParams p;
    p.stem = false;
    p.remove_stop_words = false;
    return p;
}

using Tokens = std::vector<std::pair<std::string, std::uint32_t>>;

Tokens tokens(const fts::AnalyzerParams& p, const std::string& text, std::vector<bool>* originals = nullptr) {
    fts::Analyzer a;
    std::string error;
    require(a.init(p, error), "init: " + error);
    std::vector<fts::Token> out;
    a.tokenize(text, out);
    Tokens t;
    for (const auto& tok : out) {
        t.emplace_back(tok.text, tok.position);
        if (originals != nullptr) {
            originals->push_back(tok.original);
        }
    }
    return t;
}

void test_marks() {
    auto p = plain();
    require(tokens(p, "שָׁלוֹם") != tokens(p, "שלום"), "without remove_marks, pointing makes another token (Lance's)");
    p.remove_marks = true;
    require(tokens(p, "שָׁלוֹם עוֹלָם") == Tokens{{"שלום", 0}, {"עולם", 1}}, "niqqud removed");
    require(tokens(p, "בְּרֵאשִׁ֖ית") == Tokens{{"בראשית", 0}}, "cantillation removed");
    require(tokens(p, "مَرْحَبًا بِالْعَالَمِ") == Tokens{{"مرحبا", 0}, {"بالعالم", 1}}, "harakat removed");
    require(tokens(p, "مـــرحبا") == Tokens{{"مرحبا", 0}}, "tatweel removed");
    require(tokens(p, "hello world") == Tokens{{"hello", 0}, {"world", 1}}, "ASCII unchanged");
}

void test_normalize() {
    auto p = plain();
    p.ascii_folding = false;
    require(tokens(p, "cafe\xCC\x81 noir") == Tokens{{"cafe", 0}, {"noir", 1}},
            "without unicode_normalize, a combining accent ends the word and is lost (Lance's)");
    p.unicode_normalize = true;
    require(tokens(p, "cafe\xCC\x81 noir") == Tokens{{"café", 0}, {"noir", 1}}, "NFC before the words are split");
    require(tokens(p, "ştiinţă Ştefan") == Tokens{{"știință", 0}, {"ștefan", 1}}, "cedilla letters as comma letters");
    require(tokens(p, "أحمد مدرسة") == Tokens{{"أحمد", 0}, {"مدرسة", 1}}, "Arabic letters kept without arabic_normalize");
    p.arabic_normalize = true;
    require(tokens(p, "أحمد إسلام آمن مدرسة مستشفى") ==
                Tokens{{"احمد", 0}, {"اسلام", 1}, {"امن", 2}, {"مدرسه", 3}, {"مستشفي", 4}},
            "arabic_normalize: alef forms, teh marbuta, alef maqsura");
}

void test_keep_original() {
    auto p = plain();
    require(tokens(p, "fată mare") == Tokens{{"fata", 0}, {"mare", 1}}, "ascii_folding alone keeps only the fold");
    p.ascii_folding_keep_original = true;
    std::vector<bool> originals;
    require(tokens(p, "Fată mare café", &originals) ==
                Tokens{{"fată", 0}, {"fata", 0}, {"mare", 1}, {"café", 2}, {"cafe", 2}},
            "both forms at one position; an ASCII word once");
    require(originals == std::vector<bool>{true, false, false, true, false}, "the originals are marked");

    fts::Analyzer a;
    std::string error;
    p.ascii_folding = false;
    require(!a.init(p, error) && error.find("requires ascii_folding") != std::string::npos,
            "keep_original without folding is refused");
}

void test_params_json() {
    const auto lance = plain();
    const auto json = fts::params_json(lance);
    require(json.find("nanolance/") == std::string::npos && json.find("remove_marks") == std::string::npos,
            "settings off: Lance's JSON, no new keys");

    auto p = plain();
    p.remove_marks = true;
    p.ascii_folding_keep_original = true;
    const auto ext = fts::params_json(p);
    require(ext.find("\"base_tokenizer\":\"nanolance/simple\"") != std::string::npos,
            "settings on: the tokenizer is named nanolance/simple");
    fts::AnalyzerParams back;
    std::string error;
    require(fts::parse_params(ext, back, error), "parse: " + error);
    require(back.base_tokenizer == "simple" && back.remove_marks && back.ascii_folding_keep_original &&
                !back.unicode_normalize && !back.arabic_normalize,
            "the settings round-trip and the prefix is taken off");
    require(fts::params_json(back) == ext, "and write back the same");
}

void test_off_is_lance() {
    // Every setting off: exactly the tokens the default analyzer makes (the golden test checks those
    // against Lance's).
    fts::AnalyzerParams off;
    const std::string text = "Ştiinţă cafe\xCC\x81 שָׁלוֹם مَرْحَبًا Running FAST façade";
    fts::AnalyzerParams same = off;
    require(tokens(off, text) == tokens(same, text), "deterministic");
    require(!off.extended(), "defaults are not extended");
}

}  // namespace

int main() {
    test_marks();
    test_normalize();
    test_keep_original();
    test_params_json();
    test_off_is_lance();
    std::cout << "fts multiscript: ok\n";
    return 0;
}
