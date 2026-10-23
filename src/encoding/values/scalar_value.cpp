// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The scalar value buffer of a constant page (lance-arrow scalar.rs): a u32 buffer count, the buffer lengths, then the buffers.

#include "encoding/values/scalar_value.hpp"

#include "nanolance/read_safety.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nano_lance::scalar_value {

// Lance scalar value buffer for a length-1 string/binary array: [u32 num_buffers][u32 buf_len...]
// [buffers]. A utf8/binary value is 2 buffers (offsets [0,len] + data). See lance-arrow scalar.rs.
std::vector<std::uint8_t> encode_variable(const std::vector<std::uint8_t>& value, bool large) {
    std::vector<std::uint8_t> out;
    auto write_u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
        }
    };
    const std::uint32_t offsets_len = large ? 16U : 8U;
    write_u32(2U);                                          // num_buffers (offsets + data)
    write_u32(offsets_len);                                // buffer 0: offsets
    write_u32(static_cast<std::uint32_t>(value.size()));   // buffer 1: data
    if (large) {
        const std::uint64_t start = 0;
        const std::uint64_t end = value.size();
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<std::uint8_t>((start >> (8 * i)) & 0xFFU));
        }
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<std::uint8_t>((end >> (8 * i)) & 0xFFU));
        }
    } else {
        write_u32(0U);
        write_u32(static_cast<std::uint32_t>(value.size()));
    }
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

/// Inverse of encode_variable: a Lance scalar value buffer holding a
/// length-1 array, laid out as [u32 num_buffers][u32 buffer_len ...][buffers]. For utf8/binary that
/// is two buffers -- offsets [0, len] and the data -- and the value we want is the data buffer whole;
/// for a fixed-width value it is one buffer, the value. Every length is untrusted, so each is bounds-checked before use.
[[nodiscard]] bool decode_variable(const std::vector<std::uint8_t>& buffer,
                                                std::vector<std::uint8_t>& out, std::string& error) {
    if (buffer.size() < 4U) {
        error = "constant value buffer shorter than its header";
        return false;
    }
    const auto num_buffers = load_le<std::uint32_t>(buffer.data());
    // A fixed-width value too wide to inline in the descriptor (a fixed_size_binary(33) constant) is
    // the same layout with ONE buffer: the value's bytes.
    if (num_buffers == 1U) {
        if (buffer.size() < 8U) {
            error = "constant value buffer shorter than its header";
            return false;
        }
        const auto len = load_le<std::uint32_t>(buffer.data() + 4U);
        if (len > buffer.size() - 8U) {
            error = "constant value buffer's data range exceeds the buffer";
            return false;
        }
        out.assign(buffer.begin() + 8, buffer.begin() + 8 + static_cast<std::ptrdiff_t>(len));
        return true;
    }
    if (num_buffers != 2U) {
        error = "constant value buffer declares " + std::to_string(num_buffers) +
                " buffers; expected 1 (a fixed-width value) or 2 (offsets + data)";
        return false;
    }
    std::uint64_t header = 0;
    if (!checked_mul(num_buffers, 4U, header) || !checked_add(header, 4U, header) || header > buffer.size()) {
        error = "constant value buffer header exceeds the buffer";
        return false;
    }
    const auto offsets_len = load_le<std::uint32_t>(buffer.data() + 4U);
    const auto data_len = load_le<std::uint32_t>(buffer.data() + 8U);
    std::uint64_t data_start = 0;
    std::uint64_t data_end = 0;
    if (!checked_add(header, offsets_len, data_start) || !checked_add(data_start, data_len, data_end) ||
        data_end > buffer.size()) {
        error = "constant value buffer's data range exceeds the buffer";
        return false;
    }
    out.assign(buffer.begin() + static_cast<std::ptrdiff_t>(data_start),
               buffer.begin() + static_cast<std::ptrdiff_t>(data_end));
    return true;
}

}  // namespace nano_lance::scalar_value
