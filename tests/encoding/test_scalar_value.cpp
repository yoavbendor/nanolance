// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// encoding/values/scalar_value: the inline value of a constant page, encode then decode for the empty value,
// 1 byte, 1000 bytes and a value with zero bytes inside, with 32-bit and 64-bit offsets; the exact bytes of a
// small value; the one-buffer spelling the decoder also reads; and the refusals (a truncated buffer, a
// length that runs past the buffer, a buffer count that is neither 1 nor 2).

#include "encoding/values/scalar_value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;

Bytes patterned(std::size_t n) {
    Bytes out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>((i * 37U + 11U) & 0xFFU);
    }
    return out;
}

void store_u32_le(Bytes& bytes, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4U; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>((v >> (8U * i)) & 0xFFU);
    }
}

void test_round_trip() {
    const std::vector<Bytes> values = {
        {},
        {0x41U},
        patterned(1000),
        {0x00U, 0x41U, 0x00U, 0x00U, 0x42U, 0x00U},
    };
    for (const bool large : {false, true}) {
        for (const auto& value : values) {
            const auto what = std::string(large ? "large" : "small") + " offsets, " + std::to_string(value.size()) +
                              " bytes";
            const auto buffer = nano_lance::scalar_value::encode_variable(value, large);
            Bytes back;
            std::string error;
            require(nano_lance::scalar_value::decode_variable(buffer, back, error), what + ": decode: " + error);
            require(back == value, what + ": round trip");
        }
    }
}

void test_exact_bytes() {
    // [u32 2 buffers][u32 offsets length][u32 data length][offsets: 0, 3][data]
    const Bytes value = {0xAAU, 0x00U, 0xBBU};
    const Bytes small = nano_lance::scalar_value::encode_variable(value, false);
    const Bytes expected_small = {0x02U, 0, 0, 0,  0x08U, 0, 0, 0,  0x03U, 0, 0, 0,  // header
                                  0x00U, 0, 0, 0,  0x03U, 0, 0, 0,                   // u32 offsets [0, 3]
                                  0xAAU, 0x00U, 0xBBU};
    require(small == expected_small, "32-bit offsets: the exact bytes");
    const Bytes large = nano_lance::scalar_value::encode_variable(value, true);
    const Bytes expected_large = {0x02U, 0, 0, 0,  0x10U, 0, 0, 0,  0x03U, 0, 0, 0,  // header
                                  0x00U, 0, 0, 0, 0, 0, 0, 0,  0x03U, 0, 0, 0, 0, 0, 0, 0,  // u64 offsets [0, 3]
                                  0xAAU, 0x00U, 0xBBU};
    require(large == expected_large, "64-bit offsets: the exact bytes");
}

void test_one_buffer_spelling() {
    // A fixed-width value too wide to inline is one buffer: [u32 1][u32 length][the bytes].
    const Bytes buffer = {0x01U, 0, 0, 0, 0x04U, 0, 0, 0, 0x10U, 0x20U, 0x30U, 0x40U};
    Bytes out;
    std::string error;
    require(nano_lance::scalar_value::decode_variable(buffer, out, error), "one buffer decodes: " + error);
    require(out == Bytes({0x10U, 0x20U, 0x30U, 0x40U}), "one buffer gives back its bytes");
}

void test_refusals() {
    Bytes out;
    std::string error;
    require(!nano_lance::scalar_value::decode_variable(Bytes{0x02U, 0U, 0U}, out, error) &&
                error.find("shorter") != std::string::npos,
            "a buffer shorter than its header is refused");

    const auto value = patterned(1000);
    for (const bool large : {false, true}) {
        const auto what = std::string(large ? "large" : "small") + " offsets";
        const auto buffer = nano_lance::scalar_value::encode_variable(value, large);
        auto truncated = buffer;
        truncated.pop_back();
        require(!nano_lance::scalar_value::decode_variable(truncated, out, error),
                what + ": a buffer one byte short is refused");
        truncated.resize(buffer.size() / 2U);
        require(!nano_lance::scalar_value::decode_variable(truncated, out, error),
                what + ": a buffer cut in half is refused");

        auto long_data = buffer;
        store_u32_le(long_data, 8U, 1001U);  // the data length runs one byte past the buffer
        require(!nano_lance::scalar_value::decode_variable(long_data, out, error) &&
                    error.find("exceeds") != std::string::npos,
                what + ": a data length that runs past the buffer is refused");
        auto long_offsets = buffer;
        store_u32_le(long_offsets, 4U, 0xFFFFFFF0U);  // an offsets length far past the buffer
        require(!nano_lance::scalar_value::decode_variable(long_offsets, out, error),
                what + ": an offsets length past the buffer is refused");
    }

    require(!nano_lance::scalar_value::decode_variable(Bytes{0x03U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, out, error) &&
                error.find("buffers") != std::string::npos,
            "three buffers are refused");
    require(!nano_lance::scalar_value::decode_variable(Bytes{0x01U, 0, 0, 0, 0x08U, 0, 0, 0, 0x01U}, out, error),
            "a one-buffer length past the buffer is refused");
}

}  // namespace

int main() {
    test_round_trip();
    test_exact_bytes();
    test_one_buffer_spelling();
    test_refusals();
    std::cout << "encoding/values/scalar_value: all checks passed\n";
    return 0;
}
