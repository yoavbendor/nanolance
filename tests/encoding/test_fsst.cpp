// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// FSST symbol-table parsing and decompression. Both take bytes straight from an untrusted file, so
// the cases here are mostly about what must be REFUSED: the real end-to-end coverage (decoding a
// string column pylance actually wrote) lives in bindings/python/tests/test_lance_parity.py, where
// pylance is available to produce one.

#include "nanolance/fsst.hpp"

#include <nanom/fsst_encode.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
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

/// compress_into_padded (one value, 7 readable bytes after it) and compress_into_padded4 (four values at
/// once) must emit exactly what compress_into emits, for every value in a column buffer with trailing padding.
void the_padded_compressors_match_compress_into() {
    std::vector<std::string> strings;
    std::uint64_t x = 0x2545F4914F6CDD1DULL;
    for (int i = 0; i < 6000; ++i) {
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
        std::string v = "item/" + std::to_string(x % 99991) + "/lane-" + std::to_string(i % 13);
        v.resize(static_cast<std::size_t>(i % 61));
        if (i % 7 == 0) {
            v.push_back(static_cast<char>(x >> 32U));  // bytes no symbol covers
        }
        strings.push_back(v);
    }
    fsst::Encoder encoder;
    require(fsst::train(views(strings), encoder), "training succeeds");
    std::vector<std::uint8_t> column;
    std::vector<std::size_t> begin;
    for (const auto& s : strings) {
        begin.push_back(column.size());
        column.insert(column.end(), s.begin(), s.end());
    }
    column.resize(column.size() + 8U, 0U);  // every value has 7 readable bytes after it
    std::vector<std::uint8_t> want(2 * 64 + 8);
    std::vector<std::uint8_t> got(2 * 64 + 8);
    const auto size_of = [&](std::size_t i) { return strings[i].size(); };
    for (std::size_t i = 0; i < strings.size(); ++i) {
        const auto n = fsst::compress_into(encoder, column.data() + begin[i], size_of(i), want.data());
        const auto m = fsst::compress_into_padded(encoder, column.data() + begin[i], size_of(i), got.data());
        require(n == m && std::equal(want.begin(), want.begin() + static_cast<std::ptrdiff_t>(n), got.begin()),
                "padded compression agrees on '" + strings[i] + "'");
    }
    for (std::size_t i = 0; i + 4U <= strings.size(); i += 4U) {
        const std::uint8_t* data[4];
        std::size_t sizes[4];
        std::uint8_t* dst[4];
        std::size_t written[4];
        std::vector<std::uint8_t> out(4 * (2 * 64 + 8));
        for (std::size_t k = 0; k < 4; ++k) {
            data[k] = column.data() + begin[i + k];
            sizes[k] = size_of(i + k);
            dst[k] = out.data() + k * (2 * 64 + 8);
        }
        fsst::compress_into_padded4(encoder, data, sizes, dst, written);
        for (std::size_t k = 0; k < 4; ++k) {
            const auto n = fsst::compress_into(encoder, data[k], sizes[k], want.data());
            require(written[k] == n && std::equal(want.begin(), want.begin() + static_cast<std::ptrdiff_t>(n), dst[k]),
                    "four-lane compression agrees on '" + strings[i + k] + "'");
        }
    }
}

void nothing_to_train_on_is_declined() {
    fsst::Encoder encoder;
    require(!fsst::train({}, encoder), "no values: no table");
    require(!fsst::train(views({"", "", ""}), encoder), "only empty values: no table");
}

}  // namespace


/// Words that give long symbols, so the kernel's jobs end inside matches.
std::string word_text(std::mt19937_64& rng, std::size_t size) {
    static const char* const words[] = {"alpha ", "beta ", "gamma ", "delta ", "epsilon ", "river ", "stone ", "x"};
    std::string out;
    while (out.size() < size) {
        out += words[rng() % 8U];
    }
    out.resize(size);
    return out;
}

/// A table by hand (no training): symbols as byte strings, codes in order. Mirrors what train() leaves behind.
fsst::Encoder table_of(const std::vector<std::string>& symbols) {
    namespace ed = ::nanom::codec::fsst::enc_detail;
    fsst::Encoder e;
    ed::reset(e);
    for (const auto& s : symbols) {
        std::uint64_t value = 0;
        for (std::size_t k = 0; k < s.size(); ++k) {
            value |= std::uint64_t{static_cast<std::uint8_t>(s[k])} << (8U * k);
        }
        require(ed::add(e, value, static_cast<unsigned>(s.size())), "hand-built table refused a symbol");
    }
    e.short_or_byte.resize(65536);
    for (std::uint32_t w = 0; w < 65536; ++w) {
        const auto two = e.short_codes[w];
        const auto one = e.byte_codes[w & 0xFF];
        e.short_or_byte[w] = two != fsst::Encoder::kNone   ? static_cast<std::uint16_t>((2U << 8U) | two)
                             : one != fsst::Encoder::kNone ? static_cast<std::uint16_t>((1U << 8U) | one)
                                                           : static_cast<std::uint16_t>((1U << 8U) | fsst::kEscape);
    }
    fsst::attach_batch_table(e);
    return e;
}

/// Every value compressed by the batch entry point must equal compress_into's bytes, the same for the dispatching
/// compress_many. Where this build or CPU has no kernel, only the portable path runs, and the test says so.
void batch_output_matches_compress_into(const fsst::Encoder& e, const std::vector<std::string>& values,
                                        const std::string& label) {
    std::vector<std::vector<std::uint8_t>> expected(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(values[i].data());
        expected[i].resize(2U * values[i].size() + 8U);
        expected[i].resize(fsst::compress_into(e, p, values[i].size(), expected[i].data()));
    }
    for (int dispatch = 0; dispatch < 2; ++dispatch) {
        std::vector<std::vector<std::uint8_t>> out(values.size());
        std::vector<fsst::Item> items(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            out[i].assign(2U * values[i].size() + 8U, 0xEEU);
            items[i] = fsst::Item{reinterpret_cast<const std::uint8_t*>(values[i].data()), values[i].size(),
                                  out[i].data(), false, 0U};
        }
        if (dispatch == 0) {
            if (!fsst::compress_many_avx512(e, items.data(), items.size())) {
                std::cout << "note: " << label << ": AVX-512 kernel unavailable here; portable path only\n";
                fsst::compress_many_portable(e, items.data(), items.size());
            }
        } else {
            fsst::compress_many(e, items.data(), items.size());
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            require(items[i].written == expected[i].size(),
                    label + ": value " + std::to_string(i) + " (" + std::to_string(values[i].size()) +
                        " bytes) wrote " + std::to_string(items[i].written) + ", compress_into " +
                        std::to_string(expected[i].size()));
            require(std::memcmp(out[i].data(), expected[i].data(), expected[i].size()) == 0,
                    label + ": value " + std::to_string(i) + " (" + std::to_string(values[i].size()) +
                        " bytes) differs from compress_into");
        }
    }
}

/// The batch output is compress_into's, byte for byte, on random values and on the lengths where a job ends
/// (511 and 512 bytes, the cut at 503), with values that have symbols straddling the cut, escapes at it, and empties.
void batch_output_is_compress_into_bytes() {
    std::mt19937_64 rng(20260417);
    std::vector<std::string> corpus;
    for (int i = 0; i < 400; ++i) {
        corpus.push_back(word_text(rng, 40 + rng() % 1500));
    }
    const auto e = [&] {
        std::vector<std::pair<const std::uint8_t*, std::size_t>> views;
        for (const auto& v : corpus) {
            views.push_back({reinterpret_cast<const std::uint8_t*>(v.data()), v.size()});
        }
        fsst::Encoder enc;
        require(fsst::train(views, enc), "training on the corpus");
        return enc;
    }();

    std::vector<std::string> values;
    for (const std::size_t n : {0U, 1U, 2U, 3U, 6U, 7U, 8U, 9U, 502U, 503U, 504U, 505U, 506U, 507U, 508U, 509U, 510U,
                                511U, 512U, 513U, 1022U, 1023U, 1024U, 1025U, 1533U, 1534U, 1535U, 2100U}) {
        values.push_back(word_text(rng, n));
    }
    // Binary values: escapes everywhere, including at the cut.
    for (int i = 0; i < 120; ++i) {
        std::string v(rng() % 1200, '\0');
        for (auto& c : v) {
            c = static_cast<char>(rng());
        }
        values.push_back(v);
    }
    // Text with a symbol straddling bytes 503..511 in every alignment.
    for (std::size_t shift = 0; shift < 16; ++shift) {
        std::string v = word_text(rng, 600 + shift);
        values.push_back(v.substr(shift));
    }
    for (int i = 0; i < 1500; ++i) {
        values.push_back(word_text(rng, rng() % 9));
    }
    for (int i = 0; i < 300; ++i) {
        values.push_back(word_text(rng, rng() % 3000));
    }
    batch_output_matches_compress_into(e, values, "random and boundary values");
}

/// A table whose symbols hold every byte value has no terminator byte: the kernel is not built for it, and the
/// batch entry point runs the portable compressors. The output is still compress_into's.
void table_without_a_terminator_falls_back() {
    std::vector<std::string> symbols;
    for (int b = 0; b < 256; b += 2) {
        symbols.push_back(std::string{static_cast<char>(b), static_cast<char>(b + 1)});
    }
    const auto e = table_of(symbols);
    require(e.batch == nullptr, "a table with no terminator byte must not get a kernel table");
    std::mt19937_64 rng(3);
    std::vector<std::string> values;
    for (int i = 0; i < 200; ++i) {
        std::string v(rng() % 900, '\0');
        for (auto& c : v) {
            c = static_cast<char>(rng());
        }
        values.push_back(v);
    }
    std::vector<std::uint8_t> out(2U * 900U + 8U);
    fsst::Item item{nullptr, 0, out.data(), false, 0U};
    require(!fsst::compress_many_avx512(e, &item, 1U), "no kernel table: the kernel must decline");
    batch_output_matches_compress_into(e, values, "terminator fallback");
}

/// The batch path also serves a table that has a terminator (the one-byte symbols leave a byte free).
void table_with_a_terminator_uses_the_kernel_when_there_is_one() {
    const auto e = table_of({"ab", "abc", "bcdefg", "e", "f", "gh", "hello"});
    require(e.batch != nullptr, "a table with a free terminator byte gets a kernel table");
    std::mt19937_64 rng(11);
    std::vector<std::string> values;
    for (int i = 0; i < 300; ++i) {
        std::string v(rng() % 1500, 'a');
        for (auto& c : v) {
            c = "abcdefgh"[rng() % 8U];
        }
        values.push_back(v);
    }
    batch_output_matches_compress_into(e, values, "hand-built table with a terminator");
}

int main() {
    encoder_round_trips_and_compresses();
    encoder_is_deterministic();
    the_fast_compressor_matches_the_plain_one();
    nothing_to_train_on_is_declined();
    the_padded_compressors_match_compress_into();
    table_shape_is_validated();
    a_parsed_table_reports_what_it_holds();
    codes_expand_to_their_symbols();
    corrupt_streams_are_refused_rather_than_truncated();
    an_encoder_that_declined_passes_bytes_through();
    values_accumulate_onto_one_buffer();
    batch_output_is_compress_into_bytes();
    table_without_a_terminator_falls_back();
    table_with_a_terminator_uses_the_kernel_when_there_is_one();
    std::cout << "fsst tests passed\n";
    return 0;
}
