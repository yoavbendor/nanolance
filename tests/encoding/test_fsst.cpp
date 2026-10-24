// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// FSST symbol-table parsing and decompression. Both take bytes straight from an untrusted file, so
// the cases here are mostly about what must be REFUSED: the real end-to-end coverage (decoding a
// string column pylance actually wrote) lives in bindings/python/tests/test_lance_parity.py, where
// pylance is available to produce one.

#include "nanolance/fsst.hpp"

#include <algorithm>
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

// ── The encoder (roadmap F2) ─────────────────────────────────────────────────────────────────

using Values = std::vector<std::pair<const std::uint8_t*, std::size_t>>;

Values views(const std::vector<std::string>& strings) {
    Values out;
    for (const auto& s : strings) {
        out.emplace_back(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    }
    return out;
}

/// Train on `strings`, compress each, and decode through the SERIALIZED table with the reader's
/// own parser: the round trip every written page takes. Returns compressed bytes (table excluded).
std::size_t round_trip(const std::vector<std::string>& strings, const std::string& what) {
    fsst::Encoder encoder;
    require(fsst::train(views(strings), encoder), what + ": training found no symbols");
    require(encoder.symbol_count >= 1U && encoder.symbol_count <= 255U, what + ": 1..255 symbols");
    fsst::SymbolTable table;
    std::string error;
    require(fsst::parse_symbol_table(fsst::serialize(encoder), table, error), what + ": " + error);
    require(!table.passthrough, what + ": a trained table has the encoder switch on");
    std::size_t compressed_bytes = 0;
    for (const auto& s : strings) {
        std::vector<std::uint8_t> compressed;
        fsst::compress_value(encoder, reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), compressed);
        compressed_bytes += compressed.size();
        std::vector<std::uint8_t> back;
        require(fsst::decompress_value(table, compressed.data(), compressed.size(), back, error), what + ": " + error);
        require(std::string(back.begin(), back.end()) == s, what + ": value did not round-trip: " + s);
    }
    return compressed_bytes;
}

void encoder_round_trips_and_compresses() {
    std::vector<std::string> urls;
    std::size_t raw = 0;
    for (int i = 0; i < 20000; ++i) {
        urls.push_back("https://example.com/users/" + std::to_string(i * 7919 % 100003) + "/profile?tab=" +
                       std::to_string(i % 13));
        raw += urls.back().size();
    }
    const auto packed = round_trip(urls, "urls");
    // Repetitive text: pylance's FSST gets these to well under half; so must this one.
    require(packed * 2U < raw, "urls compress to less than half (" + std::to_string(packed) + " of " +
                                   std::to_string(raw) + " bytes)");

    // Every byte value, empty values, one-byte values, values shorter than any symbol, a value of
    // 0xFF bytes (the escape code as DATA), and bytes that only appear once.
    std::vector<std::string> edge{"", "a", "ab", std::string(1, '\0'), std::string(40, '\xFF'), "xyz"};
    std::string all_bytes;
    for (int b = 0; b < 256; ++b) {
        all_bytes.push_back(static_cast<char>(b));
    }
    for (int i = 0; i < 300; ++i) {
        edge.push_back(all_bytes.substr(static_cast<std::size_t>(i % 256)) + all_bytes.substr(0, static_cast<std::size_t>(i % 256)));
        edge.push_back("repeated repeated repeated " + std::to_string(i));
    }
    round_trip(edge, "edge cases");

    // Random bytes: nothing to gain, but it must still round-trip (mostly escapes).
    std::vector<std::string> noise;
    std::uint64_t x = 88172645463325252ULL;
    for (int i = 0; i < 2000; ++i) {
        std::string v;
        for (int j = 0; j < 1 + i % 37; ++j) {
            x ^= x << 13U;
            x ^= x >> 7U;
            x ^= x << 17U;
            v.push_back(static_cast<char>(x & 0xFFU));
        }
        noise.push_back(v);
    }
    round_trip(noise, "noise");
}

void encoder_is_deterministic() {
    std::vector<std::string> strings;
    for (int i = 0; i < 50000; ++i) {
        strings.push_back("customer-" + std::to_string(i * 31 % 9973) + "@mail.example.org");
    }
    fsst::Encoder a;
    fsst::Encoder b;
    require(fsst::train(views(strings), a) && fsst::train(views(strings), b), "training succeeds");
    require(fsst::serialize(a) == fsst::serialize(b), "the same input trains the same table (fixed-seed sample)");
}

/// compress_into has a fast path (8-byte loads, one merged two-byte/one-byte/escape table, a padded
/// tail) next to the plain longest-match loop. Both must emit exactly the same codes: compress with a
/// trained encoder, then with a copy whose merged table is removed (which takes the plain loop).
void the_fast_compressor_matches_the_plain_one() {
    std::vector<std::string> strings;
    std::uint64_t x = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 20000; ++i) {
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
        std::string v = "user-" + std::to_string(x % 1000003) + "@mail" + std::to_string(i % 97) + ".example.org";
        v.resize(static_cast<std::size_t>(i % 53));  // every length 0..52: every tail, every 8-byte step
        if (i % 11 == 0) {
            v.push_back(static_cast<char>(x & 0xFFU));  // bytes no symbol covers: escapes, in the tail too
        }
        strings.push_back(v);
    }
    fsst::Encoder fast;
    require(fsst::train(views(strings), fast), "training succeeds");
    require(!fast.short_or_byte.empty(), "a trained encoder carries the merged lookup table");
    fsst::Encoder plain = fast;
    plain.short_or_byte.clear();
    std::vector<std::uint8_t> a(128);
    std::vector<std::uint8_t> b(128);
    for (const auto& s : strings) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
        const auto na = fsst::compress_into(fast, p, s.size(), a.data());
        const auto nb = fsst::compress_into(plain, p, s.size(), b.data());
        require(na == nb && std::equal(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(na), b.begin()),
                "fast and plain compression agree on '" + s + "'");
    }
}

void nothing_to_train_on_is_declined() {
    fsst::Encoder encoder;
    require(!fsst::train({}, encoder), "no values: no table");
    require(!fsst::train(views({"", "", ""}), encoder), "only empty values: no table");
}

}  // namespace

int main() {
    encoder_round_trips_and_compresses();
    encoder_is_deterministic();
    the_fast_compressor_matches_the_plain_one();
    nothing_to_train_on_is_declined();
    table_shape_is_validated();
    a_parsed_table_reports_what_it_holds();
    codes_expand_to_their_symbols();
    corrupt_streams_are_refused_rather_than_truncated();
    an_encoder_that_declined_passes_bytes_through();
    values_accumulate_onto_one_buffer();
    std::cout << "fsst tests passed\n";
    return 0;
}
