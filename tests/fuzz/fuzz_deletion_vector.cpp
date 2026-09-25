// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// libFuzzer harness over the deletion-file parsers (src/deletion_vector.cpp): a hand-written Arrow
// IPC file reader (magic, continuation markers, a flatbuffer vtable walk, a zstd-compressed body)
// and a roaring bitmap reader (two cookies, an optional run-flag header, an optional offset header,
// and three container shapes).
//
// This is the only untrusted-input parser in the tree that had no fuzzer. Everything it reads comes
// from `_deletions/*.arrow` and `_deletions/*.bin` inside a dataset directory -- the same trust level
// as the data file itself, which fuzz_decode already covers. Neither format is reachable from
// fuzz_decode, because that harness stops at the data file and never opens a deletion file.
//
// Both entry points are exposed from the header for exactly this reason, so the harness feeds them
// the fuzzer's bytes directly rather than staging a dataset on disk.
//
// It asserts CONTRACTS, not just absence of crashes:
//   * a refusal always says why;
//   * a successful roaring parse yields STRICTLY ASCENDING values -- the caller builds a keep-mask by
//     walking them in order against ascending row offsets, so an unsorted or duplicated result would
//     silently drop the wrong rows rather than fail;
//   * parsing is deterministic and replaces the output vector rather than appending to it.
//
// Build with -DNANOLANCE_BUILD_FUZZERS=ON on a Clang toolchain.

#include "nanolance/deletion_vector.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

void check_roaring(const std::vector<std::uint8_t>& bytes) {
    // A cap in the range a real deletion file lives in. The production caller always passes the
    // manifest's num_deleted_rows; passing 0 here would fall back to the reader's generic
    // per-buffer budget (2.1 billion offsets), and the fuzzer would spend its time in the allocator
    // instead of in the parser. The budget LOGIC is covered by
    // tests/test_read_safety.cpp::test_roaring_bitmap_expansion_is_budgeted.
    constexpr std::uint64_t kMaxValues = 1U << 16U;
    static const std::vector<std::uint32_t> kPrefix{7U, 7U, 7U};
    std::vector<std::uint32_t> out = kPrefix;
    std::string error;
    if (!nano_lance::parse_roaring_bitmap(bytes, out, error, kMaxValues)) {
        assert(!error.empty() && "a refused bitmap must say why");
        return;
    }
    for (std::size_t i = 1; i < out.size(); ++i) {
        assert(out[i - 1U] < out[i] && "roaring values must come back strictly ascending");
    }

    std::vector<std::uint32_t> again{9U};
    std::string again_error;
    const bool ok_again = nano_lance::parse_roaring_bitmap(bytes, again, again_error, kMaxValues);
    assert(ok_again && again == out && "parsing must be deterministic and replace the output");
    (void)ok_again;
}

void check_arrow_ipc(const std::vector<std::uint8_t>& bytes) {
    static const std::vector<std::uint32_t> kPrefix{5U};
    std::vector<std::uint32_t> out = kPrefix;
    std::string error;
    // Same reasoning as check_roaring: production always passes the manifest's deleted-row count.
    constexpr std::uint64_t kMaxValues = 1U << 16U;
    if (!nano_lance::parse_arrow_ipc_uint32_column(bytes, out, error, kMaxValues)) {
        assert(!error.empty() && "a refused IPC file must say why");
        return;
    }

    std::vector<std::uint32_t> again{3U, 3U};
    std::string again_error;
    const bool ok_again = nano_lance::parse_arrow_ipc_uint32_column(bytes, again, again_error, kMaxValues);
    assert(ok_again && again == out && "parsing must be deterministic and replace the output");
    (void)ok_again;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::vector<std::uint8_t> bytes(data, data + size);
    check_roaring(bytes);

    // The IPC reader rejects anything without the magic in its first bytes, so almost every random
    // input would stop at the first check. Prepend the magic (padded as arrow-rs writes it) so the
    // fuzzer spends its budget on the framing and flatbuffer walk behind it.
    check_arrow_ipc(bytes);
    std::vector<std::uint8_t> framed;
    framed.reserve(size + 64U);
    static const char kMagic[] = "ARROW1\0\0";
    framed.insert(framed.end(), kMagic, kMagic + 8);
    framed.resize(64U, 0U);
    framed.insert(framed.end(), bytes.begin(), bytes.end());
    check_arrow_ipc(framed);

    return 0;
}
