// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// base/unicode: decoding (valid sequences, every invalid one as U+FFFD of one byte, overlongs and
// surrogates), encoding back; lower and upper case of Latin, Greek (the final sigma), Cyrillic, Turkish İ and
// German ß, and the ASCII fast path; the simple case fold; the character classes; NFD and NFC (a decomposed
// é, canonical mark order, Hebrew points, a precomposed Hebrew presentation form, Hangul); and removing
// Hebrew niqqud and cantillation and Arabic harakat and tatweel, leaving Latin marks alone.

#include "base/unicode.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace u = nano_lance::unicode;

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::string utf8(const std::vector<std::uint32_t>& cps) {
    std::string out;
    for (const auto cp : cps) {
        u::append(out, cp);
    }
    return out;
}

void test_decode() {
    for (const std::uint32_t cp : {0x0U, 0x41U, 0x7FU, 0x80U, 0x5D0U, 0x7FFU, 0x800U, 0xFFFDU, 0xFFFFU, 0x10000U, 0x1F600U, 0x10FFFFU}) {
        std::string s;
        u::append(s, cp);
        std::size_t len = 0;
        require(u::decode(s, 0, len) == cp && len == s.size(), "round trip of U+" + std::to_string(cp));
    }
    // Invalid: a lone continuation byte, a truncated sequence, an overlong, a surrogate, past U+10FFFF.
    for (const std::string bad : {std::string("\x80"), std::string("\xD7"), std::string("\xC0\x80"),
                                  std::string("\xE0\x80\x80"), std::string("\xED\xA0\x80"), std::string("\xF4\x90\x80\x80")}) {
        std::size_t len = 0;
        require(u::decode(bad, 0, len) == 0xFFFDU && len == 1U, "invalid sequence is U+FFFD of one byte");
    }
    require(u::code_points("aש😀") == std::vector<std::uint32_t>{0x61U, 0x5E9U, 0x1F600U}, "code points");
    require(u::is_ascii("") && u::is_ascii("plain ascii text, longer than eight") && !u::is_ascii("ascii then ש") &&
                !u::is_ascii("\x80"),
            "is_ascii");
}

void test_case() {
    require(u::to_lower("Hello World") == "hello world" && u::to_upper("Hello World") == "HELLO WORLD", "ASCII");
    require(u::to_lower("ПРИВЕТ Мир") == "привет мир" && u::to_upper("привет") == "ПРИВЕТ", "Cyrillic");
    require(u::to_upper("Ștefan Țară") == "ȘTEFAN ȚARĂ" && u::to_lower("ȘTEFAN") == "ștefan", "Romanian");
    require(u::to_upper("ÜNïcödé façade") == "ÜNÏCÖDÉ FAÇADE", "Latin-1");
    require(u::to_upper("Straße") == "STRASSE" && u::to_lower("STRASSE") == "strasse", "ß upper-cases to SS");
    require(u::to_lower("İstanbul") == "i\xCC\x87stanbul", "İ lower-cases to i + combining dot");
    // The final sigma: at the end of a word, not alone, not before a letter.
    require(u::to_lower("ΣΟΦΟΣ ΛΟΓΟΣ") == utf8({0x3C3, 0x3BF, 0x3C6, 0x3BF, 0x3C2, 0x20, 0x3BB, 0x3BF, 0x3B3, 0x3BF, 0x3C2}),
            "final sigma");
    require(u::to_lower("Σ") == "σ" && u::to_lower("ΣΑ") == "σα", "a sigma not at a word's end");
    require(u::to_lower("שלום") == "שלום" && u::to_upper("مرحبا") == "مرحبا", "scripts without case");
    require(u::to_lower("a\xFF" "B") == "a\xEF\xBF\xBD" "b", "an invalid byte becomes U+FFFD");

    require(u::fold('A') == 'a' && u::fold(0x3A3) == 0x3C3 && u::fold(0x3C2) == 0x3C3 && u::fold(0x3C3) == 0x3C3,
            "Σ, ς and σ fold together");
    require(u::fold(0x41F) == 0x43F && u::fold(0x218) == 0x219 && u::fold(0xDF) == 0xDF && u::fold(0x5D0) == 0x5D0,
            "fold: Cyrillic, Romanian, ß unchanged, Hebrew unchanged");
}

void test_classes() {
    require(u::is_alpha(0x5D0) && u::is_alpha(0x627) && u::is_alpha(0x416) && u::is_alpha('a') && !u::is_alpha('1') &&
                !u::is_alpha(0x5BC),
            "alpha: Hebrew, Arabic, Cyrillic letters; not digits or points");
    require(u::is_digit('7') && u::is_digit(0x663) && !u::is_digit(0x5D0), "digits of any script");
    require(u::is_space(' ') && u::is_space(0xA0) && u::is_space(0x2003) && !u::is_space(0x200F), "space");
    require(u::is_word(0x5D0) && u::is_word(0x5BC) && u::is_word('_') && u::is_word(0x200D) && !u::is_word('-') &&
                !u::is_word(0x200F),
            "word: letters, marks, underscore, the joiner; not the RTL mark");
    require(u::is_upper(0x416) && !u::is_upper(0x436) && u::is_lower(0x436) && !u::is_upper(0x5D0) && !u::is_lower(0x5D0),
            "upper / lower");
}

void test_normalization() {
    const std::string decomposed_e = utf8({'e', 0x301});
    require(u::nfc(decomposed_e) == utf8({0xE9}) && u::nfd(utf8({0xE9})) == decomposed_e, "é both ways");
    // Marks in either order are one string after normalization (dot below, class 220, before dot above, 230).
    require(u::nfc(utf8({'s', 0x323, 0x307})) == utf8({0x1E69}) && u::nfc(utf8({'s', 0x307, 0x323})) == utf8({0x1E69}),
            "canonical order");
    require(u::nfd(utf8({0x1E69})) == utf8({'s', 0x323, 0x307}), "NFD orders marks");
    // Hebrew: shin + shin dot + dagesh in either order is the same; the presentation form decomposes.
    require(u::nfc(utf8({0x5E9, 0x5C1, 0x5BC})) == utf8({0x5E9, 0x5BC, 0x5C1}), "Hebrew points ordered");
    require(u::nfd(utf8({0xFB2C})) == utf8({0x5E9, 0x5BC, 0x5C1}) && u::nfc(utf8({0xFB2C})) == utf8({0x5E9, 0x5BC, 0x5C1}),
            "a Hebrew presentation form decomposes and does not recompose");
    require(u::nfc(utf8({0x1100, 0x1161, 0x11A8})) == utf8({0xAC01}) && u::nfd(utf8({0xAC01})) == utf8({0x1100, 0x1161, 0x11A8}),
            "Hangul");
    require(u::nfc("plain") == "plain" && u::nfc("") == "", "ASCII is its own normal form");
    require(u::nfc(utf8({0x212B})) == utf8({0xC5}), "a singleton (Ångström sign) normalizes to Å");
}

void test_marks() {
    require(u::remove_semitic_marks("שָׁלוֹם עוֹלָם") == "שלום עולם", "niqqud");
    require(u::remove_semitic_marks(utf8({0x5D1, 0x5B0, 0x5E8, 0x5B5, 0x591, 0x5D0})) == utf8({0x5D1, 0x5E8, 0x5D0}),
            "cantillation (etnahta) too");
    require(u::remove_semitic_marks(utf8({0xFB2C, 0x5DC})) == utf8({0x5E9, 0x5DC}), "a presentation form loses its points");
    require(u::remove_semitic_marks("مَرْحَبًا") == "مرحبا", "harakat");
    require(u::remove_semitic_marks("مـــرحبا") == "مرحبا", "tatweel");
    require(u::remove_semitic_marks(utf8({0x5BE})) == utf8({0x5BE}) && u::remove_semitic_marks(utf8({0x5C3})) == utf8({0x5C3}),
            "maqaf and sof pasuq are punctuation, not marks");
    require(u::remove_semitic_marks("façade Ștefan") == "façade Ștefan", "Latin marks stay");
    require(u::remove_semitic_marks("plain") == "plain", "ASCII");
}

}  // namespace

int main() {
    test_decode();
    test_case();
    test_classes();
    test_normalization();
    test_marks();
    std::cout << "unicode: ok\n";
    return 0;
}
