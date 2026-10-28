// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer encoding: how one page of one column is laid out, in both directions. Each encoding is a module,
// values/<name>.{hpp,cpp} or layout/<name>.{hpp,cpp}, with encode_* and decode_* as pure functions on buffers, so
// an example can run one in memory. This example bit-packs a page of int32 values and decodes it back.
//
// Lance's bitpacking works in blocks of up to 1024 values. A block is its bit width as one 4-byte word, then the
// packed values. Blocks follow each other in a page. The module has no public header yet (the file layer is its
// caller), so this example includes it from src/ through NANOLANCE_PRIVATE_INCLUDE, as the layer's tests do.
//
// Run it with: ctest --test-dir build -R nano_lance_example_encoding --output-on-failure

#include "encoding/values/bitpack.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Prints the failed check and returns the exit code.
int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

}  // namespace

int main() {
    // Step 1: a page of 2500 int32 values between 0 and 999, so each value needs 10 bits. 2500 values make
    // three blocks: 1024, 1024 and 452.
    const std::size_t count = 2500;
    const std::size_t width = sizeof(std::int32_t);
    std::vector<std::int32_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<std::int32_t>((i * 7919U) % 1000U);
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(values.data());

    // Step 2: encode block by block. encode_block overwrites its output, so each block goes to its own
    // buffer, and the blocks are then joined in order.
    std::vector<std::uint8_t> encoded;
    for (std::size_t start = 0; start < count; start += 1024) {
        const std::size_t n = std::min<std::size_t>(1024, count - start);
        std::vector<std::uint8_t> block;
        nano_lance::bitpack::encode_block(raw + start * width, n, width, block);
        encoded.insert(encoded.end(), block.begin(), block.end());
    }
    const std::size_t raw_bytes = count * width;
    std::cout << "page: " << raw_bytes << " bytes raw, " << encoded.size() << " bytes bit-packed\n";
    if (encoded.size() >= raw_bytes) {
        return fail("the bit-packed page should be smaller than the raw values");
    }

    // Step 3: decode the blocks back into values. decode_series reads consecutive blocks, each with its own width.
    std::vector<std::uint8_t> decoded;
    std::string error;
    if (!nano_lance::bitpack::decode_series(encoded, count, width, decoded, error)) {
        return fail("decode_series: " + error);
    }
    if (decoded.size() != raw_bytes) {
        return fail("decoded " + std::to_string(decoded.size()) + " bytes, want " + std::to_string(raw_bytes));
    }

    // Step 4: every value comes back as it went in.
    for (std::size_t i = 0; i < count; ++i) {
        std::int32_t value = 0;
        std::memcpy(&value, decoded.data() + i * width, width);
        if (value != values[i]) {
            return fail("value " + std::to_string(i) + " is " + std::to_string(value) + ", want " +
                        std::to_string(values[i]));
        }
    }
    std::cout << "decoded all " << count << " values back, equal to the input\n";
    return 0;
}
