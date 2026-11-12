// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/child_block.hpp"

#include "encoding/values/bitpack.hpp"
#include "encoding/values/general.hpp"
#include "nanolance/read_safety.hpp"

#include <cstring>
#include <limits>

namespace nano_lance::child_block {
namespace {

using page_layout::Compressive;
using page_layout::CompressiveKind;

bool valid_width(std::uint32_t bits) { return bits == 8U || bits == 16U || bits == 32U || bits == 64U; }

const char* scheme_name(page_layout::BufferScheme scheme, bool& known) {
    known = true;
    switch (scheme) {
        case page_layout::BufferScheme::kZstd: return "zstd";
        case page_layout::BufferScheme::kLz4: return "lz4";
        case page_layout::BufferScheme::kNone: return "none";
        default:
            known = false;
            return "";
    }
}

/// The little-endian value of the `bytes`-wide field at `at`.
std::uint64_t load_run(const std::vector<std::uint8_t>& raw, std::size_t at, std::size_t bytes) {
    std::uint64_t v = 0;
    std::memcpy(&v, raw.data() + at, bytes);  // little-endian host, as the rest of nanolance assumes
    return v;
}

}  // namespace

bool needs_count(const Compressive& encoding) {
    switch (encoding.kind) {
        case CompressiveKind::kInlineBitpacking:
        case CompressiveKind::kBitpacked:
            return true;
        case CompressiveKind::kGeneral:
            return encoding.values != nullptr && needs_count(*encoding.values);
        default:
            return false;
    }
}

std::uint32_t value_bits(const Compressive& encoding) {
    switch (encoding.kind) {
        case CompressiveKind::kFlat:
        case CompressiveKind::kInlineBitpacking:
        case CompressiveKind::kBitpacked:
            return encoding.bits_per_value;
        case CompressiveKind::kGeneral:
            return encoding.values != nullptr ? value_bits(*encoding.values) : 0U;
        case CompressiveKind::kRle:
            return encoding.values != nullptr ? value_bits(*encoding.values) : 0U;
        default:
            return 0U;
    }
}

bool decode(const Compressive& encoding, const std::vector<std::uint8_t>& bytes, std::optional<std::uint64_t> count,
            std::uint32_t bits, std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    if (!valid_width(bits)) {
        error = "child block values are " + std::to_string(bits) + " bits wide, which nanolance does not read";
        return false;
    }
    if (value_bits(encoding) != bits) {
        error = "child block declares " + std::to_string(value_bits(encoding)) + "-bit values, expected " +
                std::to_string(bits) + " (" + page_layout::describe_encoding(encoding) + ")";
        return false;
    }
    const std::size_t width = bits / 8U;
    std::uint64_t need = 0;
    if (count.has_value()) {
        if (*count > std::numeric_limits<std::size_t>::max() / width || !fits_size_t(*count)) {
            error = "child block declares too many values";
            return false;
        }
        need = *count * width;
    }
    switch (encoding.kind) {
        case CompressiveKind::kFlat:
            if (count.has_value() ? bytes.size() != need : bytes.size() % width != 0U) {
                error = "flat child block is " + std::to_string(bytes.size()) + " bytes" +
                        (count.has_value() ? " for " + std::to_string(*count) + " values" : "") +
                        " at " + std::to_string(bits) + " bits (" + page_layout::describe_encoding(encoding) + ")";
                return false;
            }
            out = bytes;
            return true;
        case CompressiveKind::kInlineBitpacking: {
            if (!count.has_value()) {
                error = "inline-bitpacked child block needs its value count";
                return false;
            }
            if (!bitpack::decode_series(bytes, static_cast<std::size_t>(*count), width, out, error)) {
                return false;
            }
            if (out.size() != need) {
                error = "inline-bitpacked child block decoded to " + std::to_string(out.size()) + " bytes";
                return false;
            }
            return true;
        }
        case CompressiveKind::kBitpacked: {
            if (!count.has_value()) {
                error = "bitpacked child block needs its value count";
                return false;
            }
            if (encoding.values == nullptr || encoding.values->kind != CompressiveKind::kFlat) {
                error = "bitpacked child block packs into " +
                        (encoding.values == nullptr ? std::string("nothing") : page_layout::describe_encoding(*encoding.values));
                return false;
            }
            out.assign(static_cast<std::size_t>(need), 0U);
            if (need != 0U && !bitpack::decode_out_of_line(bytes, static_cast<std::size_t>(*count),
                                                           encoding.values->bits_per_value, width, out.data(),
                                                           "child block", error)) {
                return false;
            }
            return true;
        }
        case CompressiveKind::kRle: {
            // Lance's block-level RLE: [u64 LE values size][values][run lengths]. The values and the
            // lengths are each their own child; the run count comes from whichever one is self-sizing.
            if (encoding.values == nullptr || encoding.lengths == nullptr) {
                error = "rle child block has no values or run lengths";
                return false;
            }
            if (bytes.size() < 8U) {
                error = "rle child block is too short for its header";
                return false;
            }
            std::uint64_t values_size = 0;
            std::memcpy(&values_size, bytes.data(), 8U);
            if (values_size > bytes.size() - 8U) {
                error = "rle child block's values run past its end";
                return false;
            }
            const std::uint32_t length_bits = value_bits(*encoding.lengths);
            if (length_bits != 8U && length_bits != 16U && length_bits != 32U) {
                error = "rle child block's run lengths are " + std::to_string(length_bits) + " bits wide";
                return false;
            }
            const std::vector<std::uint8_t> values(bytes.begin() + 8, bytes.begin() + 8 + static_cast<std::ptrdiff_t>(values_size));
            const std::vector<std::uint8_t> lengths(bytes.begin() + 8 + static_cast<std::ptrdiff_t>(values_size), bytes.end());
            if (!decode_rle_chunk(*encoding.values, *encoding.lengths, values, lengths, page_layout::BufferScheme::kNone, bits,
                                  length_bits, out, error)) {
                return false;
            }
            if (count.has_value() && out.size() != need) {
                error = "rle child block expanded to " + std::to_string(out.size()) + " bytes, expected " +
                        std::to_string(need);
                return false;
            }
            return true;
        }
        case CompressiveKind::kGeneral: {
            if (encoding.values == nullptr) {
                error = "general child block has no values";
                return false;
            }
            bool known = false;
            const char* scheme = scheme_name(encoding.scheme, known);
            if (!known) {
                error = "child block uses an unsupported buffer compression";
                return false;
            }
            std::vector<std::uint8_t> raw;
            if (!general::decompress(scheme, bytes.data(), bytes.size(), raw, error)) {
                error += " (" + page_layout::describe_encoding(encoding) + ")";
                return false;
            }
            if (!decode(*encoding.values, raw, count, bits, out, error)) {
                error += " (" + page_layout::describe_encoding(encoding) + ", " + std::to_string(bytes.size()) +
                         " compressed bytes)";
                return false;
            }
            return true;
        }
        default:
            error = "child block uses an encoding nanolance does not read: " +
                    page_layout::describe_encoding(encoding);
            return false;
    }
}

namespace {

/// Adds the two buffers' sizes to a refusal, so a chunk that does not split as its header says is named.
void name_rle_buffers(std::string& error, const std::vector<std::uint8_t>& values, const std::vector<std::uint8_t>& lengths) {
    error += " (rle chunk: values " + std::to_string(values.size()) + " bytes, run lengths " +
             std::to_string(lengths.size()) + " bytes)";
}

/// The runs of one RLE chunk, as the two children give them: the raw values, `bits` wide, one per run in `values_raw`,
/// and each run's length in `run_lengths`. One of the children may need the run count, which the other one gives; the
/// two must agree on the number of runs.
bool decode_rle_runs_impl(const Compressive& values_encoding, const Compressive& lengths_encoding,
                          const std::vector<std::uint8_t>& values, const std::vector<std::uint8_t>& lengths,
                          std::uint32_t bits, std::uint32_t length_bits, std::vector<std::uint8_t>& values_raw,
                          std::vector<std::uint64_t>& run_lengths, std::string& error) {
    if (!valid_width(bits) || (length_bits != 8U && length_bits != 16U && length_bits != 32U)) {
        error = "rle chunk declares " + std::to_string(bits) + "-bit values and " + std::to_string(length_bits) +
                "-bit run lengths, which nanolance does not read";
        return false;
    }
    const std::size_t value_width = bits / 8U;
    const std::size_t length_width = length_bits / 8U;
    const bool values_need_count = needs_count(values_encoding);
    const bool lengths_need_count = needs_count(lengths_encoding);
    if (values_need_count && lengths_need_count) {
        error = "rle chunk's values and run lengths both need the run count";
        return false;
    }

    // One of the two sizes the runs: a self-sizing child gives the count to the other.
    std::vector<std::uint8_t> lengths_raw;
    if (!values_need_count && !lengths_need_count) {
        if (!decode(values_encoding, values, std::nullopt, bits, values_raw, error) ||
            !decode(lengths_encoding, lengths, std::nullopt, length_bits, lengths_raw, error)) {
            return false;
        }
    } else if (values_need_count) {
        if (!decode(lengths_encoding, lengths, std::nullopt, length_bits, lengths_raw, error)) {
            return false;
        }
        if (lengths_raw.size() % length_width != 0U) {
            error = "rle run lengths are not a whole number of runs";
            return false;
        }
        const auto runs = static_cast<std::uint64_t>(lengths_raw.size() / length_width);
        if (!decode(values_encoding, values, runs, bits, values_raw, error)) {
            return false;
        }
    } else {
        if (!decode(values_encoding, values, std::nullopt, bits, values_raw, error)) {
            return false;
        }
        if (values_raw.size() % value_width != 0U) {
            error = "rle chunk values are not a whole number of runs";
            return false;
        }
        const auto runs = static_cast<std::uint64_t>(values_raw.size() / value_width);
        if (!decode(lengths_encoding, lengths, runs, length_bits, lengths_raw, error)) {
            return false;
        }
    }

    if (values_raw.size() % value_width != 0U || lengths_raw.size() % length_width != 0U) {
        error = "rle chunk buffer sizes invalid";
        return false;
    }
    const std::size_t runs = values_raw.size() / value_width;
    if (lengths_raw.size() / length_width != runs) {
        error = "rle run count mismatch between values and lengths";
        return false;
    }
    run_lengths.clear();
    run_lengths.reserve(runs);
    for (std::size_t r = 0; r < runs; ++r) {
        run_lengths.push_back(load_run(lengths_raw, r * length_width, length_width));
    }
    return true;
}

}  // namespace

bool decode_rle_runs(const Compressive& values_encoding, const Compressive& lengths_encoding,
                     const std::vector<std::uint8_t>& values, const std::vector<std::uint8_t>& lengths,
                     page_layout::BufferScheme values_scheme, std::uint32_t bits, std::uint32_t length_bits,
                     std::vector<std::uint8_t>& values_raw, std::vector<std::uint64_t>& run_lengths,
                     std::string& error) {
    std::vector<std::uint8_t> framed_values;
    const std::vector<std::uint8_t>* raw_values = &values;
    if (values_scheme != page_layout::BufferScheme::kNone) {
        bool known = false;
        const char* scheme = scheme_name(values_scheme, known);
        if (!known) {
            error = "rle chunk uses an unsupported buffer compression";
            return false;
        }
        if (!general::decompress(scheme, values.data(), values.size(), framed_values, error)) {
            name_rle_buffers(error, values, lengths);
            return false;
        }
        raw_values = &framed_values;
    }
    if (!decode_rle_runs_impl(values_encoding, lengths_encoding, *raw_values, lengths, bits, length_bits, values_raw,
                              run_lengths, error)) {
        name_rle_buffers(error, values, lengths);
        return false;
    }
    return true;
}

bool decode_rle_chunk(const Compressive& values_encoding, const Compressive& lengths_encoding,
                      const std::vector<std::uint8_t>& values, const std::vector<std::uint8_t>& lengths,
                      page_layout::BufferScheme values_scheme, std::uint32_t bits, std::uint32_t length_bits,
                      std::vector<std::uint8_t>& out, std::string& error) {
    std::vector<std::uint8_t> values_raw;
    std::vector<std::uint64_t> run_lengths;
    if (!decode_rle_runs(values_encoding, lengths_encoding, values, lengths, values_scheme, bits, length_bits,
                         values_raw, run_lengths, error)) {
        return false;
    }
    const std::size_t value_width = bits / 8U;
    std::uint64_t total = 0;
    for (std::size_t r = 0; r < run_lengths.size(); ++r) {
        std::uint64_t added = 0;
        if (!checked_mul(run_lengths[r], value_width, added) || !checked_add(total, added, total) ||
            total > default_read_limits().max_uncompressed_bytes) {
            error = "rle run expansion overflows";
            name_rle_buffers(error, values, lengths);
            return false;
        }
    }
    out.reserve(out.size() + static_cast<std::size_t>(total));
    for (std::size_t r = 0; r < run_lengths.size(); ++r) {
        const std::uint8_t* value = values_raw.data() + r * value_width;
        for (std::uint64_t i = 0; i < run_lengths[r]; ++i) {
            out.insert(out.end(), value, value + value_width);
        }
    }
    return true;
}

}  // namespace nano_lance::child_block
