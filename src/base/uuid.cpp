// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "base/uuid.hpp"

#include <chrono>
#include <cstring>
#include <random>

namespace nano_lance {

std::array<std::uint8_t, 16> new_uuid() {
    std::random_device device;
    std::mt19937_64 rng((static_cast<std::uint64_t>(device()) << 32U) ^ device() ^
                        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::array<std::uint8_t, 16> id{};
    for (std::size_t i = 0; i < id.size(); i += 8) {
        const auto r = rng();
        std::memcpy(id.data() + i, &r, 8);
    }
    id[6] = static_cast<std::uint8_t>((id[6] & 0x0FU) | 0x40U);  // version 4
    id[8] = static_cast<std::uint8_t>((id[8] & 0x3FU) | 0x80U);  // RFC 4122 variant
    return id;
}

}  // namespace nano_lance
