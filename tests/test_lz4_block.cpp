// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// LZ4 block decompression. The compressed vectors below were produced by the REFERENCE liblz4 (via
// python-lz4's `lz4.block.compress(..., store_size=True)`, which is the same `[u32 LE size][block]`
// framing the Rust `lz4` crate's prepend_size mode writes, and therefore the same thing Lance
// stores). That makes these a differential test against the real encoder rather than against my own
// reading of the spec. The malformed cases after them are hand-built, because no encoder produces
// one.
//
// End-to-end coverage -- a low-cardinality string column pylance actually wrote, whose dictionary
// block is LZ4 -- lives in bindings/python/tests/test_lance_parity.py.

#include "nanolance/lz4_block.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace lz4 = nano_lance::lz4_block;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2U);
    for (std::size_t i = 0; i + 1U < hex.size(); i += 2U) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(std::string(hex.substr(i, 2U)), nullptr, 16)));
    }
    return out;
}

/// `pattern` repeated `times` over. Keeps the large vectors' EXPECTED bytes out of the source: the
/// compressed side stays a literal from liblz4, which is the half that has to be exact.
std::vector<std::uint8_t> repeat(std::string_view pattern, std::size_t times) {
    std::vector<std::uint8_t> out;
    out.reserve(pattern.size() * times);
    for (std::size_t i = 0; i < times; ++i) {
        out.insert(out.end(), pattern.begin(), pattern.end());
    }
    return out;
}

void check(const char* what, std::string_view compressed_hex, const std::vector<std::uint8_t>& expected) {
    std::vector<std::uint8_t> out;
    std::string error;
    require(lz4::decompress_sized(from_hex(compressed_hex), out, error), std::string(what) + ": " + error);
    require(out == expected, std::string(what) + ": decoded bytes differ");
}

void reference_vectors_round_trip() {
    check("an empty buffer",
          "0000000000",
          from_hex(""));
    check("a buffer too short to compress",
          "050000005068656c6c6f",
          from_hex("68656c6c6f"));
    check("a repetitive buffer (matches)",
          "a8020000ff02616c70686120626574612067616d6d61201100ffff8150616d6d6120",
          from_hex("616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120616c70686120626574612067616d6d6120"));
    check("incompressible bytes (all literals, extended length)",
          "2c010000f0ff1e4420823cfde6f1c26b30f90ec7dd01e4887534a20f0b0d04c36ed80e71e0fd77b07670eb940bd5335f973daad8619b91ffc911f57cced458bbbf2ce03753c9bdfa0ff0169dc9575674066676cfb0b4eb8902c44269da1cf6ba66d3f8b6d4b100a9ea0e755a5c2e8210242a08e7078f7f89385eb09423555182568b96e8a4fef23a0c9fc5afd7608437816bdd0a7309cb4a1252e4da70e6720fcaa4da1e98406c189c24279e9851d5814204136feb5713c166b13269dd63fc35c797ff08a6cd90095066a745addb6d8831c2b0f87821142b4456556d89aa82bcadae3a9578fa4535a414d025c24b40ae3ac127722988ba973aea8d37179706072ed33a14607ad7523be6557b5134dec19681f4a1336aa2140d0597a3e6c8a0cc2020a2e939806ef0b6845d6a9d657eb8298f2d",
          from_hex("4420823cfde6f1c26b30f90ec7dd01e4887534a20f0b0d04c36ed80e71e0fd77b07670eb940bd5335f973daad8619b91ffc911f57cced458bbbf2ce03753c9bdfa0ff0169dc9575674066676cfb0b4eb8902c44269da1cf6ba66d3f8b6d4b100a9ea0e755a5c2e8210242a08e7078f7f89385eb09423555182568b96e8a4fef23a0c9fc5afd7608437816bdd0a7309cb4a1252e4da70e6720fcaa4da1e98406c189c24279e9851d5814204136feb5713c166b13269dd63fc35c797ff08a6cd90095066a745addb6d8831c2b0f87821142b4456556d89aa82bcadae3a9578fa4535a414d025c24b40ae3ac127722988ba973aea8d37179706072ed33a14607ad7523be6557b5134dec19681f4a1336aa2140d0597a3e6c8a0cc2020a2e939806ef0b6845d6a9d657eb8298f2d"));
    check("a dictionary-shaped mix of runs and noise",
          "58020000bf63617465676f72795f37200b00ffff0af023e52ead74c79d15a75fa29b7dab332f7d700a7ccd258924260b0594b7fcf04e33a727585b4c48a39c369640694810a1695b99",
          from_hex("63617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f372063617465676f72795f3720e52ead74c79d15a75fa29b7dab332f7d700a7ccd258924260b0594b7fcf04e33a727585b4c48a39c369640694810a1695b99"));
    check("a single-byte run (overlapping matches)",
          "e80300001f610100ffffffd2506161616161",
          repeat("a", 1000));
    check("a two-byte pattern long enough to need several extended lengths",
          "803801002f78790200ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9f507978797879",
          repeat("xy", 40000));
}

void the_output_buffer_is_replaced_not_appended() {
    // Callers reuse one scratch buffer across pages; a decode that appended would concatenate them.
    std::vector<std::uint8_t> out{'j', 'u', 'n', 'k'};
    std::string error;
    require(lz4::decompress_sized({5, 0, 0, 0, 0x50, 'h', 'e', 'l', 'l', 'o'}, out, error), error);
    require(std::string(out.begin(), out.end()) == "hello", "the buffer is replaced, not appended to");
}

void malformed_blocks_are_refused() {
    std::vector<std::uint8_t> out;
    std::string error;

    require(!lz4::decompress_sized({0x01, 0x02}, out, error), "a buffer too short for the size header");

    // Declares 5 bytes, delivers a token asking for 5 literals but only 3 of them.
    require(!lz4::decompress_sized({5, 0, 0, 0, 0x50, 'a', 'b', 'c'}, out, error),
            "a literal run past the end of the block is refused");
    require(error.find("literal") != std::string::npos, "the refusal names the literal run");

    // Offset 0 is not a legal back-reference.
    error.clear();
    require(!lz4::decompress_sized({8, 0, 0, 0, 0x10, 'a', 0x00, 0x00}, out, error),
            "a zero match offset is refused");

    // Offset 2 with only one byte decoded so far points before the start of the output.
    error.clear();
    require(!lz4::decompress_sized({8, 0, 0, 0, 0x10, 'a', 0x02, 0x00}, out, error),
            "a match offset reaching before the output start is refused");
    require(error.find("offset") != std::string::npos, "the refusal names the offset");

    // A well-formed block that simply decodes to fewer bytes than declared.
    error.clear();
    require(!lz4::decompress_sized({99, 0, 0, 0, 0x30, 'a', 'b', 'c'}, out, error),
            "a block that decodes short of its declared size is refused");
    require(error.find("declared") != std::string::npos, "the refusal names the declared size");

    // An extended literal length that never terminates before the block ends.
    error.clear();
    require(!lz4::decompress_sized({4, 0, 0, 0, 0xF0, 0xFF, 0xFF}, out, error),
            "an unterminated extended length is refused");

    // A token whose match length runs past the declared size.
    error.clear();
    require(!lz4::decompress_sized({4, 0, 0, 0, 0x10, 'a', 0x01, 0x00}, out, error),
            "a match past the declared uncompressed size is refused");
}

void a_run_is_spelled_as_an_overlapping_match() {
    // offset 1 with a long match is how LZ4 encodes a run; decoding it needs the byte-at-a-time copy,
    // because a bulk copy would read bytes the same loop is still writing.
    std::vector<std::uint8_t> out;
    std::string error;
    require(lz4::decompress_sized({16, 0, 0, 0, 0x1B, 'z', 0x01, 0x00}, out, error), error);
    require(out == std::vector<std::uint8_t>(16, 'z'), "offset-1 match expands to a run");
}

}  // namespace

int main() {
    reference_vectors_round_trip();
    the_output_buffer_is_replaced_not_appended();
    malformed_blocks_are_refused();
    a_run_is_spelled_as_an_overlapping_match();
    std::cout << "lz4 block tests passed\n";
    return 0;
}
