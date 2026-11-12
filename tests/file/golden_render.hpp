// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Shared by the Rust-free golden read tests (tests/file/test_*_read.cpp): renders the rows of an Arrow read as text,
// so a test states each row's expected text, whatever the page layout the file was written with.

#pragma once

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace golden {

inline void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

inline bool valid_at(const ArrowArray& array, std::int64_t i) {
    if (array.null_count == 0 || array.buffers[0] == nullptr) {
        return true;
    }
    const auto* bits = static_cast<const std::uint8_t*>(array.buffers[0]);
    const std::int64_t at = array.offset + i;
    return ((bits[at >> 3] >> (at & 7)) & 1U) != 0U;
}

inline std::int64_t offset_at(const std::uint8_t* offsets, bool large, std::int64_t k) {
    if (large) {
        std::int64_t v = 0;
        std::memcpy(&v, offsets + k * 8, 8);
        return v;
    }
    std::int32_t v = 0;
    std::memcpy(&v, offsets + k * 4, 4);
    return v;
}

/// Renders element `i` of `array` (of `schema`) as text: a struct as {name:value,...}, a list or map as [a,b,...], a
/// fixed-size list as [a,b,...] too, strings quoted, integers as numbers, a null as null. Only the formats the golden
/// files hold are rendered; any other is a failure, so a shape the test does not know cannot pass by accident.
inline std::string render(const ArrowSchema& schema, const ArrowArray& array, std::int64_t i) {
    const std::string format = schema.format;
    if (!valid_at(array, i)) {
        return "null";
    }
    if (format == "+s") {
        std::string out = "{";
        for (std::int64_t c = 0; c < array.n_children; ++c) {
            if (c > 0) {
                out += ",";
            }
            // A struct's children are indexed at the struct's own offset, plus the row.
            out += std::string(schema.children[c]->name) + ":" +
                   render(*schema.children[c], *array.children[c], array.offset + i);
        }
        return out + "}";
    }
    if (format == "+l" || format == "+L" || format == "+m") {
        const bool large = format == "+L";
        const auto* offsets = static_cast<const std::uint8_t*>(array.buffers[1]);
        const std::int64_t first = offset_at(offsets, large, array.offset + i);
        const std::int64_t last = offset_at(offsets, large, array.offset + i + 1);
        std::string out = "[";
        for (std::int64_t k = first; k < last; ++k) {
            if (k > first) {
                out += ",";
            }
            out += render(*schema.children[0], *array.children[0], k);
        }
        return out + "]";
    }
    if (format.rfind("+w:", 0) == 0) {
        // A fixed-size list of N: row i holds the child's elements N*i .. N*i+N-1.
        const std::int64_t n = std::stoll(format.substr(3));
        std::string out = "[";
        for (std::int64_t k = 0; k < n; ++k) {
            if (k > 0) {
                out += ",";
            }
            out += render(*schema.children[0], *array.children[0], (array.offset + i) * n + k);
        }
        return out + "]";
    }
    if (format == "u" || format == "U") {
        const bool large = format == "U";
        const auto* offsets = static_cast<const std::uint8_t*>(array.buffers[1]);
        const auto* data = static_cast<const char*>(array.buffers[2]);
        const std::int64_t begin = offset_at(offsets, large, array.offset + i);
        const std::int64_t end = offset_at(offsets, large, array.offset + i + 1);
        return "\"" + std::string(data + begin, static_cast<std::size_t>(end - begin)) + "\"";
    }
    if (format == "I") {
        std::uint32_t v = 0;
        std::memcpy(&v, static_cast<const std::uint8_t*>(array.buffers[1]) + (array.offset + i) * 4, 4);
        return std::to_string(v);
    }
    if (format == "i") {
        std::int32_t v = 0;
        std::memcpy(&v, static_cast<const std::uint8_t*>(array.buffers[1]) + (array.offset + i) * 4, 4);
        return std::to_string(v);
    }
    require(false, "the golden check does not render Arrow format '" + format + "'");
    return "";
}

/// The rows of one read, each rendered. A file whose top level is one column is rendered as that column; one whose
/// top level holds several is rendered as the struct they make.
inline std::vector<std::string> render_rows(const ArrowSchema& schema, const std::vector<ArrowArray>& batches) {
    std::vector<std::string> rows;
    const bool one_column = schema.n_children == 1;
    for (const auto& batch : batches) {
        for (std::int64_t i = 0; i < batch.length; ++i) {
            rows.push_back(one_column ? render(*schema.children[0], *batch.children[0], i) : render(schema, batch, i));
        }
    }
    return rows;
}

inline void release(ArrowSchema& schema, std::vector<ArrowArray>& batches) {
    for (auto& batch : batches) {
        if (batch.release != nullptr) {
            ArrowArrayRelease(&batch);
        }
    }
    if (schema.release != nullptr) {
        ArrowSchemaRelease(&schema);
    }
}

}  // namespace golden
