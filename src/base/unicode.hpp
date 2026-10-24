// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// UTF-8 text by character, not by byte: decoding, case (SQL's lower() and upper(), and the case fold a
// case-insensitive match compares), the character classes a regular expression asks about, canonical
// normalization (NFC / NFD), and removing the vowel and cantillation marks of Hebrew and Arabic script.
// Every function has an ASCII fast path: text that is all ASCII costs one scan before the byte-wise work it
// always did. The data is generated (tools/gen_unicode_tables.py).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nano_lance::unicode {

/// True when every byte is below 0x80.
bool is_ascii(std::string_view s);

/// The code point at `s[i]` and, in `len`, its length in bytes. A byte that does not start a valid UTF-8
/// sequence is U+FFFD of length 1, so a scan always advances.
std::uint32_t decode(std::string_view s, std::size_t i, std::size_t& len);

/// `cp` as UTF-8, appended to `out`.
void append(std::string& out, std::uint32_t cp);

/// The code points of `s`.
std::vector<std::uint32_t> code_points(std::string_view s);

/// Full lower / upper case, as SQL's lower() and upper(): one character may become several (İ -> i̇,
/// ß -> SS), and a capital sigma at the end of a word lowers to the final ς.
std::string to_lower(std::string_view s);
std::string to_upper(std::string_view s);

/// The simple case fold of one character: what a case-insensitive comparison compares (σ, ς and Σ fold
/// together; ß stays ß).
std::uint32_t fold(std::uint32_t cp);

/// Character classes, for regular expressions.
bool is_alpha(std::uint32_t cp);
bool is_digit(std::uint32_t cp);  // decimal digits of any script
bool is_space(std::uint32_t cp);
bool is_upper(std::uint32_t cp);
bool is_lower(std::uint32_t cp);
bool is_word(std::uint32_t cp);  // \w: letters, marks, decimal digits, connector punctuation, joiners

/// Canonical normalization: NFD decomposes every character canonically (é -> e + ◌́) and puts marks in
/// canonical order; NFC then recomposes. Text that is the same to a reader is then the same bytes.
std::string nfd(std::string_view s);
std::string nfc(std::string_view s);

/// A Hebrew, Arabic, Syriac or Thaana vowel / cantillation mark, or the Arabic tatweel: what a search that
/// ignores pointing removes.
bool is_semitic_mark(std::uint32_t cp);

/// `s` without its Semitic marks (decomposed first, so precomposed presentation forms lose theirs too),
/// in NFC.
std::string remove_semitic_marks(std::string_view s);

}  // namespace nano_lance::unicode
