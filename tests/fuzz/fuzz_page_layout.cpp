// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// libFuzzer harness over the /lance.encodings21.PageLayout descriptor parser.
//
// This is untrusted input with a wider blast radius than most: the descriptor is what decode will be
// dispatched on (docs/OPTIMIZATION_PLAN.md section 6 step 2), so a parser that can be steered into a
// bad state chooses the wrong decoder for the page's buffers. It also comes from files written by
// OTHER implementations -- the whole point is reading stock-Lance data -- so "our writer would never
// emit that" is not a defence.
//
// Its own target rather than a branch of fuzz_decode.cpp: libFuzzer's corpus and coverage feedback
// work best aimed at one grammar, and descriptor bytes look nothing like a data file. fuzz_decode.cpp
// separately reaches this parser through the real ColumnPage path, which covers the DirectEncoding
// unwrap that a direct harness skips.
//
// Build with -DNANOLANCE_BUILD_FUZZERS=ON on Clang; run under -fsanitize=fuzzer,address,undefined.

#include "nanolance/page_layout.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pl = nano_lance::page_layout;

namespace {

/// Walk the whole tree so every owned pointer is dereferenced at least once -- ASan then catches a
/// node that was freed or never initialized, which a parse-and-discard harness would miss.
void touch(const pl::Compressive* node, int depth, std::size_t& seen) {
    if (node == nullptr) {
        return;
    }
    // The parser caps nesting; if that cap ever breaks, this recursion would too, so assert on it
    // rather than relying on a stack overflow to be reported usefully.
    assert(depth < 256);
    ++seen;
    (void)node->kind;
    (void)node->wire_field;
    (void)node->bits_per_value;
    (void)node->scheme;
    (void)node->wire_scheme;
    touch(node->values.get(), depth + 1, seen);
    touch(node->lengths.get(), depth + 1, seen);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::vector<std::uint8_t> bytes(data, data + size);

    pl::PageLayout layout;
    std::string error;
    const bool ok = pl::decode_page_layout(bytes, layout, error);

    // Contract 1: a refusal always carries a reason. A silent false would leave a caller reporting
    // nothing useful, which is the failure mode this whole parser exists to replace.
    if (!ok && error.empty()) {
        __builtin_trap();
    }

    // Contract 2: on failure the output is left in the reset state, so a caller that ignores the
    // return value cannot act on half-parsed garbage.
    if (!ok && layout.kind != pl::LayoutKind::kNone) {
        __builtin_trap();
    }

    if (ok) {
        std::size_t seen = 0;
        touch(layout.mini_block.value_compression.get(), 0, seen);
        touch(layout.mini_block.dictionary.get(), 0, seen);
        if (layout.constant.inline_value) {
            // Read every byte so a bad length in the descriptor shows up as a heap overflow here.
            volatile std::uint8_t sink = 0;
            for (const auto b : *layout.constant.inline_value) {
                sink = static_cast<std::uint8_t>(sink ^ b);
            }
            (void)sink;
        }
    }

    // describe() runs on both paths (it feeds error messages), so it must be total.
    const auto text = pl::describe(layout);
    if (text.empty()) {
        __builtin_trap();
    }

    // Contract 3: parsing is deterministic and free of cross-call state. A second run on the same
    // bytes must agree -- this catches a static/thread_local buffer being reused across pages.
    pl::PageLayout again;
    std::string error_again;
    const bool ok_again = pl::decode_page_layout(bytes, again, error_again);
    if (ok_again != ok || error_again != error || pl::describe(again) != text) {
        __builtin_trap();
    }

    return 0;
}
