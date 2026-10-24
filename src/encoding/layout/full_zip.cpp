// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "encoding/layout/full_zip.hpp"

#include <cstring>
#include <limits>

namespace nano_lance::full_zip {

namespace {

/// The low `bytes` bytes of `v`, little-endian.
void put_le(std::vector<std::uint8_t>& out, std::uint64_t v, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8U * i)) & 0xFFU));
    }
}

std::uint64_t get_le(const std::uint8_t* p, std::size_t bytes) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8U * i);
    }
    return v;
}

}  // namespace

void append_level(std::vector<std::uint8_t>& out, std::size_t control, std::uint32_t word,
                  const std::uint8_t* value, std::size_t value_size, std::size_t length_bytes) {
    put_le(out, word, control);
    if (value == nullptr) {
        return;
    }
    put_le(out, value_size, length_bytes);
    out.insert(out.end(), value, value + value_size);
}

std::vector<std::uint8_t> repetition_index(std::span<const std::uint64_t> starts) {
    const std::size_t width =
        starts.empty() || starts.back() <= std::numeric_limits<std::uint32_t>::max() ? 4U : 8U;
    std::vector<std::uint8_t> out;
    out.reserve(starts.size() * width);
    for (const auto s : starts) {
        put_le(out, s, width);
    }
    return out;
}

bool next_level(std::span<const std::uint8_t> data, std::size_t& at, const RowShape& shape, Level& out,
                std::string& error) {
    out = Level{};
    const auto control = shape.control();
    if (control != 0U) {
        if (data.size() - at < control) {
            error = "FullZip page truncated in a control word";
            return false;
        }
        const auto word = static_cast<std::uint32_t>(get_le(data.data() + at, control));
        at += control;
        const std::uint32_t def_mask = shape.bits_def == 0U ? 0U : ((1U << shape.bits_def) - 1U);
        const std::uint32_t rep_mask = shape.bits_rep == 0U ? 0U : ((1U << shape.bits_rep) - 1U);
        out.def = word & def_mask;
        out.rep = shape.bits_def >= 32U ? 0U : (word >> shape.bits_def) & rep_mask;
    }
    if (out.def > shape.max_visible_def) {
        return true;  // an empty or null list: the control word is all there is
    }
    out.visible = true;
    if (shape.length_bytes == 0U) {
        if (data.size() - at < shape.value_bytes) {
            error = "FullZip page truncated in a value";
            return false;
        }
        out.has_value = true;
        out.value_at = at;
        out.value_size = shape.value_bytes;
        at += shape.value_bytes;
        return true;
    }
    if (out.def != 0U) {
        return true;  // a null value has no length
    }
    if (data.size() - at < shape.length_bytes) {
        error = "FullZip page truncated in a length";
        return false;
    }
    const auto length = get_le(data.data() + at, shape.length_bytes);
    at += shape.length_bytes;
    if (length > data.size() - at) {
        error = "FullZip value runs past the page";
        return false;
    }
    out.has_value = true;
    out.value_at = at;
    out.value_size = static_cast<std::size_t>(length);
    at += out.value_size;
    return true;
}

bool count_levels(std::span<const std::uint8_t> data, const RowShape& shape, std::uint64_t& levels,
                  std::uint64_t& visible, std::string& error) {
    levels = 0;
    visible = 0;
    if (shape.control() == 0U) {
        error = "a FullZip page window without control words";
        return false;
    }
    std::size_t at = 0;
    Level level;
    while (at < data.size()) {
        if (!next_level(data, at, shape, level, error)) {
            return false;
        }
        ++levels;
        visible += level.visible ? 1U : 0U;
    }
    return true;
}

std::size_t repetition_index_width(std::uint64_t index_bytes, std::uint64_t rows) {
    const auto entries = rows + 1U;
    if (index_bytes == 0U || index_bytes % entries != 0U) {
        return 0U;
    }
    const auto width = index_bytes / entries;
    return width == 1U || width == 2U || width == 4U || width == 8U ? static_cast<std::size_t>(width) : 0U;
}

std::uint64_t repetition_index_entry(std::span<const std::uint8_t> index, std::size_t width, std::uint64_t k) {
    return get_le(index.data() + k * width, width);
}

}  // namespace nano_lance::full_zip
