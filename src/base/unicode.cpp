// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "base/unicode.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace nano_lance::unicode {

namespace {

#include "base/unicode_tables.inc"

bool in_ranges(const std::uint32_t (*begin)[2], const std::uint32_t (*end)[2], std::uint32_t cp) {
    const auto* it = std::upper_bound(begin, end, cp, [](std::uint32_t v, const std::uint32_t (&r)[2]) { return v < r[0]; });
    return it != begin && cp <= (*(it - 1))[1];
}

template <std::size_t N>
bool in(const std::uint32_t (&table)[N][2], std::uint32_t cp) {
    return in_ranges(std::begin(table), std::end(table), cp);
}

template <std::size_t N>
const char* mapping(const CaseMapping (&table)[N], std::uint32_t cp) {
    const auto* it = std::lower_bound(std::begin(table), std::end(table), cp,
                                      [](const CaseMapping& m, std::uint32_t v) { return m.cp < v; });
    return it != std::end(table) && it->cp == cp ? it->to : nullptr;
}

std::uint8_t combining_class(std::uint32_t cp) {
    if (cp < 0x300U) {
        return 0;
    }
    const auto* it = std::upper_bound(std::begin(kCombiningClass), std::end(kCombiningClass), cp,
                                      [](std::uint32_t v, const CombiningClass& c) { return v < c.first; });
    return it != std::begin(kCombiningClass) && cp <= (it - 1)->last ? (it - 1)->ccc : 0;
}

// Hangul syllables decompose and compose by arithmetic (Unicode 3.12).
constexpr std::uint32_t kSBase = 0xAC00, kLBase = 0x1100, kVBase = 0x1161, kTBase = 0x11A7;
constexpr std::uint32_t kLCount = 19, kVCount = 21, kTCount = 28, kNCount = kVCount * kTCount, kSCount = kLCount * kNCount;

void decompose_into(std::uint32_t cp, std::vector<std::uint32_t>& out) {
    if (cp >= kSBase && cp < kSBase + kSCount) {
        const auto s = cp - kSBase;
        out.push_back(kLBase + s / kNCount);
        out.push_back(kVBase + (s % kNCount) / kTCount);
        if (s % kTCount != 0U) {
            out.push_back(kTBase + s % kTCount);
        }
        return;
    }
    if (const char* d = mapping(kDecompose, cp); d != nullptr) {
        const std::string_view v(d);
        for (std::size_t i = 0; i < v.size();) {
            std::size_t len = 0;
            out.push_back(decode(v, i, len));
            i += len;
        }
        return;
    }
    out.push_back(cp);
}

/// Canonical ordering: a stable sort of each run of nonzero-class marks by class.
void canonical_order(std::vector<std::uint32_t>& cps) {
    for (std::size_t i = 0; i < cps.size();) {
        if (combining_class(cps[i]) == 0U) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < cps.size() && combining_class(cps[j]) != 0U) {
            ++j;
        }
        std::stable_sort(cps.begin() + static_cast<std::ptrdiff_t>(i), cps.begin() + static_cast<std::ptrdiff_t>(j),
                         [](std::uint32_t a, std::uint32_t b) { return combining_class(a) < combining_class(b); });
        i = j;
    }
}

std::uint32_t compose_pair(std::uint32_t a, std::uint32_t b) {
    if (a >= kLBase && a < kLBase + kLCount && b >= kVBase && b < kVBase + kVCount) {
        return kSBase + ((a - kLBase) * kVCount + (b - kVBase)) * kTCount;
    }
    if (a >= kSBase && a < kSBase + kSCount && (a - kSBase) % kTCount == 0U && b > kTBase && b < kTBase + kTCount) {
        return a + (b - kTBase);
    }
    const auto* it = std::lower_bound(std::begin(kCompose), std::end(kCompose), std::make_pair(a, b),
                                      [](const Composition& c, const std::pair<std::uint32_t, std::uint32_t>& k) {
                                          return c.first < k.first || (c.first == k.first && c.second < k.second);
                                      });
    return it != std::end(kCompose) && it->first == a && it->second == b ? it->composed : 0U;
}

/// The canonical composition of decomposed, canonically ordered code points (UAX #15's algorithm).
void compose(std::vector<std::uint32_t>& cps) {
    if (cps.empty()) {
        return;
    }
    std::size_t starter = 0;
    std::uint32_t starter_cp = cps[0];
    int last_class = combining_class(starter_cp) == 0U ? 0 : 256;
    std::size_t out = 1;
    for (std::size_t i = 1; i < cps.size(); ++i) {
        const auto cp = cps[i];
        const int cc = combining_class(cp);
        if (const auto composite = compose_pair(starter_cp, cp); composite != 0U && (last_class < cc || last_class == 0)) {
            cps[starter] = composite;
            starter_cp = composite;
            continue;
        }
        if (cc == 0) {
            starter = out;
            starter_cp = cp;
        }
        last_class = cc;
        cps[out++] = cp;
    }
    cps.resize(out);
}

std::string encode(const std::vector<std::uint32_t>& cps) {
    std::string out;
    out.reserve(cps.size());
    for (const auto cp : cps) {
        append(out, cp);
    }
    return out;
}

std::vector<std::uint32_t> decomposed(std::string_view s) {
    std::vector<std::uint32_t> cps;
    cps.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        std::size_t len = 0;
        decompose_into(decode(s, i, len), cps);
        i += len;
    }
    canonical_order(cps);
    return cps;
}

}  // namespace

bool is_ascii(std::string_view s) {
    std::size_t i = 0;
    for (; i + 8 <= s.size(); i += 8) {
        std::uint64_t w = 0;
        std::memcpy(&w, s.data() + i, 8);
        if ((w & 0x8080808080808080ULL) != 0U) {
            return false;
        }
    }
    for (; i < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) >= 0x80U) {
            return false;
        }
    }
    return true;
}

std::uint32_t decode(std::string_view s, std::size_t i, std::size_t& len) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    const auto cont = [&](std::size_t k) {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0U) == 0x80U;
    };
    const auto c = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + k]) & 0x3FU); };
    if (b0 < 0x80U) {
        len = 1;
        return b0;
    }
    if (b0 >= 0xC2U && b0 < 0xE0U && cont(1)) {
        len = 2;
        return ((b0 & 0x1FU) << 6U) | c(1);
    }
    if (b0 >= 0xE0U && b0 < 0xF0U && cont(1) && cont(2)) {
        const auto cp = ((b0 & 0x0FU) << 12U) | (c(1) << 6U) | c(2);
        if (cp >= 0x800U && (cp < 0xD800U || cp > 0xDFFFU)) {
            len = 3;
            return cp;
        }
    } else if (b0 >= 0xF0U && b0 < 0xF5U && cont(1) && cont(2) && cont(3)) {
        const auto cp = ((b0 & 0x07U) << 18U) | (c(1) << 12U) | (c(2) << 6U) | c(3);
        if (cp >= 0x10000U && cp < 0x110000U) {
            len = 4;
            return cp;
        }
    }
    len = 1;
    return 0xFFFDU;
}

void append(std::string& out, std::uint32_t cp) {
    if (cp < 0x80U) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

std::vector<std::uint32_t> code_points(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        std::size_t len = 0;
        out.push_back(decode(s, i, len));
        i += len;
    }
    return out;
}

namespace {

/// Unicode's Final_Sigma condition for the capital sigma at bytes [at, at + 2) of `s`: a cased letter before
/// it (skipping case-ignorables) and none after. Only that character
/// needs its context, so the rest of the text is never decoded for it.
bool final_sigma_at(std::string_view s, std::size_t at) {
    bool before = false;
    for (std::size_t j = at; j > 0;) {
        std::size_t k = j - 1;
        while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0U) == 0x80U) {
            --k;
        }
        std::size_t len = 0;
        const auto c = decode(s, k, len);
        j = k;
        if (in(kCaseIgnorable, c)) {
            continue;
        }
        before = in(kCased, c);
        break;
    }
    if (!before) {
        return false;
    }
    for (std::size_t i = at + 2; i < s.size();) {
        std::size_t len = 0;
        const auto c = decode(s, i, len);
        i += len;
        if (in(kCaseIgnorable, c)) {
            continue;
        }
        return !in(kCased, c);
    }
    return true;
}

/// One pass over the bytes: ASCII inline, every other character decoded and mapped by `table`.
template <bool Lower, std::size_t N>
std::string change_case(std::string_view s, const CaseMapping (&table)[N]) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80U) {
            const bool flip = Lower ? (b >= 'A' && b <= 'Z') : (b >= 'a' && b <= 'z');
            out.push_back(static_cast<char>(flip ? b ^ 0x20U : b));
            ++i;
            continue;
        }
        std::size_t len = 0;
        const auto cp = decode(s, i, len);
        if (Lower && cp == 0x3A3U && final_sigma_at(s, i)) {
            append(out, 0x3C2U);
        } else if (const char* m = mapping(table, cp); m != nullptr) {
            out += m;
        } else if (cp == 0xFFFDU && len == 1U) {
            append(out, cp);  // an invalid byte
        } else {
            out.append(s.substr(i, len));
        }
        i += len;
    }
    return out;
}

}  // namespace

std::string to_lower(std::string_view s) { return change_case<true>(s, kToLower); }

std::string to_upper(std::string_view s) { return change_case<false>(s, kToUpper); }

std::uint32_t fold(std::uint32_t cp) {
    if (cp < 0x80U) {
        return cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp;
    }
    const auto* it = std::lower_bound(std::begin(kSimpleFold), std::end(kSimpleFold), cp,
                                      [](const std::uint32_t (&m)[2], std::uint32_t v) { return m[0] < v; });
    return it != std::end(kSimpleFold) && (*it)[0] == cp ? (*it)[1] : cp;
}

bool is_alpha(std::uint32_t cp) { return in(kAlpha, cp); }
bool is_digit(std::uint32_t cp) { return in(kDigit, cp); }
bool is_space(std::uint32_t cp) { return in(kSpace, cp); }
bool is_upper(std::uint32_t cp) { return mapping(kToLower, cp) != nullptr && is_alpha(cp); }
bool is_lower(std::uint32_t cp) { return mapping(kToUpper, cp) != nullptr && is_alpha(cp); }
bool is_word(std::uint32_t cp) { return in(kWord, cp); }

std::string nfd(std::string_view s) {
    if (is_ascii(s)) {
        return std::string(s);
    }
    return encode(decomposed(s));
}

std::string nfc(std::string_view s) {
    if (is_ascii(s)) {
        return std::string(s);
    }
    auto cps = decomposed(s);
    compose(cps);
    return encode(cps);
}

bool is_semitic_mark(std::uint32_t cp) { return cp >= 0x590U && in(kSemiticMark, cp); }

std::string remove_semitic_marks(std::string_view s) {
    if (is_ascii(s)) {
        return std::string(s);
    }
    auto cps = decomposed(s);
    cps.erase(std::remove_if(cps.begin(), cps.end(), is_semitic_mark), cps.end());
    compose(cps);
    return encode(cps);
}

}  // namespace nano_lance::unicode
