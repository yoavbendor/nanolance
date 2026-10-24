// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The bytes of a string, as a byte vector (schema metadata values, among others).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nano_lance {

inline std::vector<std::uint8_t> bytes_of(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace nano_lance
