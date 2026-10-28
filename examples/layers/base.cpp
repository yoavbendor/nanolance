// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer base: the small helpers every other layer uses. This example shows two of them.
//
//   - Bool packing (nanolance/bool_bitpack.hpp). A bool column is one byte per value in memory and one bit per
//     value in a Lance file, least significant bit first. pack_lsb_first and unpack_lsb_first convert between them.
//   - The thread pool (nanolance/parallel.hpp). parallel::for_each runs a task for every index and returns when all
//     of them are done.
//
// Run it with: ctest --test-dir build -R nano_lance_example_base --output-on-failure

#include "nanolance/bool_bitpack.hpp"
#include "nanolance/parallel.hpp"

#include <cstddef>
#include <cstdint>
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
    // Step 1: 19 bools, so the last byte is only partly used. Every third value is true.
    std::vector<std::uint8_t> bools(19);
    for (std::size_t i = 0; i < bools.size(); ++i) {
        bools[i] = (i % 3 == 0) ? 1 : 0;
    }

    // Step 2: pack them. 19 bits take 3 bytes. Bit 0 of byte 0 is the first value, so the first byte
    // has bits 0, 3 and 6 set: 0x49.
    std::vector<std::uint8_t> packed;
    nano_lance::boolpack::pack_lsb_first(bools.data(), bools.size(), packed);
    std::cout << "packed " << bools.size() << " bools into " << packed.size() << " bytes\n";
    if (packed.size() != 3) {
        return fail("19 bools should take 3 bytes, got " + std::to_string(packed.size()));
    }
    if (packed[0] != 0x49U) {
        return fail("first byte should be 0x49 (least significant bit first)");
    }

    // Step 3: unpack them again and compare every value.
    std::vector<std::uint8_t> unpacked(bools.size());
    nano_lance::boolpack::unpack_lsb_first(packed.data(), bools.size(), unpacked.data());
    for (std::size_t i = 0; i < bools.size(); ++i) {
        if (unpacked[i] != bools[i]) {
            return fail("value " + std::to_string(i) + " changed in the round trip");
        }
    }
    std::cout << "unpacked the same 19 values\n";

    // Step 4: the thread pool. Each task writes its own slot, so the tasks need no lock. The pool may
    // run them on several threads; the result does not depend on how many.
    const std::size_t n = 1000;
    std::vector<std::uint64_t> squares(n);
    nano_lance::parallel::for_each(n, [&](std::size_t i) {
        squares[i] = static_cast<std::uint64_t>(i) * static_cast<std::uint64_t>(i);
    });
    std::uint64_t sum = 0;
    for (const std::uint64_t s : squares) {
        sum += s;
    }
    // The sum of i*i for i = 0..999 is 999 * 1000 * 1999 / 6.
    const std::uint64_t expected = 332833500U;
    std::cout << "sum of squares below " << n << " = " << sum << " on " << nano_lance::parallel::threads()
              << " thread(s)\n";
    if (sum != expected) {
        return fail("sum of squares is " + std::to_string(sum) + ", want " + std::to_string(expected));
    }
    return 0;
}
