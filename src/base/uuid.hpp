// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Random (version 4) UUIDs.

#pragma once

#include <array>
#include <cstdint>

namespace nano_lance {

/// A random (version 4) UUID.
std::array<std::uint8_t, 16> new_uuid();

}  // namespace nano_lance
