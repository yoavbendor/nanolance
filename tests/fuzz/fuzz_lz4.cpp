// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// libFuzzer harness over the LZ4 block decompressor (src/lz4_block.cpp), which walks untrusted
// tokens, literal runs and back-references from a Lance-written buffer.
//
// fuzz_decode does not reach here: it stops at the data file's footer and column metadata, and LZ4
// only runs once a dictionary or value buffer is being decompressed.
//
// It asserts CONTRACTS, not just absence of crashes:
//   * a refusal always says why;
//   * a success produces EXACTLY the declared number of bytes -- the caller's later bounds checks
//     are written against that, so a short decode would be worse than a refusal;
//   * the output buffer is replaced, never appended to (callers reuse one scratch buffer);
//   * decoding is deterministic.
//
// The 4-byte size prefix is taken from the fuzzer's own bytes but clamped, so the interesting cases
// (a size the block cannot fill, a block that overruns it) stay reachable without every input
// turning into an allocation test.
//
// Build with -DNANOLANCE_BUILD_FUZZERS=ON on a Clang toolchain.

#include "nanolance/lz4_block.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lz4 = nano_lance::lz4_block;

namespace {

void check_one(const std::vector<std::uint8_t>& sized) {
    static const std::vector<std::uint8_t> kPrefix{'p', 'r', 'e'};
    std::vector<std::uint8_t> out = kPrefix;
    std::string error;
    const bool ok = lz4::decompress_sized(sized, out, error);
    if (!ok) {
        assert(!error.empty() && "a refused block must say why");
        return;
    }
    assert(sized.size() >= 4U);
    const std::size_t declared = static_cast<std::size_t>(sized[0]) |
                                 (static_cast<std::size_t>(sized[1]) << 8U) |
                                 (static_cast<std::size_t>(sized[2]) << 16U) |
                                 (static_cast<std::size_t>(sized[3]) << 24U);
    assert(out.size() == declared && "a successful decode produces exactly the declared size");

    std::vector<std::uint8_t> again{'x'};
    std::string again_error;
    const bool ok_again = lz4::decompress_sized(sized, again, again_error);
    assert(ok_again && again == out && "decoding must be deterministic and replace the buffer");
    (void)ok_again;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // 1. The raw bytes, size prefix and all.
    check_one(std::vector<std::uint8_t>(data, data + size));

    if (size < 4U) {
        return 0;
    }

    // 2. The same block with a plausible declared size, so the decoder gets past the budget check and
    //    into the token loop on inputs whose first four bytes happen to be enormous.
    std::vector<std::uint8_t> clamped(data, data + size);
    const std::size_t plausible = (size - 4U) * 4U + 1U;
    clamped[0] = static_cast<std::uint8_t>(plausible & 0xFFU);
    clamped[1] = static_cast<std::uint8_t>((plausible >> 8U) & 0xFFU);
    clamped[2] = static_cast<std::uint8_t>((plausible >> 16U) & 0xFFU);
    clamped[3] = static_cast<std::uint8_t>((plausible >> 24U) & 0xFFU);
    check_one(clamped);

    return 0;
}
