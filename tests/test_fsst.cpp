// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// FSST symbol-table parsing and decompression. Both take bytes straight from an untrusted file, so
// the cases here are mostly about what must be REFUSED: the real end-to-end coverage (decoding a
// string column pylance actually wrote) lives in bindings/python/tests/test_lance_parity.py, where
// pylance is available to produce one.

#include "nanolance/fsst.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace fsst = nano_lance::fsst;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

/// Build a serialized symbol table the way Lance's encoder does: a u64 header, then one u64 per
/// symbol, then one length byte per symbol immediately after them (NOT at a fixed offset past all
/// 256 slots), zero-padded to the fixed size.
std::vector<std::uint8_t> make_table(const std::vector<std::string>& symbols, bool encoder_switch) {
    std::vector<std::uint8_t> bytes(fsst::kSymbolTableBytes, 0U);
    std::uint64_t header = std::uint64_t{0x46535354U} << 32U;
    if (encoder_switch) {
        header |= std::uint64_t{1} << 24U;
    }
    header |= static_cast<std::uint64_t>(symbols.size());
    for (std::size_t i = 0; i < 8U; ++i) {
        bytes[i] = static_cast<std::uint8_t>((header >> (8U * i)) & 0xFFU);
    }
    std::size_t pos = 8U;
    for (const auto& symbol : symbols) {
        for (std::size_t b = 0; b < symbol.size(); ++b) {
            bytes[pos + b] = static_cast<std::uint8_t>(symbol[b]);
        }
        pos += 8U;
    }
    for (const auto& symbol : symbols) {
        bytes[pos++] = static_cast<std::uint8_t>(symbol.size());
    }
    return bytes;
}

std::string decode(const fsst::SymbolTable& table, const std::vector<std::uint8_t>& codes,
                   std::string& error) {
    std::vector<std::uint8_t> out;
    if (!fsst::decompress_value(table, codes.data(), codes.size(), out, error)) {
        return {};
    }
    return std::string(out.begin(), out.end());
}

void table_shape_is_validated() {
    fsst::SymbolTable table;
    std::string error;

    require(!fsst::parse_symbol_table({}, table, error), "an empty symbol table must be refused");
    require(error.find("expected") != std::string::npos, "the size refusal names the expected size");

    auto short_table = make_table({"ab"}, true);
    short_table.pop_back();
    require(!fsst::parse_symbol_table(short_table, table, error), "a short symbol table is refused");

    auto wrong_magic = make_table({"ab"}, true);
    wrong_magic[7] = 0U;
    require(!fsst::parse_symbol_table(wrong_magic, table, error), "the magic is checked");
    require(error.find("magic") != std::string::npos, "the magic refusal says so");

    // Length 0 and length 9 are both outside FSST's 1..8, and both would break the decode loop:
    // 0 silently emits nothing, 9 reads past the symbol.
    for (const std::uint8_t bad_length : {std::uint8_t{0}, std::uint8_t{9}}) {
        auto bad = make_table({"ab"}, true);
        bad[8U + 8U] = bad_length;  // the single symbol's length byte
        require(!fsst::parse_symbol_table(bad, table, error),
                "symbol length " + std::to_string(bad_length) + " is refused");
    }
}

void a_parsed_table_reports_what_it_holds() {
    fsst::SymbolTable table;
    std::string error;
    require(fsst::parse_symbol_table(make_table({"the ", "qu", "ick"}, true), table, error),
            "a well-formed table parses: " + error);
    require(!table.passthrough, "encoder_switch on means the values really are compressed");
    require(table.symbol_count == 3U, "the symbol count comes from the header's low byte");
    require(table.lengths[0] == 4U && table.lengths[1] == 2U && table.lengths[2] == 3U,
            "each symbol's declared length is kept");
}

void codes_expand_to_their_symbols() {
    fsst::SymbolTable table;
    std::string error;
    require(fsst::parse_symbol_table(make_table({"the ", "qu", "ick"}, true), table, error), error);

    require(decode(table, {0, 1, 2}, error) == "the quick", "codes index the symbol table");
    require(decode(table, {}, error).empty(), "an empty value decodes to an empty value");

    // 255 escapes the next byte, which is how FSST spells a byte no symbol covers.
    require(decode(table, {0, 255, '!'}, error) == "the !", "escape emits the next byte verbatim");
    require(decode(table, {255, 255, 255, 255}, error) == std::string("\xFF\xFF", 2),
            "an escaped 0xFF is itself");
}

void corrupt_streams_are_refused_rather_than_truncated() {
    fsst::SymbolTable table;
    std::string error;
    require(fsst::parse_symbol_table(make_table({"the ", "qu", "ick"}, true), table, error), error);

    std::vector<std::uint8_t> out;
    error.clear();
    require(!fsst::decompress_value(table, std::vector<std::uint8_t>{0, 255}.data(), 2U, out, error),
            "an escape with no payload left in the value is refused");
    require(error.find("escape") != std::string::npos, "the refusal names the escape");

    error.clear();
    out.clear();
    require(!fsst::decompress_value(table, std::vector<std::uint8_t>{7}.data(), 1U, out, error),
            "a code past the end of the symbol table is refused");
    require(error.find("symbol table") != std::string::npos, "the refusal says the code is unknown");
}

void an_encoder_that_declined_passes_bytes_through() {
    // Lance skips FSST below 32 KiB of input but still writes the wrapper, with encoder_switch clear
    // and no symbols. That is most small files, so it has to be exactly a copy.
    fsst::SymbolTable table;
    std::string error;
    require(fsst::parse_symbol_table(make_table({}, false), table, error), error);
    require(table.passthrough, "encoder_switch clear means passthrough");
    require(table.symbol_count == 0U, "a passthrough table declares no symbols");

    const std::string raw = "these bytes were never compressed\xFF\x00 at all";
    std::vector<std::uint8_t> out;
    require(fsst::decompress_value(table, reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size(),
                                   out, error),
            "passthrough never fails: " + error);
    require(std::string(out.begin(), out.end()) == raw, "passthrough copies the bytes unchanged");

    // 255 is a literal byte here, not an escape -- the whole point of passthrough.
    out.clear();
    require(fsst::decompress_value(table, std::vector<std::uint8_t>{255}.data(), 1U, out, error), error);
    require(out.size() == 1U && out[0] == 255U, "passthrough does not interpret 0xFF as an escape");
}

void values_accumulate_onto_one_buffer() {
    // The decoder appends value after value onto the column's data buffer, so decompress_value must
    // not clear what is already there.
    fsst::SymbolTable table;
    std::string error;
    require(fsst::parse_symbol_table(make_table({"ab", "cd"}, true), table, error), error);
    std::vector<std::uint8_t> out{'-'};
    const std::vector<std::uint8_t> first{0};
    const std::vector<std::uint8_t> second{1};
    require(fsst::decompress_value(table, first.data(), first.size(), out, error), error);
    require(fsst::decompress_value(table, second.data(), second.size(), out, error), error);
    require(std::string(out.begin(), out.end()) == "-abcd", "values append");
}

}  // namespace

int main() {
    table_shape_is_validated();
    a_parsed_table_reports_what_it_holds();
    codes_expand_to_their_symbols();
    corrupt_streams_are_refused_rather_than_truncated();
    an_encoder_that_declined_passes_bytes_through();
    values_accumulate_onto_one_buffer();
    std::cout << "fsst tests passed\n";
    return 0;
}
