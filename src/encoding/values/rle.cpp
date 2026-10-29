// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "encoding/values/rle.hpp"

#include "nanolance/read_safety.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace nano_lance::rle {

namespace {

/// Appends `count` copies of the `vlen`-byte value at `val` to `out`, growing it once. False when the result
/// would pass the decoded-size limit. The caller reserves the whole column up front, so the growth here is
/// normally a no-op.
bool append_repeated(std::vector<std::uint8_t>& out, const std::uint8_t* val, std::size_t vlen,
                     std::size_t count) {
    if (count == 0U || vlen == 0U) {
        return true;
    }
    std::uint64_t added = 0;
    std::uint64_t new_size = 0;
    if (!checked_mul(vlen, count, added) || !checked_add(out.size(), added, new_size) ||
        !fits_size_t(new_size) || new_size > default_read_limits().max_uncompressed_bytes) {
        return false;
    }
    const std::size_t base = out.size();
    reserve_more(out, static_cast<std::size_t>(added));
    out.resize(static_cast<std::size_t>(new_size));
    std::uint8_t* dst = out.data() + base;
    // Doubling fill: write the value once, then repeatedly copy everything written so far.
    std::memcpy(dst, val, vlen);
    std::size_t filled = vlen;
    const std::size_t total = static_cast<std::size_t>(added);
    while (filled < total) {
        const std::size_t chunk = std::min(filled, total - filled);
        std::memcpy(dst + filled, dst, chunk);
        filled += chunk;
    }
    return true;
}

}  // namespace

namespace {

// Chunk limits measured from pylance 12 (W1q): at most 4096 rows, and at most 8192 encoded bytes.
constexpr std::size_t kChunkMaxRows = 4096U;
constexpr std::size_t kChunkMaxBytes = 8192U;
constexpr std::size_t kChunkHeaderBytes = 8U;

/// Encoded bytes of a chunk holding `pieces` (value, 8-bit length) pairs.
std::uint64_t chunk_bytes(std::uint64_t pieces, std::size_t value_bytes) {
    const std::uint64_t raw = kChunkHeaderBytes + pieces * (static_cast<std::uint64_t>(value_bytes) + 1U);
    return (raw + 7U) / 8U * 8U;
}

}  // namespace

std::vector<std::size_t> chunk_rows(std::span<const std::uint64_t> run_lengths, std::size_t value_bytes,
                                    std::size_t max_rows) {
    std::vector<std::size_t> out;
    std::uint64_t remaining = 0;
    for (const auto length : run_lengths) {
        remaining += length;
    }
    std::size_t run = 0;
    std::uint64_t offset = 0;
    while (remaining > 0U) {
        // The candidates, smallest first: 1, 2, 4, ... below `remaining`, then min(remaining, 4096). A chunk's
        // size only grows with its rows, so one walk from the chunk's start finds the largest that fits, and
        // stops at the first that does not (pylance's choice; at least one row always goes).
        const std::uint64_t last = std::min<std::uint64_t>(remaining, std::min<std::size_t>(kChunkMaxRows, max_rows));
        std::uint64_t target = 1U;
        std::uint64_t chosen = 1U;
        std::uint64_t rows = 0;    // rows walked from the chunk's start
        std::uint64_t pieces = 0;  // pairs those rows encode to
        std::size_t r = run;
        std::uint64_t off = offset;
        bool done = false;
        while (!done && r < run_lengths.size()) {
            const std::uint64_t seg = std::min(run_lengths[r] - off, last - rows);
            // Every candidate that ends inside this segment.
            while (target <= rows + seg) {
                const std::uint64_t in_seg = target - rows;
                if (chunk_bytes(pieces + (in_seg + 254U) / 255U, value_bytes) > kChunkMaxBytes) {
                    done = true;
                    break;
                }
                chosen = target;
                if (target == last) {
                    done = true;
                    break;
                }
                target = std::min(target * 2U, last);
            }
            rows += seg;
            pieces += (seg + 254U) / 255U;
            ++r;
            off = 0U;
        }
        out.push_back(static_cast<std::size_t>(chosen));
        remaining -= chosen;
        // Advance past the chosen rows, splitting a run that the chunk ends inside.
        std::uint64_t left = chosen;
        while (left > 0U) {
            const std::uint64_t take = std::min(run_lengths[run] - offset, left);
            left -= take;
            offset += take;
            if (offset == run_lengths[run]) {
                ++run;
                offset = 0U;
            }
        }
    }
    return out;
}

void append_run(std::span<const std::uint8_t> value, std::uint64_t length, std::vector<std::uint8_t>& values,
                std::vector<std::uint8_t>& lengths) {
    for (std::uint64_t remaining = length; remaining > 0;) {
        const auto take = static_cast<std::uint8_t>(std::min<std::uint64_t>(255U, remaining));
        values.insert(values.end(), value.begin(), value.end());
        lengths.push_back(take);
        remaining -= take;
    }
}

void append_index_run(std::uint32_t index, std::uint64_t length, std::vector<std::uint8_t>& values,
                      std::vector<std::uint8_t>& lengths) {
    std::uint8_t le[4];
    for (std::size_t i = 0; i < 4U; ++i) {
        le[i] = static_cast<std::uint8_t>((index >> (8U * i)) & 0xFFU);
    }
    append_run(std::span<const std::uint8_t>(le, 4U), length, values, lengths);
}

bool decode(std::span<const std::uint8_t> values, std::span<const std::uint8_t> lengths, std::size_t width,
            std::vector<std::uint8_t>& out, std::string& error) {
    if (width == 0U || values.size() % width != 0U) {
        error = "rle chunk buffer sizes invalid";
        return false;
    }
    const std::size_t num_runs = values.size() / width;
    if (num_runs != lengths.size()) {
        error = "rle run count mismatch between values and lengths";
        return false;
    }
    for (std::size_t r = 0; r < num_runs; ++r) {
        const std::uint64_t run = lengths[r];
        if (!append_repeated(out, values.data() + r * width, width, static_cast<std::size_t>(run))) {
            error = "rle run expansion overflows";
            return false;
        }
    }
    return true;
}

bool decode_levels(std::span<const std::uint8_t> block, std::uint32_t value_bits, std::uint32_t length_bits,
                   std::uint32_t count, std::uint16_t* levels, std::string& error) {
    if ((value_bits != 8U && value_bits != 16U) || (length_bits != 8U && length_bits != 16U && length_bits != 32U)) {
        error = "run-length-encoded definition levels declare unsupported widths (" + std::to_string(value_bits) +
                "-bit values, " + std::to_string(length_bits) + "-bit run lengths)";
        return false;
    }
    const std::size_t value_bytes = value_bits / 8U;
    const std::size_t length_bytes = length_bits / 8U;

    if (block.size() < 8U) {
        error = "run-length definition-level buffer is too short for its header";
        return false;
    }
    const auto values_size64 = load_le<std::uint64_t>(block.data());
    if (!fits_size_t(values_size64) || static_cast<std::size_t>(values_size64) > block.size() - 8U) {
        error = "run-length definition-level values buffer runs past the end of the block";
        return false;
    }
    const auto values_size = static_cast<std::size_t>(values_size64);
    if (values_size % value_bytes != 0U) {
        error = "run-length definition-level values buffer is not a whole number of values";
        return false;
    }
    const auto runs = values_size / value_bytes;
    const auto lengths_size = block.size() - 8U - values_size;
    if (lengths_size != runs * length_bytes) {
        error = "run-length definition-level block has " + std::to_string(runs) + " run values but " +
                std::to_string(lengths_size) + " bytes of run lengths";
        return false;
    }

    const auto* values = block.data() + 8U;
    const auto* lengths = values + values_size;
    std::uint64_t written = 0;
    for (std::size_t r = 0; r < runs; ++r) {
        const std::uint16_t level = value_bytes == 1U ? values[r] : load_le<std::uint16_t>(values + r * 2U);
        std::uint64_t run = 0;
        switch (length_bytes) {
            case 1U: run = lengths[r]; break;
            case 2U: run = load_le<std::uint16_t>(lengths + r * 2U); break;
            default: run = load_le<std::uint32_t>(lengths + r * 4U); break;
        }
        if (run > count || written + run > count) {
            error = "run-length definition levels cover more than the chunk's " + std::to_string(count) +
                    " values";
            return false;
        }
        for (std::uint64_t i = 0; i < run; ++i) {
            levels[written + i] = level;
        }
        written += run;
    }
    if (written != count) {
        error = "run-length definition levels cover " + std::to_string(written) + " of the chunk's " +
                std::to_string(count) + " values";
        return false;
    }
    return true;
}

}  // namespace nano_lance::rle
