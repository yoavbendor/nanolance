// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "fts_posting.hpp"

#include <algorithm>
#include <cstring>

namespace nano_lance::fts {
namespace {

bool get_varint(const std::uint8_t*& p, const std::uint8_t* end, std::uint32_t& v) {
    std::uint64_t out = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (p == end) {
            return false;
        }
        const std::uint8_t b = *p++;
        out |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0U) {
            v = static_cast<std::uint32_t>(out);
            return out <= 0xFFFFFFFFULL;
        }
    }
    return false;
}

std::uint32_t load32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

}  // namespace

void bitpack_pack(const std::uint32_t* values, std::size_t lanes, std::uint8_t bits, std::uint8_t* out) {
    if (bits == 0U) {
        return;
    }
    std::uint32_t words[8 * 32] = {};
    for (unsigned j = 0; j < lanes; ++j) {
        for (unsigned i = 0; i < 32; ++i) {
            const std::uint32_t v = values[lanes * i + j];
            const unsigned bit = i * bits;
            const unsigned w = bit >> 5U;
            const unsigned shift = bit & 31U;
            words[lanes * w + j] |= v << shift;
            if (shift + bits > 32U) {
                words[lanes * (w + 1) + j] |= v >> (32U - shift);
            }
        }
    }
    std::memcpy(out, words, 4U * lanes * bits);
}

void bitpack_unpack(const std::uint8_t* in, std::size_t lanes, std::uint8_t bits, std::uint32_t* values) {
    const std::size_t count = 32U * lanes;
    if (bits == 0U) {
        std::memset(values, 0, count * sizeof(std::uint32_t));
        return;
    }
    std::uint32_t words[8 * 32];
    std::memcpy(words, in, 4U * lanes * bits);
    const std::uint32_t mask = bits == 32U ? 0xFFFFFFFFU : (1U << bits) - 1U;
    for (unsigned j = 0; j < lanes; ++j) {
        for (unsigned i = 0; i < 32; ++i) {
            const unsigned bit = i * bits;
            const unsigned w = bit >> 5U;
            const unsigned shift = bit & 31U;
            std::uint32_t v = words[lanes * w + j] >> shift;
            if (shift + bits > 32U) {
                v |= words[lanes * (w + 1) + j] << (32U - shift);
            }
            values[lanes * i + j] = v & mask;
        }
    }
}

namespace {

constexpr std::size_t kPforMaxExceptions = 31;  // Lance's PFOR_MAX_EXCEPTIONS

std::size_t u32_bits(std::uint32_t v) {
    return static_cast<std::size_t>(bit_width(v));
}

std::size_t varint_len(std::uint32_t v) {
    return (std::max<std::size_t>(u32_bits(v), 1U) + 6U) / 7U;
}

}  // namespace

void encode_pfor256(const std::uint32_t* values, std::vector<std::uint8_t>& out) {
    constexpr std::size_t kCount = kLargePostingBlock;
    std::size_t max_bits = 0;
    for (std::size_t i = 0; i < kCount; ++i) {
        max_bits = std::max(max_bits, u32_bits(values[i]));
    }
    // The width that minimizes the bytes (Lance: the first width whose exceptions exceed 31 ends the search).
    std::size_t best_width = max_bits;
    std::size_t best_cost = kCount * max_bits / 8U;
    for (std::size_t width = max_bits; width-- > 0;) {
        std::size_t exceptions = 0;
        std::size_t exception_bytes = 0;
        for (std::size_t i = 0; i < kCount; ++i) {
            if (u32_bits(values[i]) > width) {
                ++exceptions;
                exception_bytes += 1U + varint_len(values[i] >> width);
            }
        }
        if (exceptions > kPforMaxExceptions) {
            break;
        }
        const std::size_t cost = kCount * width / 8U + exception_bytes;
        if (cost < best_cost) {
            best_cost = cost;
            best_width = width;
        }
    }
    const std::uint32_t mask = best_width >= 32U ? 0xFFFFFFFFU : (1U << best_width) - 1U;
    std::uint32_t body[kCount];
    std::vector<std::uint8_t> exception_buf;
    std::uint8_t exception_count = 0;
    for (std::size_t i = 0; i < kCount; ++i) {
        body[i] = values[i] & mask;
        if (u32_bits(values[i]) > best_width) {
            exception_buf.push_back(static_cast<std::uint8_t>(i));
            put_varint(exception_buf, values[i] >> best_width);
            ++exception_count;
        }
    }
    out.push_back(static_cast<std::uint8_t>(best_width));
    out.push_back(exception_count);
    const std::size_t at = out.size();
    out.resize(at + 32U * best_width);
    bitpack_pack(body, 8, static_cast<std::uint8_t>(best_width), out.data() + at);
    out.insert(out.end(), exception_buf.begin(), exception_buf.end());
}

bool decode_posting(const std::vector<PostingBlockView>& blocks, std::uint32_t length, TailCodec codec,
                    std::size_t block, std::vector<std::uint32_t>& docs, std::vector<std::uint32_t>& freqs,
                    std::string& error) {
    if (blocks.size() != (static_cast<std::size_t>(length) + block - 1) / block) {
        error = "posting list: " + std::to_string(blocks.size()) + " blocks for " + std::to_string(length) +
                " documents";
        return false;
    }
    // 128 blocks start with their score; 256 blocks have none.
    const std::size_t prefix = block == kPostingBlock ? 4U : 0U;
    const std::size_t lanes = block == kLargePostingBlock ? 8U : 4U;
    std::uint32_t values[kLargePostingBlock];
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const std::size_t n = std::min<std::size_t>(block, length - b * block);
        const std::uint8_t* p = blocks[b].data + prefix;
        const std::uint8_t* end = blocks[b].data + blocks[b].size;
        bool ok = blocks[b].size >= prefix;
        if (ok && n == block) {
            ok = end - p >= 5;
            std::uint32_t doc = ok ? load32(p) : 0;
            const std::uint8_t bits = ok ? p[4] : 0;
            p += 5;
            // A 128 block's frequency width byte follows the deltas; a 256 block's PFOR header does too.
            const std::ptrdiff_t tail_bytes = block == kLargePostingBlock ? 2 : 1;
            ok = ok && bits <= 32U && end - p >= static_cast<std::ptrdiff_t>(4U * lanes * bits) + tail_bytes;
            if (ok) {
                bitpack_unpack(p, lanes, bits, values);
                p += 4U * lanes * bits;
                for (std::size_t i = 0; i < block; ++i) {
                    doc += values[i];
                    docs.push_back(doc);
                }
                if (block == kLargePostingBlock) {  // patched FOR
                    const std::uint8_t width = p[0];
                    const std::uint8_t count = p[1];
                    p += 2;
                    // Lance never writes an exception at width 32 (nothing exceeds it).
                    ok = width <= 32U && end - p >= static_cast<std::ptrdiff_t>(4U * lanes * width) &&
                         (count == 0U || width < 32U);
                    if (ok) {
                        bitpack_unpack(p, lanes, width, values);
                        p += 4U * lanes * width;
                    }
                    for (std::size_t e = 0; ok && e < count; ++e) {
                        ok = end - p >= 1;
                        const std::size_t index = ok ? *p++ : 0U;
                        std::uint32_t high = 0;
                        ok = ok && index < block && get_varint(p, end, high);
                        if (ok) {
                            values[index] |= high << width;
                        }
                    }
                    if (ok) {
                        freqs.insert(freqs.end(), values, values + block);
                    }
                } else {
                    const std::uint8_t fbits = *p++;
                    ok = fbits <= 32U && end - p >= static_cast<std::ptrdiff_t>(4U * lanes * fbits);
                    if (ok) {
                        bitpack_unpack(p, lanes, fbits, values);
                        freqs.insert(freqs.end(), values, values + block);
                    }
                }
            }
        } else if (ok && codec == TailCodec::Fixed32) {
            ok = static_cast<std::size_t>(end - p) >= 8 * n;
            for (std::size_t i = 0; ok && i < n; ++i) {
                docs.push_back(load32(p + 4 * i));
            }
            for (std::size_t i = 0; ok && i < n; ++i) {
                freqs.push_back(load32(p + 4 * (n + i)));
            }
        } else if (ok) {
            std::uint32_t doc = 0;
            for (std::size_t i = 0; ok && i < n; ++i) {
                std::uint32_t delta = 0;
                ok = get_varint(p, end, delta);
                doc = i == 0 ? delta : doc + delta;
                docs.push_back(doc);
            }
            for (std::size_t i = 0; ok && i < n; ++i) {
                std::uint32_t f = 0;
                ok = get_varint(p, end, f);
                freqs.push_back(f);
            }
        }
        if (!ok) {
            error = "posting list: malformed block " + std::to_string(b);
            return false;
        }
    }
    return true;
}

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void encode_posting_block(const std::uint32_t* docs, const std::uint32_t* freqs, std::size_t n, float score,
                          std::vector<std::uint8_t>& out, TailCodec tail, std::size_t block) {
    if (block == kPostingBlock) {
        std::uint8_t bytes[4];
        std::memcpy(bytes, &score, 4);
        out.insert(out.end(), bytes, bytes + 4);
    }
    const std::size_t lanes = block == kLargePostingBlock ? 8U : 4U;
    if (n == block) {
        std::uint32_t deltas[kLargePostingBlock];
        std::uint32_t any = 0;
        for (std::size_t i = 0; i < n; ++i) {
            deltas[i] = docs[i] - (i == 0 ? docs[0] : docs[i - 1]);
            any |= deltas[i];
        }
        std::uint8_t bytes[4];
        std::memcpy(bytes, &docs[0], 4);
        out.insert(out.end(), bytes, bytes + 4);
        std::uint8_t bits = bit_width(any);
        out.push_back(bits);
        std::size_t at = out.size();
        out.resize(at + 4U * lanes * bits);
        bitpack_pack(deltas, lanes, bits, out.data() + at);
        if (block == kLargePostingBlock) {
            encode_pfor256(freqs, out);
            return;
        }
        any = 0;
        for (std::size_t i = 0; i < n; ++i) {
            any |= freqs[i];
        }
        bits = bit_width(any);
        out.push_back(bits);
        at = out.size();
        out.resize(at + 4U * lanes * bits);
        bitpack_pack(freqs, lanes, bits, out.data() + at);
        return;
    }
    if (tail == TailCodec::Fixed32) {
        std::uint8_t bytes[4];
        for (std::size_t i = 0; i < n; ++i) {
            std::memcpy(bytes, &docs[i], 4);
            out.insert(out.end(), bytes, bytes + 4);
        }
        for (std::size_t i = 0; i < n; ++i) {
            std::memcpy(bytes, &freqs[i], 4);
            out.insert(out.end(), bytes, bytes + 4);
        }
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        put_varint(out, i == 0 ? docs[0] : docs[i] - docs[i - 1]);
    }
    for (std::size_t i = 0; i < n; ++i) {
        put_varint(out, freqs[i]);
    }
}

}  // namespace nano_lance::fts
