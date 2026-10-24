// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// LSB-first bool packing (nanolance/bool_bitpack.hpp): row i is bit i % 8 of byte i / 8, as Lance stores a
// bool column (Flat, 1 bit per value). Packing then unpacking gives the values back for every count around
// the 8-value boundaries, every output byte is written (no pre-zeroing needed), and unpacking at a bit
// offset that is not a multiple of 8 reads the right bits.

#include "nanolance/bool_bitpack.hpp"

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

std::vector<std::uint8_t> pattern(std::size_t n, unsigned seed) {
    std::vector<std::uint8_t> v(n);
    std::uint32_t x = seed * 2654435761U + 1U;
    for (auto& b : v) {
        x = x * 1103515245U + 12345U;
        b = static_cast<std::uint8_t>((x >> 16U) & 1U);
    }
    return v;
}

}  // namespace

int main() {
    namespace bp = nano_lance::boolpack;

    // The bit order: row 0 is the lowest bit of byte 0.
    {
        const std::uint8_t v[10] = {1, 0, 0, 0, 0, 0, 0, 1, 0, 1};
        const auto packed = bp::pack_lsb_first(v, 10);
        require(packed.size() == 2, "10 values take 2 bytes");
        require(packed[0] == 0x81U, "rows 0 and 7 are bits 0 and 7 of byte 0");
        require(packed[1] == 0x02U, "row 9 is bit 1 of byte 1, the unused bits zero");
    }

    // Round trips for every count from 0 to 80 (partial and full bytes).
    for (std::size_t n = 0; n <= 80; ++n) {
        const auto v = pattern(n, static_cast<unsigned>(n));
        // Pre-fill the output with ones: every byte must be written, not or'ed into.
        std::vector<std::uint8_t> packed((n + 7U) / 8U, 0xFFU);
        bp::pack_lsb_first(v.data(), n, packed.data());
        for (std::size_t i = 0; i < n; ++i) {
            require(((packed[i / 8U] >> (i % 8U)) & 1U) == v[i],
                    "packed bit " + std::to_string(i) + " of " + std::to_string(n));
        }
        if (n % 8U != 0U) {
            require((packed.back() >> (n % 8U)) == 0U, "the bits past the last value are zero");
        }
        std::vector<std::uint8_t> back(n, 0xAAU);
        bp::unpack_lsb_first(packed.data(), n, back.data());
        require(back == v, "unpack gives the values back, n=" + std::to_string(n));
    }

    // Unpacking a window at a bit offset (a page's rows after a skip).
    {
        const auto v = pattern(200, 7);
        const auto packed = bp::pack_lsb_first(v.data(), v.size());
        for (std::size_t offset : {0U, 1U, 3U, 7U, 8U, 9U, 63U, 64U, 101U}) {
            for (std::size_t count : {0U, 1U, 5U, 8U, 17U, 64U, 99U}) {
                if (offset + count > v.size()) {
                    continue;
                }
                std::vector<std::uint8_t> out(count);
                bp::unpack_lsb_first(packed.data(), offset, count, out.data());
                for (std::size_t i = 0; i < count; ++i) {
                    require(out[i] == v[offset + i],
                            "window at " + std::to_string(offset) + "+" + std::to_string(count));
                }
            }
        }
    }

    // The vector overload resizes to ceil(n / 8).
    {
        std::vector<std::uint8_t> out(100, 0);
        const std::uint8_t v[3] = {1, 1, 1};
        bp::pack_lsb_first(v, 3, out);
        require(out.size() == 1 && out[0] == 0x07U, "the vector overload resizes and packs");
    }

    std::cout << "bool_bitpack: all checks passed\n";
    return 0;
}
