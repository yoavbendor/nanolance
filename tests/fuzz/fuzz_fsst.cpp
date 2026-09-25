// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// libFuzzer harness over the FSST decompressor (src/fsst.cpp), which reads a symbol table out of an
// untrusted page descriptor and then walks untrusted code bytes against it.
//
// fuzz_decode does not reach here: it stops at the data file's footer and column metadata, and FSST
// only runs once a column's pages are being decoded. This target goes straight at it.
//
// It asserts CONTRACTS, not just absence of crashes:
//   * a refused parse always says why, and leaves no table behind;
//   * a decode either fails or expands by at most 8x (one code, one <=8-byte symbol) -- the bound the
//     caller's allocation budget relies on;
//   * a passthrough table copies its input exactly;
//   * decoding appends, never truncates what the caller already had;
//   * both are deterministic.
//
// Build with -DNANOLANCE_BUILD_FUZZERS=ON on a Clang toolchain.

#include "nanolance/fsst.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fsst = nano_lance::fsst;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // 1. The raw bytes as a symbol table. Almost always refused (the magic has to be right), which is
    //    the point: the refusal path must stay total and must not publish a half-built table.
    {
        const std::vector<std::uint8_t> bytes(data, data + size);
        fsst::SymbolTable table;
        table.symbol_count = 0xABCDU;  // a sentinel a failed parse must not overwrite
        std::string error;
        if (!fsst::parse_symbol_table(bytes, table, error)) {
            assert(!error.empty() && "a refused symbol table must say why");
            assert(table.symbol_count == 0xABCDU && "a refused parse must not publish a table");
        }
    }

    if (size <= fsst::kSymbolTableBytes) {
        return 0;
    }

    // 2. Force the magic in so the parse gets past its first check, and let the fuzzer drive
    //    everything else in the header (n_symbols, encoder_switch) and every symbol byte.
    std::vector<std::uint8_t> table_bytes(data, data + fsst::kSymbolTableBytes);
    table_bytes[4] = 0x54U;  // "FSST" occupies the header's top 32 bits, little-endian
    table_bytes[5] = 0x53U;
    table_bytes[6] = 0x53U;
    table_bytes[7] = 0x46U;

    fsst::SymbolTable table;
    std::string error;
    if (!fsst::parse_symbol_table(table_bytes, table, error)) {
        assert(!error.empty() && "a refused symbol table must say why");
        return 0;
    }

    const std::uint8_t* codes = data + fsst::kSymbolTableBytes;
    const std::size_t code_count = size - fsst::kSymbolTableBytes;

    static const std::vector<std::uint8_t> kPrefix{'p', 'r', 'e'};
    std::vector<std::uint8_t> out = kPrefix;
    std::string decode_error;
    const bool ok = fsst::decompress_value(table, codes, code_count, out, decode_error);
    if (!ok) {
        assert(!decode_error.empty() && "a refused decode must say why");
        return 0;
    }
    assert(out.size() >= kPrefix.size() && "decoding appends; it must never shrink the buffer");
    assert(std::memcmp(out.data(), kPrefix.data(), kPrefix.size()) == 0 &&
           "decoding must not disturb bytes the caller already had");
    const std::size_t produced = out.size() - kPrefix.size();
    assert(produced <= code_count * fsst::kMaxSymbolLength && "a code expands to at most 8 bytes");
    if (table.passthrough) {
        assert(produced == code_count && "a passthrough table copies its input exactly");
        assert(std::memcmp(out.data() + kPrefix.size(), codes, code_count) == 0 &&
               "a passthrough table copies its input unchanged");
    }

    // 3. Deterministic: the same table and the same codes must decode the same way twice.
    std::vector<std::uint8_t> again = kPrefix;
    std::string again_error;
    const bool ok_again = fsst::decompress_value(table, codes, code_count, again, again_error);
    assert(ok_again == ok && again == out && "decoding must be deterministic");
    (void)ok_again;

    return 0;
}
