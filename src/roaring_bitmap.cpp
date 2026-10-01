// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/roaring_bitmap.hpp"

#include <cstring>

namespace nano_lance::roaring {
namespace {

constexpr std::uint32_t kCookieNoRuns = 12346;  // SERIAL_COOKIE_NO_RUNCONTAINER
constexpr std::uint32_t kCookie = 12347;        // SERIAL_COOKIE: run flags follow
constexpr std::uint32_t kNoOffsetThreshold = 4;  // fewer containers than this: no offset header (kCookie)
constexpr std::uint32_t kArrayMax = 4096;        // larger containers are 8 KiB bitmaps
constexpr std::size_t kBitmapBytes = 8192;

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;

    bool u16(std::uint16_t& v) {
        if (size - at < 2U) {
            return false;
        }
        v = static_cast<std::uint16_t>(data[at] | (data[at + 1U] << 8U));
        at += 2U;
        return true;
    }
    bool u32(std::uint32_t& v) {
        if (size - at < 4U) {
            return false;
        }
        v = static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1U]) << 8U) |
            (static_cast<std::uint32_t>(data[at + 2U]) << 16U) | (static_cast<std::uint32_t>(data[at + 3U]) << 24U);
        at += 4U;
        return true;
    }
    bool skip(std::size_t n) {
        if (size - at < n) {
            return false;
        }
        at += n;
        return true;
    }
};

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFFU);
    put16(out, v >> 16U);
}

}  // namespace

bool decode(const std::uint8_t* data, std::size_t size, std::vector<std::uint32_t>& out, std::string& error) {
    out.clear();
    Reader r{data, size};
    std::uint32_t cookie = 0;
    if (!r.u32(cookie)) {
        error = "roaring bitmap: no cookie";
        return false;
    }
    std::uint32_t containers = 0;
    std::vector<std::uint8_t> run_flags;
    bool offsets = true;
    if (cookie == kCookieNoRuns) {
        if (!r.u32(containers)) {
            error = "roaring bitmap: no container count";
            return false;
        }
    } else if ((cookie & 0xFFFFU) == kCookie) {
        containers = (cookie >> 16U) + 1U;
        const std::size_t flag_bytes = (containers + 7U) / 8U;
        if (size - r.at < flag_bytes) {
            error = "roaring bitmap: truncated run flags";
            return false;
        }
        run_flags.assign(data + r.at, data + r.at + flag_bytes);
        r.at += flag_bytes;
        offsets = containers >= kNoOffsetThreshold;
    } else {
        error = "roaring bitmap: unknown cookie " + std::to_string(cookie);
        return false;
    }
    // 2^16 containers cover every 32-bit value; and each needs at least its 4-byte header.
    if (containers > 65536U || containers > (size - r.at) / 4U) {
        error = "roaring bitmap: implausible container count";
        return false;
    }
    std::vector<std::uint16_t> keys(containers);
    std::vector<std::uint32_t> cardinalities(containers);
    for (std::uint32_t c = 0; c < containers; ++c) {
        std::uint16_t card_minus_one = 0;
        if (!r.u16(keys[c]) || !r.u16(card_minus_one)) {
            error = "roaring bitmap: truncated header";
            return false;
        }
        if (c != 0U && keys[c] <= keys[c - 1U]) {
            error = "roaring bitmap: container keys out of order";
            return false;
        }
        cardinalities[c] = static_cast<std::uint32_t>(card_minus_one) + 1U;
    }
    if (offsets && !r.skip(static_cast<std::size_t>(containers) * 4U)) {
        error = "roaring bitmap: truncated offsets";
        return false;
    }
    for (std::uint32_t c = 0; c < containers; ++c) {
        const std::uint32_t high = static_cast<std::uint32_t>(keys[c]) << 16U;
        const bool run = !run_flags.empty() && (run_flags[c / 8U] & (1U << (c % 8U))) != 0U;
        const auto first = out.size();
        if (run) {
            std::uint16_t runs = 0;
            if (!r.u16(runs)) {
                error = "roaring bitmap: truncated run container";
                return false;
            }
            std::uint32_t previous_end = 0;
            for (std::uint16_t k = 0; k < runs; ++k) {
                std::uint16_t start = 0;
                std::uint16_t length_minus_one = 0;
                if (!r.u16(start) || !r.u16(length_minus_one)) {
                    error = "roaring bitmap: truncated run container";
                    return false;
                }
                const std::uint32_t end = static_cast<std::uint32_t>(start) + length_minus_one;
                if (end > 0xFFFFU || (k != 0U && start <= previous_end)) {
                    error = "roaring bitmap: malformed run";
                    return false;
                }
                for (std::uint32_t v = start; v <= end; ++v) {
                    out.push_back(high | v);
                }
                previous_end = end;
            }
        } else if (cardinalities[c] <= kArrayMax) {
            for (std::uint32_t k = 0; k < cardinalities[c]; ++k) {
                std::uint16_t v = 0;
                if (!r.u16(v)) {
                    error = "roaring bitmap: truncated array container";
                    return false;
                }
                if (k != 0U && (high | v) <= out.back()) {
                    error = "roaring bitmap: array container out of order";
                    return false;
                }
                out.push_back(high | v);
            }
        } else {
            if (size - r.at < kBitmapBytes) {
                error = "roaring bitmap: truncated bitmap container";
                return false;
            }
            for (std::uint32_t v = 0; v < 65536U; ++v) {
                if ((data[r.at + v / 8U] & (1U << (v % 8U))) != 0U) {
                    out.push_back(high | v);
                }
            }
            r.at += kBitmapBytes;
        }
        if (out.size() - first != cardinalities[c]) {
            error = "roaring bitmap: a container holds other than its declared cardinality";
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> encode(const std::vector<std::uint32_t>& ids) {
    // Containers: runs of ids sharing their high 16 bits.
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (std::size_t i = 0; i < ids.size();) {
        std::size_t j = i + 1U;
        while (j < ids.size() && (ids[j] >> 16U) == (ids[i] >> 16U)) {
            ++j;
        }
        spans.emplace_back(i, j);
        i = j;
    }
    std::vector<std::uint8_t> out;
    put32(out, kCookieNoRuns);
    put32(out, static_cast<std::uint32_t>(spans.size()));
    for (const auto& [b, e] : spans) {
        put16(out, ids[b] >> 16U);
        put16(out, static_cast<std::uint32_t>(e - b - 1U));
    }
    // Offsets: where each container starts, from the start of the serialization.
    std::size_t at = out.size() + spans.size() * 4U;
    for (const auto& [b, e] : spans) {
        put32(out, static_cast<std::uint32_t>(at));
        at += e - b <= kArrayMax ? (e - b) * 2U : kBitmapBytes;
    }
    for (const auto& [b, e] : spans) {
        if (e - b <= kArrayMax) {
            for (auto k = b; k < e; ++k) {
                put16(out, ids[k] & 0xFFFFU);
            }
        } else {
            std::vector<std::uint8_t> bits(kBitmapBytes, 0U);
            for (auto k = b; k < e; ++k) {
                const auto v = ids[k] & 0xFFFFU;
                bits[v / 8U] = static_cast<std::uint8_t>(bits[v / 8U] | (1U << (v % 8U)));
            }
            out.insert(out.end(), bits.begin(), bits.end());
        }
    }
    return out;
}

}  // namespace nano_lance::roaring
