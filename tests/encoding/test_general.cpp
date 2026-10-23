// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// encoding/values/general: Lance's general buffer compression, zstd as a pair (frame, unframe) and the
// decompress entry point for both schemes. Empty, 1-byte, 1 MiB random and 1 MiB compressible buffers at
// zstd levels 1, 3 and 19; the frame's u64 size header; the unframe refusals (truncated, lying size, garbage);
// lz4 blocks (hand-built: nanolance/lz4_block.hpp decodes LZ4 and has no encoder, so the test writes a
// literal-only block from the LZ4 block format); and an unknown scheme.

#include "encoding/values/general.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::vector<std::uint8_t> random_bytes(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::uint8_t> out(n);
    for (auto& b : out) {
        b = static_cast<std::uint8_t>(rng());
    }
    return out;
}

/// Text-like and repetitive: zstd shrinks it a great deal.
std::vector<std::uint8_t> compressible_bytes(std::size_t n) {
    const std::string words = "the quick brown fox jumps over the lazy dog 0123456789\n";
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(words[(i / 3U) % words.size()]);
    }
    return out;
}

std::uint64_t load_u64_le(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8U; ++i) {
        v |= static_cast<std::uint64_t>(bytes[at + i]) << (8U * i);
    }
    return v;
}

void store_u64_le(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t v) {
    for (std::size_t i = 0; i < 8U; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>((v >> (8U * i)) & 0xFFU);
    }
}

/// A valid LZ4 block of literals only: the last sequence of a block has no match, so one token (literal
/// length in its high nibble, 15 meaning "plus extension bytes"), the extension bytes, then the literals.
std::vector<std::uint8_t> lz4_literal_block(const std::vector<std::uint8_t>& literals) {
    std::vector<std::uint8_t> block;
    const std::size_t n = literals.size();
    if (n < 15U) {
        block.push_back(static_cast<std::uint8_t>(n << 4U));
    } else {
        block.push_back(0xF0U);
        std::size_t rest = n - 15U;
        while (rest >= 255U) {
            block.push_back(255U);
            rest -= 255U;
        }
        block.push_back(static_cast<std::uint8_t>(rest));
    }
    block.insert(block.end(), literals.begin(), literals.end());
    return block;
}

/// `[u32 LE uncompressed size][LZ4 block]`, the wrapper decompress("lz4") reads.
std::vector<std::uint8_t> lz4_wrapped(const std::vector<std::uint8_t>& raw) {
    std::vector<std::uint8_t> out(4U, 0U);
    const auto size = static_cast<std::uint32_t>(raw.size());
    for (std::size_t i = 0; i < 4U; ++i) {
        out[i] = static_cast<std::uint8_t>((size >> (8U * i)) & 0xFFU);
    }
    const auto block = lz4_literal_block(raw);
    out.insert(out.end(), block.begin(), block.end());
    return out;
}

void test_zstd_round_trip() {
    const std::vector<std::vector<std::uint8_t>> inputs = {
        {},
        {0x5AU},
        random_bytes(1U << 20U, 1),
        compressible_bytes(1U << 20U),
    };
    for (const int level : {1, 3, 19}) {
        for (const auto& raw : inputs) {
            const auto name = "zstd level " + std::to_string(level) + ", " + std::to_string(raw.size()) + " bytes";
            std::vector<std::uint8_t> framed;
            std::string error;
            require(nano_lance::general::zstd_frame(raw, level, framed, error), name + ": frame: " + error);
            require(framed.size() > 8U, name + ": frame has a header and a body");
            require(load_u64_le(framed, 0) == raw.size(), name + ": header is the uncompressed size, u64 LE");
            std::vector<std::uint8_t> back;
            require(nano_lance::general::zstd_unframe(framed, back, error), name + ": unframe: " + error);
            require(back == raw, name + ": round trip");
        }
    }
    // A compressible megabyte really is compressed (a sanity check that the level is passed through).
    std::vector<std::uint8_t> framed;
    std::string error;
    require(nano_lance::general::zstd_frame(inputs[3], 19, framed, error) && framed.size() < inputs[3].size() / 10U,
            "a compressible megabyte compresses at level 19");
}

void test_unframe_refuses() {
    const auto raw = random_bytes(4096, 7);
    std::vector<std::uint8_t> framed;
    std::string error;
    require(nano_lance::general::zstd_frame(raw, 3, framed, error), "frame for refusal cases: " + error);
    std::vector<std::uint8_t> out;

    require(!nano_lance::general::zstd_unframe(std::vector<std::uint8_t>(7U, 0U), out, error) &&
                error.find("shorter") != std::string::npos,
            "a frame shorter than its size header is refused");

    auto truncated = framed;
    truncated.resize(8U + (framed.size() - 8U) / 2U);
    require(!nano_lance::general::zstd_unframe(truncated, out, error), "a truncated frame is refused");
    auto short_body = framed;
    short_body.pop_back();
    require(!nano_lance::general::zstd_unframe(short_body, out, error), "a frame one byte short is refused");

    auto one_more = framed;
    store_u64_le(one_more, 0, raw.size() + 1U);
    require(!nano_lance::general::zstd_unframe(one_more, out, error) &&
                error.find("content size") != std::string::npos,
            "a size header one too large is refused");
    auto huge = framed;
    store_u64_le(huge, 0, std::uint64_t{1} << 40U);
    require(!nano_lance::general::zstd_unframe(huge, out, error) &&
                error.find("more than zstd can produce") != std::string::npos,
            "a size header zstd cannot produce from the bytes is refused before any allocation");

    require(!nano_lance::general::zstd_unframe(random_bytes(100, 9), out, error),
            "random bytes are refused");
    auto garbage_body = framed;
    for (std::size_t i = 8U; i < garbage_body.size(); ++i) {
        garbage_body[i] = static_cast<std::uint8_t>(i * 31U);
    }
    require(!nano_lance::general::zstd_unframe(garbage_body, out, error), "a frame with a corrupt body is refused");
}

void test_decompress() {
    std::string error;
    std::vector<std::uint8_t> out;
    const auto raw = compressible_bytes(1U << 20U);
    std::vector<std::uint8_t> framed;
    require(nano_lance::general::zstd_frame(raw, 3, framed, error), "frame for decompress: " + error);
    require(nano_lance::general::decompress("zstd", framed.data(), framed.size(), out, error) && out == raw,
            "decompress with scheme zstd gives back the input");

    require(nano_lance::general::decompress("", raw.data(), raw.size(), out, error) && out == raw,
            "an empty scheme name stores the buffer as is");
    require(nano_lance::general::decompress("none", raw.data(), raw.size(), out, error) && out == raw,
            "scheme none stores the buffer as is");
    require(nano_lance::general::decompress("zstd", nullptr, 0U, out, error) && out.empty(),
            "an empty buffer stays empty");

    for (const std::size_t n : {std::size_t{1}, std::size_t{20}, std::size_t{300}, std::size_t{70000}}) {
        const auto literals = random_bytes(n, 11 + n);
        const auto wrapped = lz4_wrapped(literals);
        const auto what = "lz4, " + std::to_string(n) + " bytes";
        require(nano_lance::general::decompress("lz4", wrapped.data(), wrapped.size(), out, error) && out == literals,
                what + ": decompress gives back the input");
        auto lying = wrapped;  // the u32 size header says n + 1: the block decodes n bytes, so it is refused
        const auto declared = static_cast<std::uint32_t>(n + 1U);
        for (std::size_t i = 0; i < 4U; ++i) {
            lying[i] = static_cast<std::uint8_t>((declared >> (8U * i)) & 0xFFU);
        }
        require(!nano_lance::general::decompress("lz4", lying.data(), lying.size(), out, error),
                what + ": a size header that lies is refused");
    }

    require(!nano_lance::general::decompress("snappy", framed.data(), framed.size(), out, error) &&
                error.find("unsupported") != std::string::npos,
            "an unknown scheme is refused");
}

}  // namespace

int main() {
    test_zstd_round_trip();
    test_unframe_refuses();
    test_decompress();
    std::cout << "encoding/values/general: all checks passed\n";
    return 0;
}
