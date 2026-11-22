// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/blob_v2_layout.hpp"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace nano_lance {

std::vector<std::uint8_t> blob_v2_pack_descriptor_row(const BlobV2ExternalDescriptor& descriptor) {
    const auto uri_bytes = static_cast<std::uint32_t>(descriptor.blob_uri.size());
    const std::uint32_t prefix = kBlobV2FixedDescriptorBytes + uri_bytes;

    // Sized once and written by index. Building the row with reserve() and push_back() trips GCC 14's
    // -Wfree-nonheap-object (a false positive on the vector's growth path after inlining).
    constexpr std::size_t kFixedBytes = 4U + 1U + 8U + 8U + 4U + 4U;
    std::vector<std::uint8_t> row_bytes(kFixedBytes + descriptor.blob_uri.size());
    std::uint8_t* out = row_bytes.data();
    // Little-endian integer of `width` bytes.
    const auto put = [&out](std::uint64_t value, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            *out++ = static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU);
        }
    };
    put(prefix, 4U);
    put(descriptor.kind, 1U);
    put(descriptor.position, 8U);
    put(descriptor.size, 8U);
    put(descriptor.blob_id, 4U);
    put(uri_bytes, 4U);
    if (!descriptor.blob_uri.empty()) {
        std::memcpy(out, descriptor.blob_uri.data(), descriptor.blob_uri.size());
    }
    return row_bytes;
}

bool blob_v2_control_buffer_to_row_sizes(const std::vector<std::uint8_t>& control, const std::uint64_t num_rows,
                                         std::vector<std::uint32_t>& row_packed_sizes, std::string& error) {
    row_packed_sizes.clear();
    error.clear();
    if (num_rows > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) - 1U) {
        error = "blob row count overflow";
        return false;
    }
    const auto entries = static_cast<std::size_t>(num_rows) + 1U;
    std::size_t width = 0;
    bool older = false;  // an older nanolance file: [0][1] and the rows' u32 ends
    if (control.size() % entries == 0U) {
        width = control.size() / entries;
    } else if (num_rows != 0U && control.size() == 2U + 4U * static_cast<std::size_t>(num_rows) && control[0] == 0U &&
               control[1] == 1U) {
        width = 4U;
        older = true;
    }
    if (width != 1U && width != 2U && width != 4U && width != 8U) {
        error = "a blob page's row index is " + std::to_string(control.size()) + " bytes for " +
                std::to_string(num_rows) + " rows";
        return false;
    }
    const auto offset_at = [&](std::size_t i) -> std::uint64_t {
        if (older && i == 0U) {
            return 0U;
        }
        std::uint64_t v = 0;
        std::memcpy(&v, control.data() + (older ? 2U + (i - 1U) * 4U : i * width), width);
        return v;
    };
    if (offset_at(0) != 0U) {
        error = "a blob page's row index does not start at 0";
        return false;
    }
    row_packed_sizes.reserve(static_cast<std::size_t>(num_rows));
    std::uint64_t prev = 0;
    for (std::size_t i = 1; i < entries; ++i) {
        const auto cum = offset_at(i);
        if (cum < prev || cum - prev > std::numeric_limits<std::uint32_t>::max()) {
            error = "a blob page's row index is not increasing";
            return false;
        }
        row_packed_sizes.push_back(static_cast<std::uint32_t>(cum - prev));
        prev = cum;
    }
    return true;
}

void blob_append_legacy_row(ColumnValues& out, std::uint64_t position, std::uint64_t size, bool null) {
    out.kind = ColumnValues::Kind::BlobV2External;
    const auto row = out.blob_v2.row_packed_sizes.size();
    if (null || !out.validity.empty()) {
        if (out.validity.empty()) {
            out.validity.assign((row + 8U) / 8U, 0xFFU);
        }
        out.validity.resize((row + 8U) / 8U, 0xFFU);
        if (null) {
            out.validity[row / 8U] &= static_cast<std::uint8_t>(~(1U << (row % 8U)));
            ++out.null_count;
        } else {
            out.validity[row / 8U] |= static_cast<std::uint8_t>(1U << (row % 8U));
        }
    }
    if (null) {
        out.blob_v2.row_packed_sizes.push_back(0U);
        return;
    }
    BlobV2ExternalDescriptor descriptor;
    descriptor.kind = kBlobKindInline;
    descriptor.position = position;
    descriptor.size = size;
    const auto bytes = blob_v2_pack_descriptor_row(descriptor);
    out.blob_v2.packed_payload.insert(out.blob_v2.packed_payload.end(), bytes.begin(), bytes.end());
    out.blob_v2.row_packed_sizes.push_back(static_cast<std::uint32_t>(bytes.size()));
}

}  // namespace nano_lance
