// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Lance's LZ4 envelope over nanom's LZ4 block decoder (nanom/codec.hpp), the one parquet2nanoarrow
// reads LZ4_RAW pages with. This file keeps only what is Lance's: the [u32 size] prefix and the size
// checks made before anything is allocated.

#include "nanolance/lz4_block.hpp"

#include "nanolance/read_safety.hpp"

#include <nanom/codec.hpp>

#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string>

namespace nano_lance::lz4_block {

bool decompress_block(const std::uint8_t* data, std::size_t size, std::size_t uncompressed_size,
                      std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    // The declared size decides the allocation, so it has to be checked against what this block could
    // possibly produce BEFORE allocating -- otherwise an 8-byte buffer declaring 3.7 GB makes the
    // reader allocate 3.7 GB and the read-limit budget (8 GiB per decoded buffer) never fires. Found
    // by tests/fuzz/fuzz_lz4.cpp, 38 executions in.
    //
    // The bound is exact enough to be safe: literals are copied 1:1 from the block, and a match
    // costs at least one extension byte per 255 output bytes, so no LZ4 block can expand by more
    // than 255x plus a constant.
    const std::size_t max_possible =
        size > (SIZE_MAX / 255U) - 1U ? SIZE_MAX : (size + 1U) * 255U;
    if (uncompressed_size > max_possible) {
        error = "LZ4 block declares " + std::to_string(uncompressed_size) +
                " uncompressed bytes, more than its " + std::to_string(size) +
                " compressed bytes could produce";
        return false;
    }
    if (size == 0U) {  // nothing to decode: valid only when nothing was promised
        if (uncompressed_size != 0U) {
            error = "LZ4 block is empty but declares " + std::to_string(uncompressed_size) + " bytes";
            return false;
        }
        return true;
    }
    out.resize(uncompressed_size);
    const auto produced = ::nanom::codec::lz4_block_decompress(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size),
        std::span<std::byte>(reinterpret_cast<std::byte*>(out.data()), out.size()));
    if (!produced) {
        error = std::string("LZ4 block is malformed: ") + produced.error().what + " at byte " +
                std::to_string(produced.error().at);
        out.clear();
        return false;
    }
    if (*produced != uncompressed_size) {
        error = "LZ4 block decoded " + std::to_string(*produced) + " bytes, not the declared " +
                std::to_string(uncompressed_size);
        out.clear();
        return false;
    }
    return true;
}

bool decompress_sized(const std::vector<std::uint8_t>& sized, std::vector<std::uint8_t>& out,
                      std::string& error) {
    if (sized.size() < 4U) {
        error = "LZ4 buffer is too short to hold its uncompressed size";
        return false;
    }
    const auto declared = load_le<std::uint32_t>(sized.data());
    if (declared > default_read_limits().max_uncompressed_bytes) {
        error = "LZ4 buffer declares " + std::to_string(declared) +
                " uncompressed bytes, over the decoded-size limit";
        return false;
    }
    return decompress_block(sized.data() + 4U, sized.size() - 4U, static_cast<std::size_t>(declared),
                            out, error);
}

// ---- compression -----------------------------------------------------------------------------------
//
// The format (https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md): a sequence of tokens, each
// [literal-length nibble | match-length-minus-4 nibble], a length extension (255-bytes, then a final
// byte < 255) for a nibble of 15, the literals, and a 2-byte little-endian back-reference offset. The
// last sequence has literals only. Two rules bound the matches: a match starts at least 12 bytes before
// the end, and the last 5 bytes are always literals (mflimit and last_literals in the reference code).
// Matching is greedy: the first 4-byte sequence seen in a hash table of earlier positions that still
// matches and lies within 65535 bytes wins. No entropy coding, so the output is larger than lz4-rs's
// on some inputs; decoders do not care.

namespace {

constexpr std::size_t kMinMatch = 4;
constexpr std::size_t kMfLimit = 12;     // a match may not start within the last 12 bytes
constexpr std::size_t kLastLiterals = 5;  // a match may not extend into the last 5 bytes
constexpr std::size_t kMaxOffset = 65535;
constexpr unsigned kHashBits = 16;

std::uint32_t load32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof v);  // memcpy: no alignment or aliasing assumption, on any target
    return v;
}

std::uint32_t hash4(std::uint32_t v) {
    // The multiplicative hash of the reference implementation, keeping the top kHashBits bits.
    return (v * 2654435761U) >> (32U - kHashBits);
}

void put_length(std::vector<std::uint8_t>& out, std::size_t extra) {
    // The 15 in a nibble stands for 15 plus a run of 255s, closed by a byte below 255.
    while (extra >= 255U) {
        out.push_back(255U);
        extra -= 255U;
    }
    out.push_back(static_cast<std::uint8_t>(extra));
}

void put_literals_and_match(std::vector<std::uint8_t>& out, const std::uint8_t* lit, std::size_t lit_len,
                            bool has_match, std::size_t offset, std::size_t match_len) {
    const std::size_t lit_nibble = lit_len < 15U ? lit_len : 15U;
    const std::size_t match_nibble = has_match ? (match_len - kMinMatch < 15U ? match_len - kMinMatch : 15U) : 0U;
    out.push_back(static_cast<std::uint8_t>((lit_nibble << 4U) | match_nibble));
    if (lit_len >= 15U) {
        put_length(out, lit_len - 15U);
    }
    out.insert(out.end(), lit, lit + lit_len);
    if (!has_match) {
        return;  // the last sequence of a block carries no match
    }
    out.push_back(static_cast<std::uint8_t>(offset & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((offset >> 8U) & 0xFFU));
    if (match_len - kMinMatch >= 15U) {
        put_length(out, match_len - kMinMatch - 15U);
    }
}

}  // namespace

void compress_block(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    out.clear();
    // Too short for any match: one literal-only sequence. An empty input is the one-byte block the
    // reference encoder writes too (a zero token), which decompress_block accepts for size 0.
    if (size < kMfLimit + 1U) {
        put_literals_and_match(out, data, size, false, 0U, 0U);
        return;
    }
    std::vector<std::uint32_t> table(std::size_t{1} << kHashBits, 0U);
    const std::size_t match_start_limit = size - kMfLimit;     // no match starts at or after this
    const std::size_t match_end_limit = size - kLastLiterals;  // no match ends after this
    std::size_t anchor = 0;  // first byte not yet written as part of a sequence
    std::size_t ip = 1;      // position 0 has no earlier occurrence to match against
    while (ip < match_start_limit) {
        const std::uint32_t seq = load32(data + ip);
        const std::size_t slot = hash4(seq);
        const std::size_t candidate = table[slot];
        table[slot] = static_cast<std::uint32_t>(ip);
        if (candidate >= ip || ip - candidate > kMaxOffset || load32(data + candidate) != seq) {
            ++ip;
            continue;
        }
        std::size_t match_end = ip + kMinMatch;
        std::size_t ref_end = candidate + kMinMatch;
        while (match_end < match_end_limit && data[match_end] == data[ref_end]) {
            ++match_end;
            ++ref_end;
        }
        put_literals_and_match(out, data + anchor, ip - anchor, true, ip - candidate, match_end - ip);
        // Index the positions the match covers, so the next search can start from anywhere in it.
        for (std::size_t p = ip + 1; p < match_end && p < match_start_limit; p += 2) {
            table[hash4(load32(data + p))] = static_cast<std::uint32_t>(p);
        }
        ip = match_end;
        anchor = match_end;
    }
    put_literals_and_match(out, data + anchor, size - anchor, false, 0U, 0U);
}

bool compress_sized(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out,
                    std::string& error) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        error = "LZ4 input of " + std::to_string(size) + " bytes does not fit the u32 size prefix";
        return false;
    }
    std::vector<std::uint8_t> block;
    compress_block(data, size, block);
    out.clear();
    out.reserve(4U + block.size());
    const auto declared = static_cast<std::uint32_t>(size);
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>((declared >> shift) & 0xFFU));
    }
    out.insert(out.end(), block.begin(), block.end());
    return true;
}

}  // namespace nano_lance::lz4_block
