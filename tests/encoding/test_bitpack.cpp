// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// encoding/values/bitpack: Lance's bitpacking, encoder and decoder as a pair. Every value width
// (1, 2, 4, 8 bytes) at the bit widths around its edges, blocks of 0, 1, 1023 and 1024 values, a series
// of blocks of different widths, out-of-line buffers with a packed and a raw tail, and the malformed
// buffers the decoders must refuse (the read path's input is untrusted).

#include "encoding/values/bitpack.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <nanom/columnar_encode.hpp>
#include <nanom/fastlanes.hpp>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

/// `count` little-endian values of `width_bytes`, each below 2^bits (bits <= 8 * width_bytes).
std::vector<std::uint8_t> values(std::size_t count, std::size_t width_bytes, unsigned bits, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::uint8_t> out(count * width_bytes);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t v = bits == 0U ? 0U : rng() >> (64U - bits);
        if (i == 0 && bits != 0U) {
            v = bits == 64U ? ~0ULL : (1ULL << bits) - 1U;  // one value at the maximum, so the width is exact
        }
        std::memcpy(out.data() + i * width_bytes, &v, width_bytes);
    }
    return out;
}

std::string name(std::size_t width_bytes, unsigned bits, std::size_t count) {
    return std::to_string(width_bytes * 8U) + "-bit values at " + std::to_string(bits) + " bits, " +
           std::to_string(count) + " values";
}

void test_block_round_trip() {
    for (const std::size_t w : {1U, 2U, 4U, 8U}) {
        for (const unsigned bits : {0U, 1U, 2U, 7U, static_cast<unsigned>(w * 4U), static_cast<unsigned>(w * 8U - 1U),
                                    static_cast<unsigned>(w * 8U)}) {
            for (const std::size_t n : {0U, 1U, 5U, 1023U, 1024U}) {
                const auto in = values(n, w, bits, bits * 131U + n);
                const auto what = name(w, bits, n);
                require(nano_lance::bitpack::bit_width(in.data(), n, w) == (n == 0U ? 0U : bits), "bit_width, " + what);
                std::vector<std::uint8_t> block;
                nano_lance::bitpack::encode_block(in.data(), n, w, block);
                // [width word][packed words]: the size follows from the width alone.
                const unsigned width = n == 0U ? 0U : bits;
                require(block.size() == w + (width * 1024U / 8U), "block size, " + what);
                std::vector<std::uint8_t> out;
                std::string error;
                require(nano_lance::bitpack::decode_block(block, n, w, out, error), "decode_block, " + what + ": " + error);
                require(out == in, "round trip, " + what);
                // Decoding appends: a second decode doubles the output.
                require(nano_lance::bitpack::decode_block(block, n, w, out, error) && out.size() == 2U * in.size(),
                        "decode_block appends, " + what);
            }
        }
    }
}

void test_block_reuses_output_buffer() {
    // encode_block overwrites every byte of `out`: a long block then a short one leaves only the short one.
    const auto wide = values(1024, 4, 31, 1);
    const auto narrow = values(10, 4, 3, 2);
    std::vector<std::uint8_t> block;
    nano_lance::bitpack::encode_block(wide.data(), 1024, 4, block);
    nano_lance::bitpack::encode_block(narrow.data(), 10, 4, block);
    std::vector<std::uint8_t> out;
    std::string error;
    require(nano_lance::bitpack::decode_block(block, 10, 4, out, error) && out == narrow, "reused output buffer");
}

void test_block_refuses_malformed() {
    const auto in = values(100, 2, 9, 3);
    std::vector<std::uint8_t> block;
    nano_lance::bitpack::encode_block(in.data(), 100, 2, block);
    std::vector<std::uint8_t> out;
    std::string error;
    require(!nano_lance::bitpack::decode_block(std::vector<std::uint8_t>{1}, 1, 2, out, error) &&
                error.find("shorter") != std::string::npos,
            "a block shorter than its width word is refused");
    auto wide = block;
    wide[0] = 17;  // wider than a 16-bit value
    require(!nano_lance::bitpack::decode_block(wide, 100, 2, out, error) && error.find("invalid bit width") != std::string::npos,
            "a width wider than the value is refused");
    auto cut = block;
    cut.pop_back();
    require(!nano_lance::bitpack::decode_block(cut, 100, 2, out, error) && error.find("does not match") != std::string::npos,
            "a truncated block is refused");
    require(!nano_lance::bitpack::decode_block(block, 1025, 2, out, error) && error.find("exceeds") != std::string::npos,
            "more than 1024 values from one block is refused");
}

void test_series() {
    // Three blocks of different widths, as a dictionary buffer holds them; the last one partly used.
    for (const std::size_t w : {1U, 2U, 4U, 8U}) {
        const unsigned top = static_cast<unsigned>(w * 8U);
        const auto a = values(1024, w, top, 4);
        const auto b = values(1024, w, 1, 5);
        const auto c = values(300, w, top / 2U, 6);
        std::vector<std::uint8_t> buffer;
        std::vector<std::uint8_t> block;
        for (const auto* part : {&a, &b, &c}) {
            nano_lance::bitpack::encode_block(part->data(), part->size() / w, w, block);
            buffer.insert(buffer.end(), block.begin(), block.end());
        }
        std::vector<std::uint8_t> expected = a;
        expected.insert(expected.end(), b.begin(), b.end());
        expected.insert(expected.end(), c.begin(), c.end());
        std::vector<std::uint8_t> out;
        std::string error;
        const auto what = std::to_string(w * 8U) + "-bit series";
        require(nano_lance::bitpack::decode_series(buffer, 2348, w, out, error), what + ": " + error);
        require(out == expected, what + " round trip");
        require(!nano_lance::bitpack::decode_series(buffer, 3073, w, out, error), what + ": asking for a fourth block is refused");
        auto cut = buffer;
        cut.resize(cut.size() - 1U);
        require(!nano_lance::bitpack::decode_series(cut, 2348, w, out, error), what + ": a truncated last block is refused");
    }
    std::vector<std::uint8_t> out;
    std::string error;
    require(!nano_lance::bitpack::decode_series(std::vector<std::uint8_t>(16, 0), 1, 16, out, error),
            "16-byte values have no FastLanes kernel");
}

/// An out-of-line buffer as Lance writes one: whole blocks packed at `width`, then the tail packed and
/// padded (raw_tail false) or as raw words.
template <class T>
std::vector<std::uint8_t> out_of_line(const std::vector<std::uint8_t>& in, unsigned width, bool raw_tail) {
    const std::size_t count = in.size() / sizeof(T);
    const auto words = ::nanom::columnar::fastlanes::packed_words_1024<T>(width);
    std::vector<std::uint8_t> out;
    std::vector<T> packed(words != 0U ? words : 1U);
    for (std::size_t at = 0; at < count; at += 1024U) {
        const std::size_t n = std::min<std::size_t>(1024U, count - at);
        if (n < 1024U && raw_tail) {
            out.insert(out.end(), in.begin() + at * sizeof(T), in.end());
            break;
        }
        T block[1024] = {};
        std::memcpy(block, in.data() + at * sizeof(T), n * sizeof(T));
        ::nanom::columnar::fastlanes::pack_1024_unchecked<T>(width, block, packed.data());
        const auto* p = reinterpret_cast<const std::uint8_t*>(packed.data());
        out.insert(out.end(), p, p + words * sizeof(T));
    }
    return out;
}

template <class T>
void out_of_line_case(unsigned width, std::size_t count, bool raw_tail) {
    const auto in = values(count, sizeof(T), width, width * 7U + count);
    const auto buffer = out_of_line<T>(in, width, raw_tail);
    std::vector<std::uint8_t> out(in.size());
    std::string error;
    const auto what = name(sizeof(T), width, count) + (raw_tail ? ", raw tail" : ", packed tail");
    require(nano_lance::bitpack::decode_out_of_line(buffer, count, width, sizeof(T), out.data(), "test buffer", error),
            "decode_out_of_line, " + what + ": " + error);
    require(out == in, "out-of-line round trip, " + what);
}

void test_out_of_line() {
    for (const bool raw : {false, true}) {
        out_of_line_case<std::uint16_t>(1, 1025, raw);  // one packed block plus a single tail value
        out_of_line_case<std::uint16_t>(9, 2500, raw);
        out_of_line_case<std::uint32_t>(17, 1500, raw);
        out_of_line_case<std::uint64_t>(40, 3000, raw);
        out_of_line_case<std::uint8_t>(3, 700, raw);
    }
    out_of_line_case<std::uint16_t>(0, 2048, false);  // width 0: an all-zero buffer packs to nothing
    out_of_line_case<std::uint32_t>(5, 2048, false);  // no tail at all

    std::vector<std::uint8_t> out(64);
    std::string error;
    require(!nano_lance::bitpack::decode_out_of_line(std::vector<std::uint8_t>(10), 4, 17, 2, out.data(), "levels", error) &&
                error.find("levels") == 0,
            "a width wider than the value is refused, naming the buffer");
    require(!nano_lance::bitpack::decode_out_of_line(std::vector<std::uint8_t>(3), 1, 4, 2, out.data(), "levels", error),
            "a buffer that is not whole words is refused");
    require(!nano_lance::bitpack::decode_out_of_line(std::vector<std::uint8_t>(64), 30, 4, 2, out.data(), "levels", error) &&
                error.find("expected") != std::string::npos,
            "a buffer of the wrong length for its count is refused");
    require(!nano_lance::bitpack::decode_out_of_line(std::vector<std::uint8_t>(64), 4, 4, 16, out.data(), "levels", error),
            "16-byte values have no FastLanes kernel");
}

/// encode_out_of_line is the inverse of decode_out_of_line: the same values come back, at each width.
void test_encode_out_of_line() {
    const struct {
        std::size_t width_bytes;
        unsigned width;
        std::size_t count;
    } cases[] = {{8, 40, 3000}, {8, 63, 2500}, {8, 1, 1100}, {4, 17, 1500}, {2, 9, 2500}, {1, 3, 700}, {8, 40, 1024}};
    for (const auto& c : cases) {
        const auto in = values(c.count, c.width_bytes, c.width, c.width * 11U + c.count);
        std::vector<std::uint8_t> buffer;
        nano_lance::bitpack::encode_out_of_line(in.data(), c.count, c.width, c.width_bytes, buffer);
        std::vector<std::uint8_t> out(in.size());
        std::string error;
        const auto what = name(c.width_bytes, c.width, c.count);
        require(nano_lance::bitpack::decode_out_of_line(buffer, c.count, c.width, c.width_bytes, out.data(), "dict",
                                                        error),
                "decode after encode, " + what + ": " + error);
        require(out == in, "encode_out_of_line round trip, " + what);
    }
    // One width for every block: no per-block header, so the size is blocks x packed words.
    std::vector<std::uint8_t> buffer;
    const auto in = values(2500, 8, 40, 7U);
    nano_lance::bitpack::encode_out_of_line(in.data(), 2500, 40, 8, buffer);
    require(buffer.size() == 3U * ::nanom::columnar::fastlanes::packed_words_1024<std::uint64_t>(40) * 8U,
            "three blocks of 40-bit values, no headers");
}

}  // namespace

int main() {
    test_block_round_trip();
    test_block_reuses_output_buffer();
    test_block_refuses_malformed();
    test_series();
    test_out_of_line();
    test_encode_out_of_line();
    std::cout << "encoding/values/bitpack: every block, series and out-of-line case round-trips\n";
    return 0;
}
