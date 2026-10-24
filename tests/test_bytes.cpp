// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// bytes_of (base/bytes.hpp): a string's bytes as an unsigned byte vector, for schema metadata values. The
// bytes must be the string's exact contents, including embedded NULs and bytes above 0x7F (the metadata
// is binary-safe; a signed char conversion or a C-string constructor would corrupt both).

#include "base/bytes.hpp"

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

}  // namespace

int main() {
    require(nano_lance::bytes_of("").empty(), "the empty string has no bytes");

    const auto one = nano_lance::bytes_of("a");
    require(one == std::vector<std::uint8_t>{'a'}, "one character is one byte");

    const auto text = nano_lance::bytes_of("lance");
    require(text == std::vector<std::uint8_t>{'l', 'a', 'n', 'c', 'e'}, "ASCII bytes in order");

    // Embedded NUL: the length is the string's, not the position of the first zero.
    const std::string with_nul("a\0b", 3);
    require(nano_lance::bytes_of(with_nul) == std::vector<std::uint8_t>({'a', 0x00, 'b'}),
            "an embedded NUL is kept");

    // High bytes stay unsigned values 0x80..0xFF.
    const std::string high("\x80\xff\x01", 3);
    require(nano_lance::bytes_of(high) == std::vector<std::uint8_t>({0x80, 0xFF, 0x01}), "bytes above 0x7F are kept");

    // A long string: same length, same bytes.
    std::string big(100000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>(i * 31U);
    }
    const auto bytes = nano_lance::bytes_of(big);
    require(bytes.size() == big.size(), "the length is preserved");
    bool same = true;
    for (std::size_t i = 0; i < big.size(); ++i) {
        same = same && bytes[i] == static_cast<std::uint8_t>(big[i]);
    }
    require(same, "every byte is preserved");

    std::cout << "bytes: 6 checks passed\n";
    return 0;
}
