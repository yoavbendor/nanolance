// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/fsst.hpp"

#include "nanolance/read_safety.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>

namespace nano_lance::fsst {

namespace {

/// "FSST" in the header's top 32 bits. The low 32 hold encoder_switch / suffix_lim / terminator /
/// n_symbols, one byte each.
constexpr std::uint64_t kMagic = std::uint64_t{0x46535354U} << 32U;
constexpr std::uint64_t kEncoderSwitchBit = std::uint64_t{1} << 24U;

}  // namespace

bool parse_symbol_table(const std::vector<std::uint8_t>& bytes, SymbolTable& out, std::string& error) {
    if (bytes.size() != kSymbolTableBytes) {
        error = "FSST symbol table is " + std::to_string(bytes.size()) + " bytes, expected " +
                std::to_string(kSymbolTableBytes);
        return false;
    }
    const auto header = load_le<std::uint64_t>(bytes.data());
    if ((header & kMagic) != kMagic) {
        error = "FSST symbol table has the wrong magic";
        return false;
    }

    // Parse into a scratch value and publish only on success, so a caller that ignores the return
    // value cannot be handed a half-built table (the same discipline decode_page_layout follows).
    SymbolTable parsed;
    parsed.passthrough = (header & kEncoderSwitchBit) == 0U;
    parsed.symbol_count = static_cast<std::uint32_t>(header & 0xFFU);

    // Symbols come first, then one length byte per symbol immediately after them -- NOT at a fixed
    // offset past all 256 slots. The table is sized for 256 and zero-padded, so the lengths of a
    // 120-symbol table sit at byte 8 + 120*8, not 8 + 256*8.
    std::size_t pos = 8U;
    for (std::uint32_t i = 0; i < parsed.symbol_count; ++i) {
        for (std::size_t b = 0; b < kMaxSymbolLength; ++b) {
            parsed.symbols[i][b] = bytes[pos + b];
        }
        pos += kMaxSymbolLength;
    }
    for (std::uint32_t i = 0; i < parsed.symbol_count; ++i) {
        const auto length = bytes[pos++];
        if (length < 1U || length > kMaxSymbolLength) {
            error = "FSST symbol " + std::to_string(i) + " declares length " + std::to_string(length) +
                    ", expected 1..8";
            return false;
        }
        parsed.lengths[i] = length;
    }

    out = parsed;
    return true;
}

bool decode_checked(const SymbolTable& table, const std::uint8_t* data, std::size_t size, std::uint8_t*& dst,
                    std::string& error) {
    if (table.passthrough) {
        if (size != 0U) {
            std::memcpy(dst, data, size);
        }
        dst += size;
        return true;
    }
    auto* out = dst;
    for (std::size_t i = 0; i < size;) {
        const auto code = data[i];
        if (code == kEscape) {
            if (i + 1U >= size) {
                error = "FSST escape at the end of a value has no payload byte";
                return false;
            }
            *out++ = data[i + 1U];
            i += 2U;
            continue;
        }
        if (code >= table.symbol_count) {
            error = "FSST code " + std::to_string(code) + " is not in the symbol table (" +
                    std::to_string(table.symbol_count) + " symbols)";
            return false;
        }
        // The whole 8-byte word, then advance by the symbol's length: the caller's slack.
        std::memcpy(out, table.symbols[code].data(), kMaxSymbolLength);
        out += table.lengths[code];
        ++i;
    }
    dst = out;
    return true;
}

bool decompress_value(const SymbolTable& table, const std::uint8_t* data, std::size_t size,
                      std::vector<std::uint8_t>& out, std::string& error) {
    const auto at = out.size();
    out.resize(at + size * kMaxSymbolLength + kMaxSymbolLength);  // geometric growth, as reserve_more
    auto* dst = out.data() + at;
    if (!decode_checked(table, data, size, dst, error)) {
        out.resize(at);
        return false;
    }
    out.resize(static_cast<std::size_t>(dst - out.data()));
    return true;
}

namespace {

constexpr std::size_t kLongSlots = 1024U;
constexpr std::size_t kSampleTarget = std::size_t{1} << 14U;  // FSST_SAMPLETARGET
constexpr std::uint16_t kMaxSymbols = 255U;                    // code 255 is the escape
/// Construction codes: 0..254 are real symbols, 256 + b is byte b escaped.
constexpr std::uint16_t kPseudo = 256U;
constexpr std::size_t kCodes = 512U;

std::uint64_t hash3(std::uint64_t word) {
    const auto w = word & 0xFFFFFFU;
    const auto m = w * 2971215073ULL;  // FSST_HASH_PRIME
    return (m ^ (m >> 15U)) & (kLongSlots - 1U);
}

std::uint64_t mask(unsigned length) {
    return length >= 8U ? ~std::uint64_t{0} : ((std::uint64_t{1} << (8U * length)) - 1U);
}

/// Up to 8 bytes at `p`, zero-filled past `remaining`.
std::uint64_t load(const std::uint8_t* p, std::size_t remaining) {
    std::uint64_t w = 0;
    std::memcpy(&w, p, std::min<std::size_t>(remaining, 8U));
    return w;
}

void reset(Encoder& e) {
    e.symbol_count = 0;
    e.byte_codes.fill(Encoder::kNone);
    e.short_codes.assign(65536U, Encoder::kNone);
    e.long_codes.fill(Encoder::Slot{});
}

/// Add a symbol as the next code. False when its three-byte hash slot is taken: that symbol is
/// skipped, as in Lance.
bool add(Encoder& e, std::uint64_t value, unsigned length) {
    const auto code = static_cast<std::uint16_t>(e.symbol_count);
    if (length == 1U) {
        e.byte_codes[value & 0xFFU] = code;
    } else if (length == 2U) {
        e.short_codes[value & 0xFFFFU] = code;
    } else {
        auto& slot = e.long_codes[hash3(value)];
        if (slot.length != 0U) {
            return false;
        }
        slot = Encoder::Slot{value, static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(code)};
    }
    e.symbols[code] = value;
    e.lengths[code] = static_cast<std::uint8_t>(length);
    ++e.symbol_count;
    return true;
}

/// The longest symbol matching at `p`, or kNone.
std::uint16_t find(const Encoder& e, const std::uint8_t* p, std::size_t remaining, unsigned& length) {
    const auto word = load(p, remaining);
    if (remaining >= 3U) {
        const auto& slot = e.long_codes[hash3(word)];
        if (slot.length != 0U && slot.length <= remaining && (word & mask(slot.length)) == slot.value) {
            length = slot.length;
            return slot.code;
        }
    }
    if (remaining >= 2U) {
        const auto code = e.short_codes[word & 0xFFFFU];
        if (code != Encoder::kNone) {
            length = 2U;
            return code;
        }
    }
    length = 1U;
    return e.byte_codes[word & 0xFFU];
}

using Values = std::vector<std::pair<const std::uint8_t*, std::size_t>>;

/// A deterministic sample of about kSampleTarget bytes: every value when they are that small,
/// otherwise values drawn at random (splitmix64, fixed seed) until the target is reached.
Values make_sample(const Values& values) {
    std::size_t total = 0;
    for (const auto& v : values) {
        total += v.second;
    }
    if (total <= kSampleTarget) {
        return values;
    }
    Values sample;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL ^ total;
    std::size_t taken = 0;
    while (taken < kSampleTarget) {
        state += 0x9E3779B97F4A7C15ULL;
        auto z = state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        z ^= z >> 31U;
        const auto& v = values[static_cast<std::size_t>(z % values.size())];
        sample.push_back(v);
        taken += v.second;
    }
    return sample;
}

struct Symbol {
    std::uint64_t value;
    unsigned length;
};

Symbol symbol_of(const Encoder& e, std::uint16_t code) {
    if (code >= kPseudo) {
        return {static_cast<std::uint64_t>(code - kPseudo), 1U};
    }
    return {e.symbols[code], e.lengths[code]};
}

}  // namespace

bool train(const Values& values, Encoder& out) {
    const auto sample = make_sample(values);
    Encoder table;
    reset(table);
    Encoder best = table;
    std::int64_t best_gain = std::numeric_limits<std::int64_t>::min();
    std::vector<std::uint16_t> count1(kCodes);
    std::vector<std::uint16_t> count2(kCodes * kCodes);
    // Rows of count2 written this round, so clearing it costs what was used rather than 512 KiB.
    std::vector<std::uint16_t> touched_rows;
    std::vector<bool> row_touched(kCodes);
    const auto bump = [](std::uint16_t& c) {
        if (c != 0xFFFFU) {
            ++c;
        }
    };
    const auto code_at = [&table](const std::uint8_t* p, std::size_t remaining, unsigned& length) {
        const auto code = find(table, p, remaining, length);
        return code == Encoder::kNone ? static_cast<std::uint16_t>(kPseudo + *p) : code;
    };

    for (const unsigned frac : {8U, 38U, 68U, 98U, 108U, 128U}) {
        // Compress the sample with the current table, counting symbols and adjacent pairs.
        std::fill(count1.begin(), count1.end(), 0U);
        for (const auto row : touched_rows) {
            std::fill_n(count2.begin() + static_cast<std::ptrdiff_t>(row * kCodes), kCodes, std::uint16_t{0});
            row_touched[row] = false;
        }
        touched_rows.clear();
        std::int64_t gain = 0;
        for (const auto& [data, size] : sample) {
            if (size == 0U) {
                continue;
            }
            std::size_t pos = 0;
            unsigned length = 0;
            auto prev = code_at(data, size, length);
            pos += length;
            gain += std::max<std::int64_t>(0, static_cast<std::int64_t>(length) - 1 - (prev >= kPseudo ? 1 : 0));
            while (pos < size) {
                bump(count1[prev]);
                if (symbol_of(table, prev).length != 1U) {
                    bump(count1[kPseudo + data[pos]]);
                }
                const auto cur = code_at(data + pos, size - pos, length);
                gain += std::max<std::int64_t>(0, static_cast<std::int64_t>(length) - 1 - (cur >= kPseudo ? 1 : 0));
                if (frac < 128U) {  // no pairs in the last round
                    if (!row_touched[prev]) {
                        row_touched[prev] = true;
                        touched_rows.push_back(prev);
                    }
                    bump(count2[prev * kCodes + cur]);
                    if (length > 1U) {
                        bump(count2[prev * kCodes + kPseudo + data[pos]]);
                    }
                }
                pos += length;
                prev = cur;
            }
            bump(count1[prev]);
        }
        if (gain >= best_gain) {
            best_gain = gain;
            best = table;
        }

        // Candidates: every symbol seen, and every pair seen, weighted by count x length.
        std::map<std::pair<std::uint64_t, unsigned>, std::uint64_t> candidates;
        const auto consider = [&](Symbol s, std::uint64_t count) {
            if (count < (5U * frac) / 128U) {
                return;
            }
            candidates[{s.value, s.length}] += count * s.length;
        };
        std::vector<std::uint16_t> live;
        for (std::uint16_t c = 0; c < table.symbol_count; ++c) {
            live.push_back(c);
        }
        for (std::uint16_t b = 0; b < 256U; ++b) {
            live.push_back(static_cast<std::uint16_t>(kPseudo + b));
        }
        for (const auto c1 : live) {
            const auto n1 = count1[c1];
            if (n1 == 0U) {
                continue;
            }
            const auto s1 = symbol_of(table, c1);
            // Single bytes count 8x: they cut the escape rate (the paper's heuristic).
            consider(s1, static_cast<std::uint64_t>(s1.length == 1U ? 8U : 1U) * n1);
            if (frac >= 128U || s1.length == 8U || !row_touched[c1]) {
                continue;
            }
            for (const auto c2 : live) {
                const auto n2 = count2[c1 * kCodes + c2];
                if (n2 == 0U) {
                    continue;
                }
                const auto s2 = symbol_of(table, c2);
                const unsigned length = std::min(8U, s1.length + s2.length);
                const auto value = (s1.value | (s2.value << (8U * s1.length))) & mask(length);
                consider(Symbol{value, length}, n2);
            }
        }
        std::vector<std::pair<std::uint64_t, std::pair<std::uint64_t, unsigned>>> ranked;
        ranked.reserve(candidates.size());
        for (const auto& [symbol, g] : candidates) {
            ranked.emplace_back(g, symbol);
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second.first < b.second.first;
        });
        reset(table);
        for (const auto& [g, symbol] : ranked) {
            if (table.symbol_count == kMaxSymbols) {
                break;
            }
            add(table, symbol.first, symbol.second);
        }
    }
    if (best.symbol_count == 0U) {
        return false;
    }
    best.short_or_byte.resize(65536U);
    for (std::uint32_t w = 0; w < 65536U; ++w) {
        const auto two = best.short_codes[w];
        const auto one = best.byte_codes[w & 0xFFU];
        best.short_or_byte[w] = two != Encoder::kNone   ? static_cast<std::uint16_t>((2U << 8U) | two)
                                : one != Encoder::kNone ? static_cast<std::uint16_t>((1U << 8U) | one)
                                                        : static_cast<std::uint16_t>((1U << 8U) | kEscape);
    }
    out = std::move(best);
    return true;
}

std::size_t compress_into(const Encoder& encoder, const std::uint8_t* data, std::size_t size, std::uint8_t* dst) {
    auto* out = dst;
    std::size_t pos = 0;
    if (!encoder.short_or_byte.empty()) {
        // While 8 bytes remain: one unaligned load, the long-symbol slot, then one lookup that
        // settles the two-byte / one-byte / escape cases. The code and the literal byte are both
        // stored and `out` advances past the literal only for an escape (it stays within 2 * size).
        while (pos + 8U <= size) {
            std::uint64_t word = 0;
            std::memcpy(&word, data + pos, 8U);
            const auto& slot = encoder.long_codes[hash3(word)];
            if (slot.length != 0U && ((word ^ slot.value) << (64U - 8U * slot.length)) == 0U) {
                *out++ = slot.code;
                pos += slot.length;
                continue;
            }
            const auto entry = encoder.short_or_byte[word & 0xFFFFU];
            const auto code = static_cast<std::uint8_t>(entry & 0xFFU);
            out[0] = code;
            out[1] = static_cast<std::uint8_t>(word & 0xFFU);
            out += code == kEscape ? 2 : 1;
            pos += entry >> 8U;
        }
        // The last < 8 bytes, the same way from a zero-padded copy (so the loads stay in bounds), with
        // no match allowed to run past the value.
        if (pos < size) {
            std::array<std::uint8_t, 16> tail{};
            const auto rest = size - pos;
            std::memcpy(tail.data(), data + pos, rest);
            std::size_t at = 0;
            while (at < rest) {
                std::uint64_t word = 0;
                std::memcpy(&word, tail.data() + at, 8U);
                const auto remaining = rest - at;
                const auto& slot = encoder.long_codes[hash3(word)];
                if (slot.length != 0U && slot.length <= remaining &&
                    ((word ^ slot.value) << (64U - 8U * slot.length)) == 0U) {
                    *out++ = slot.code;
                    at += slot.length;
                    continue;
                }
                std::uint16_t entry = 0;
                if (remaining >= 2U) {
                    entry = encoder.short_or_byte[word & 0xFFFFU];
                } else {
                    const auto one = encoder.byte_codes[word & 0xFFU];
                    entry = static_cast<std::uint16_t>((1U << 8U) | (one != Encoder::kNone ? one : kEscape));
                }
                const auto code = static_cast<std::uint8_t>(entry & 0xFFU);
                out[0] = code;
                out[1] = static_cast<std::uint8_t>(word & 0xFFU);
                out += code == kEscape ? 2 : 1;
                at += entry >> 8U;
            }
            return static_cast<std::size_t>(out - dst);
        }
        return static_cast<std::size_t>(out - dst);
    }
    while (pos < size) {
        unsigned length = 0;
        const auto code = find(encoder, data + pos, size - pos, length);
        if (code == Encoder::kNone) {
            *out++ = kEscape;
            *out++ = data[pos];
            ++pos;
        } else {
            *out++ = static_cast<std::uint8_t>(code);
            pos += length;
        }
    }
    return static_cast<std::size_t>(out - dst);
}

void compress_value(const Encoder& encoder, const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    const auto at = out.size();
    out.resize(at + 2U * size);
    out.resize(at + compress_into(encoder, data, size, out.data() + at));
}

std::vector<std::uint8_t> serialize(const Encoder& encoder) {
    std::vector<std::uint8_t> out(kSymbolTableBytes, 0U);
    const std::uint64_t header = kMagic | kEncoderSwitchBit | encoder.symbol_count;
    std::memcpy(out.data(), &header, 8U);  // little-endian, as every integer this project writes
    std::size_t pos = 8U;
    for (std::uint32_t i = 0; i < encoder.symbol_count; ++i) {
        std::memcpy(out.data() + pos, &encoder.symbols[i], 8U);
        pos += 8U;
    }
    for (std::uint32_t i = 0; i < encoder.symbol_count; ++i) {
        out[pos++] = encoder.lengths[i];
    }
    return out;
}

}  // namespace nano_lance::fsst
