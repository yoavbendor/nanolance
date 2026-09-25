// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// libFuzzer harness over the FSST ENCODER (roadmap F2): symbol-table training and compression,
// checked against the decoder the reader uses.
//
// The input's first byte is a separator; the rest, split on it, is the column's values -- so the
// fuzzer controls how many values there are, how long, and every byte in them (including 0xFF,
// the escape code, as data). The contracts:
//   * every value decodes back to exactly itself, through the SERIALIZED table and parse_symbol_table
//     -- the path every written page takes;
//   * a compressed value is at most twice its input (every byte escaped) -- the bound the writer
//     sizes its buffer with;
//   * training is deterministic: the same values give the same serialized table.
//
// Build with -DNANOLANCE_BUILD_FUZZERS=ON on a Clang toolchain.

#include "nanolance/fsst.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fsst = nano_lance::fsst;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2U) {
        return 0;
    }
    const auto separator = data[0];
    std::vector<std::pair<const std::uint8_t*, std::size_t>> values;
    std::size_t start = 1;
    for (std::size_t i = 1; i <= size; ++i) {
        if (i == size || data[i] == separator) {
            values.emplace_back(data + start, i - start);
            start = i + 1U;
        }
    }

    fsst::Encoder encoder;
    if (!fsst::train(values, encoder)) {
        return 0;  // nothing learned (e.g. only empty values): the writer stores them plain
    }
    assert(encoder.symbol_count >= 1U && encoder.symbol_count <= 255U && "a table holds 1..255 symbols");

    const auto serialized = fsst::serialize(encoder);
    fsst::Encoder again;
    assert(fsst::train(values, again) && fsst::serialize(again) == serialized && "training is deterministic");

    fsst::SymbolTable table;
    std::string error;
    const bool parsed = fsst::parse_symbol_table(serialized, table, error);
    assert(parsed && "a serialized table must parse");
    (void)parsed;

    for (const auto& [value, length] : values) {
        std::vector<std::uint8_t> compressed;
        fsst::compress_value(encoder, value, length, compressed);
        assert(compressed.size() <= 2U * length && "a value compresses to at most twice its size");
        std::vector<std::uint8_t> back;
        const bool ok = fsst::decompress_value(table, compressed.data(), compressed.size(), back, error);
        assert(ok && "what the encoder writes, the decoder reads");
        (void)ok;
        assert(back.size() == length && std::equal(back.begin(), back.end(), value) && "values round-trip exactly");
    }
    return 0;
}
