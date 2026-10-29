// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "encoding/layout/miniblock.hpp"

#include <string>

namespace nano_lance::miniblock {

namespace {

void put_le16(std::vector<std::uint8_t>& out, std::size_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
}

void put_le32(std::vector<std::uint8_t>& out, std::size_t v) {
    put_le16(out, v & 0xFFFFU);
    put_le16(out, (v >> 16U) & 0xFFFFU);
}

void pad(std::vector<std::uint8_t>& out) { out.resize(align(out.size()), kFillByte); }

}  // namespace

std::vector<std::uint8_t> chunk_header(std::size_t num_levels, std::size_t def_bytes,
                                       std::span<const std::size_t> buffer_sizes) {
    std::vector<std::uint8_t> out;
    out.reserve(align(4U + buffer_sizes.size() * 4U));
    put_le16(out, def_bytes == 0U ? 0U : num_levels);
    if (def_bytes != 0U) {
        put_le16(out, def_bytes);
    }
    for (const auto size : buffer_sizes) {
        put_le32(out, size);
    }
    pad(out);
    return out;
}

std::size_t chunk_bytes(std::size_t def_bytes, std::span<const std::size_t> buffer_sizes) {
    std::size_t total = align(2U + (def_bytes != 0U ? 2U : 0U) + buffer_sizes.size() * 4U) + align(def_bytes);
    for (const auto size : buffer_sizes) {
        total += align(size);
    }
    return total;
}

void append_chunk(std::vector<std::uint8_t>& out, std::size_t num_levels, std::span<const std::uint8_t> def,
                  std::span<const std::span<const std::uint8_t>> buffers) {
    std::vector<std::size_t> sizes;
    sizes.reserve(buffers.size());
    for (const auto b : buffers) {
        sizes.push_back(b.size());
    }
    const auto header = chunk_header(num_levels, def.size(), sizes);
    out.reserve(out.size() + chunk_bytes(def.size(), sizes));
    out.insert(out.end(), header.begin(), header.end());
    if (!def.empty()) {
        out.insert(out.end(), def.begin(), def.end());
        pad(out);
    }
    for (const auto b : buffers) {
        out.insert(out.end(), b.begin(), b.end());
        pad(out);
    }
}

void append_chunk(std::vector<std::uint8_t>& out, std::size_t num_levels, std::span<const std::uint8_t> def,
                  std::span<const std::uint8_t> values) {
    const std::span<const std::uint8_t> one[1] = {values};
    append_chunk(out, num_levels, def, std::span<const std::span<const std::uint8_t>>(one));
}

std::uint32_t control_word(std::size_t chunk_bytes, std::size_t values, bool last) {
    std::uint32_t log_values = 0U;
    if (!last) {
        for (std::size_t v = values; v > 1U; v >>= 1U) {
            ++log_values;
        }
    }
    return static_cast<std::uint32_t>((chunk_bytes / kAlignment - 1U) << 4U) | (log_values & 0x0FU);
}

std::vector<std::uint8_t> control_buffer(std::span<const ChunkMeta> chunks) {
    std::vector<std::uint8_t> out;
    out.reserve(chunks.size() * 4U);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        put_le32(out, control_word(chunks[i].bytes, chunks[i].values, i + 1U == chunks.size()));
    }
    return out;
}
bool decode_chunk_items(std::span<const std::uint8_t> control, bool large_words, std::uint64_t num_items,
                        std::vector<std::uint64_t>& out, std::string& error) {
    out.clear();
    const std::size_t word = large_words ? 4U : 2U;
    if (control.empty() || control.size() % word != 0U) {
        error = "list page metadata is " + std::to_string(control.size()) + " bytes, not whole words";
        return false;
    }
    const std::size_t chunks = control.size() / word;
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i + 1U < chunks; ++i) {
        const auto log = control[i * word] & 0x0FU;
        if (log == 0U) {
            error = "list page chunk " + std::to_string(i) + " is not the last but declares no values";
            return false;
        }
        const std::uint64_t n = std::uint64_t{1} << log;
        if (n > num_items - sum) {
            error = "list page chunks declare more values than the page's " + std::to_string(num_items);
            return false;
        }
        sum += n;
        out.push_back(n);
    }
    if (num_items - sum == 0U && num_items != 0U) {
        error = "list page's last chunk has no values";
        return false;
    }
    out.push_back(num_items - sum);
    return true;
}

bool read_chunk_header(std::span<const std::uint8_t> payload, std::size_t offset, const ChunkShape& shape,
                       ChunkHeader& out, std::string& error) {
    out.buffer_sizes.clear();
    // num_buffers comes from the untrusted descriptor; cap it so a hostile value cannot make the header
    // arithmetic below run away before any of it is bounds-checked.
    if (shape.num_buffers > 64U) {
        error = "miniblock layout declares " + std::to_string(shape.num_buffers) + " buffers per chunk";
        return false;
    }
    const auto declared = shape.declared_bytes();
    if (offset > payload.size() || payload.size() - offset < declared) {
        error = "truncated miniblock payload header";
        return false;
    }

    std::size_t at = offset;
    const auto take_u16 = [&payload, &at]() {
        const auto v = static_cast<std::uint32_t>(payload[at]) | (static_cast<std::uint32_t>(payload[at + 1U]) << 8U);
        at += 2U;
        return v;
    };
    const auto take_u32 = [&take_u16]() {
        const auto lo = take_u16();
        return lo | (take_u16() << 16U);
    };

    out.num_levels = static_cast<std::uint16_t>(take_u16());
    out.rep_size = shape.has_repetition ? take_u16() : 0U;
    out.def_size = shape.has_definition ? take_u16() : 0U;
    out.buffer_sizes.reserve(shape.num_buffers);
    for (std::uint32_t i = 0; i < shape.num_buffers; ++i) {
        out.buffer_sizes.push_back(shape.large_buffer_sizes ? take_u32() : take_u16());
    }

    // Every section is padded, so walk them in order and let each one's end define the next one's start.
    // Sizes are untrusted: accumulate in 64 bits and compare against what is left of the payload.
    std::uint64_t cursor = align(declared);
    const auto advance = [&cursor](std::uint64_t size) {
        cursor = (cursor + size + kAlignment - 1U) & ~static_cast<std::uint64_t>(kAlignment - 1U);
    };
    advance(out.rep_size);
    advance(out.def_size);
    for (const auto size : out.buffer_sizes) {
        advance(size);
    }
    if (cursor > payload.size() - offset) {
        error = "miniblock chunk exceeds payload";
        return false;
    }
    out.chunk_bytes = static_cast<std::size_t>(cursor);
    return true;
}

bool split_payload(std::span<const std::uint8_t> payload, const ChunkShape& shape, std::vector<ChunkView>& out,
                   std::string& error) {
    std::size_t used = 0;
    std::size_t offset = 0;
    ChunkHeader header;
    while (offset < payload.size()) {
        if (!read_chunk_header(payload, offset, shape, header, error)) {
            out.resize(used);
            return false;
        }
        if (used == out.size()) {
            out.emplace_back();
        }
        ChunkView& chunk = out[used++];
        chunk.values.clear();
        chunk.extra_buffers.clear();
        chunk.repdef.clear();
        chunk.repdef_values = 0;
        chunk.rep.clear();
        chunk.items = 0;
        const auto slice = [&payload](std::size_t at, std::size_t size, std::vector<std::uint8_t>& to) {
            to.assign(payload.begin() + static_cast<std::ptrdiff_t>(at),
                      payload.begin() + static_cast<std::ptrdiff_t>(at + size));
        };
        std::size_t at = offset + align(shape.declared_bytes());
        chunk.num_levels = header.num_levels;
        if (shape.has_repetition && header.rep_size != 0U) {
            slice(at, header.rep_size, chunk.rep);
        }
        at += align(header.rep_size);
        if (shape.has_definition && header.def_size != 0U) {
            chunk.repdef_values = header.num_levels;
            slice(at, header.def_size, chunk.repdef);
        }
        at += align(header.def_size);
        for (std::size_t i = 0; i < header.buffer_sizes.size(); ++i) {
            const auto size = static_cast<std::size_t>(header.buffer_sizes[i]);
            if (i == 0U) {
                slice(at, size, chunk.values);
            } else {
                slice(at, size, chunk.extra_buffers.emplace_back());
            }
            at += align(size);
        }
        offset += header.chunk_bytes;
    }
    out.resize(used);
    return true;
}

}  // namespace nano_lance::miniblock
