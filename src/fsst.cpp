// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/fsst.hpp"

#include "nanolance/read_safety.hpp"

namespace nano_lance::fsst {

namespace {

/// "FSST" in the header's top 32 bits. The low 32 hold encoder_switch / suffix_lim / terminator /
/// n_symbols, one byte each.
constexpr std::uint64_t kMagic = std::uint64_t{0x46535354U} << 32U;
constexpr std::uint64_t kEncoderSwitchBit = std::uint64_t{1} << 24U;

}  // namespace

bool parse_symbol_table(const std::vector<std::uint8_t>& bytes, SymbolTable& out, std::string& error) {
    if (bytes.size() != kSymbolTableBytes) {
        error = "FSST symbol table is " + std::to_string(bytes.size()) + " bytes, expected " +
                std::to_string(kSymbolTableBytes);
        return false;
    }
    const auto header = load_le<std::uint64_t>(bytes.data());
    if ((header & kMagic) != kMagic) {
        error = "FSST symbol table has the wrong magic";
        return false;
    }

    // Parse into a scratch value and publish only on success, so a caller that ignores the return
    // value cannot be handed a half-built table (the same discipline decode_page_layout follows).
    SymbolTable parsed;
    parsed.passthrough = (header & kEncoderSwitchBit) == 0U;
    parsed.symbol_count = static_cast<std::uint32_t>(header & 0xFFU);

    // Symbols come first, then one length byte per symbol immediately after them -- NOT at a fixed
    // offset past all 256 slots. The table is sized for 256 and zero-padded, so the lengths of a
    // 120-symbol table sit at byte 8 + 120*8, not 8 + 256*8.
    std::size_t pos = 8U;
    for (std::uint32_t i = 0; i < parsed.symbol_count; ++i) {
        for (std::size_t b = 0; b < kMaxSymbolLength; ++b) {
            parsed.symbols[i][b] = bytes[pos + b];
        }
        pos += kMaxSymbolLength;
    }
    for (std::uint32_t i = 0; i < parsed.symbol_count; ++i) {
        const auto length = bytes[pos++];
        if (length < 1U || length > kMaxSymbolLength) {
            error = "FSST symbol " + std::to_string(i) + " declares length " + std::to_string(length) +
                    ", expected 1..8";
            return false;
        }
        parsed.lengths[i] = length;
    }

    out = parsed;
    return true;
}

bool decompress_value(const SymbolTable& table, const std::uint8_t* data, std::size_t size,
                      std::vector<std::uint8_t>& out, std::string& error) {
    if (table.passthrough) {
        out.insert(out.end(), data, data + size);
        return true;
    }
    // Room for the worst case: every code expands to at most kMaxSymbolLength bytes. Geometric,
    // because this runs once per value -- see reserve_more.
    reserve_more(out, size * kMaxSymbolLength);
    for (std::size_t i = 0; i < size;) {
        const auto code = data[i];
        if (code == kEscape) {
            if (i + 1U >= size) {
                error = "FSST escape at the end of a value has no payload byte";
                return false;
            }
            out.push_back(data[i + 1U]);
            i += 2U;
            continue;
        }
        if (code >= table.symbol_count) {
            error = "FSST code " + std::to_string(code) + " is not in the symbol table (" +
                    std::to_string(table.symbol_count) + " symbols)";
            return false;
        }
        const auto* symbol = table.symbols[code].data();
        out.insert(out.end(), symbol, symbol + table.lengths[code]);
        ++i;
    }
    return true;
}

}  // namespace nano_lance::fsst
