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

void bitpack4x_pack(const std::uint32_t* values, std::uint8_t bits, std::uint8_t* out) {
    if (bits == 0U) {
        return;
    }
    std::uint32_t words[4 * 32] = {};
    for (unsigned j = 0; j < 4; ++j) {
        for (unsigned i = 0; i < 32; ++i) {
            const std::uint32_t v = values[4 * i + j];
            const unsigned bit = i * bits;
            const unsigned w = bit >> 5U;
            const unsigned shift = bit & 31U;
            words[4 * w + j] |= v << shift;
            if (shift + bits > 32U) {
                words[4 * (w + 1) + j] |= v >> (32U - shift);
            }
        }
    }
    std::memcpy(out, words, 16U * bits);
}

void bitpack4x_unpack(const std::uint8_t* in, std::uint8_t bits, std::uint32_t* values) {
    if (bits == 0U) {
        std::memset(values, 0, kPostingBlock * sizeof(std::uint32_t));
        return;
    }
    std::uint32_t words[4 * 32];
    std::memcpy(words, in, 16U * bits);
    const std::uint32_t mask = bits == 32U ? 0xFFFFFFFFU : (1U << bits) - 1U;
    for (unsigned j = 0; j < 4; ++j) {
        for (unsigned i = 0; i < 32; ++i) {
            const unsigned bit = i * bits;
            const unsigned w = bit >> 5U;
            const unsigned shift = bit & 31U;
            std::uint32_t v = words[4 * w + j] >> shift;
            if (shift + bits > 32U) {
                v |= words[4 * (w + 1) + j] << (32U - shift);
            }
            values[4 * i + j] = v & mask;
        }
    }
}

bool decode_posting(const std::vector<PostingBlockView>& blocks, std::uint32_t length, TailCodec codec,
                    std::vector<std::uint32_t>& docs, std::vector<std::uint32_t>& freqs, std::string& error) {
    if (blocks.size() != (static_cast<std::size_t>(length) + kPostingBlock - 1) / kPostingBlock) {
        error = "posting list: " + std::to_string(blocks.size()) + " blocks for " + std::to_string(length) +
                " documents";
        return false;
    }
    std::uint32_t values[kPostingBlock];
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const std::size_t n = std::min<std::size_t>(kPostingBlock, length - b * kPostingBlock);
        const std::uint8_t* p = blocks[b].data + 4;  // past the block's score
        const std::uint8_t* end = blocks[b].data + blocks[b].size;
        bool ok = blocks[b].size >= 4;
        if (ok && n == kPostingBlock) {
            ok = end - p >= 5;
            std::uint32_t doc = ok ? load32(p) : 0;
            const std::uint8_t bits = ok ? p[4] : 0;
            p += 5;
            ok = ok && bits <= 32U && end - p >= 16 * bits + 1;
            if (ok) {
                bitpack4x_unpack(p, bits, values);
                p += 16 * bits;
                for (std::size_t i = 0; i < kPostingBlock; ++i) {
                    doc += values[i];
                    docs.push_back(doc);
                }
                const std::uint8_t fbits = *p++;
                ok = fbits <= 32U && end - p >= 16 * fbits;
                if (ok) {
                    bitpack4x_unpack(p, fbits, values);
                    freqs.insert(freqs.end(), values, values + kPostingBlock);
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
                          std::vector<std::uint8_t>& out) {
    std::uint8_t bytes[4];
    std::memcpy(bytes, &score, 4);
    out.insert(out.end(), bytes, bytes + 4);
    if (n == kPostingBlock) {
        std::uint32_t deltas[kPostingBlock];
        std::uint32_t any = 0;
        for (std::size_t i = 0; i < n; ++i) {
            deltas[i] = docs[i] - (i == 0 ? docs[0] : docs[i - 1]);
            any |= deltas[i];
        }
        std::memcpy(bytes, &docs[0], 4);
        out.insert(out.end(), bytes, bytes + 4);
        std::uint8_t bits = bit_width(any);
        out.push_back(bits);
        std::size_t at = out.size();
        out.resize(at + 16U * bits);
        bitpack4x_pack(deltas, bits, out.data() + at);
        any = 0;
        for (std::size_t i = 0; i < n; ++i) {
            any |= freqs[i];
        }
        bits = bit_width(any);
        out.push_back(bits);
        at = out.size();
        out.resize(at + 16U * bits);
        bitpack4x_pack(freqs, bits, out.data() + at);
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
