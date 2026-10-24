// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Random UUIDs (base/uuid.hpp). A dataset, a transaction and a write each get a fresh id; the id must be
// a version 4 UUID with the RFC 4122 variant (Lance readers check both), and two calls must differ.

#include "base/uuid.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

}  // namespace

int main() {
    // One id: 16 bytes, version nibble 4 (byte 6), variant bits 10xxxxxx (byte 8).
    {
        const auto id = nano_lance::new_uuid();
        require(id.size() == 16, "a UUID is 16 bytes");
        require((id[6] >> 4U) == 0x4U, "the version nibble is 4");
        require((id[8] & 0xC0U) == 0x80U, "the RFC 4122 variant bits are 10");
    }

    // Many ids: every one has the version and variant bits, none repeats, and none is all zero.
    {
        std::set<std::array<std::uint8_t, 16>> seen;
        for (int i = 0; i < 2000; ++i) {
            const auto id = nano_lance::new_uuid();
            require((id[6] >> 4U) == 0x4U, "every id is version 4");
            require((id[8] & 0xC0U) == 0x80U, "every id has the RFC 4122 variant");
            bool all_zero = true;
            for (const auto b : id) {
                all_zero = all_zero && b == 0;
            }
            require(!all_zero, "an id is not all zero");
            require(seen.insert(id).second, "two ids are different");
        }
        require(seen.size() == 2000, "2000 calls give 2000 distinct ids");
    }

    std::cout << "uuid: 3 checks passed (2001 ids)\n";
    return 0;
}
