// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// libFuzzer harness over the repetition/definition unraveler (src/repdef.cpp). Levels come straight
// from untrusted pages, and the unraveler's output sizes every list array built from them, so beyond
// "no crash" it asserts the structure any Arrow list needs on every input it ACCEPTS:
//
//   * one output layer per input layer;
//   * list offsets start at 0, never decrease, and end at exactly the child layer's length;
//   * a validity bitmap, when present, covers the layer, and null_count is within it;
//   * the innermost layer holds exactly num_items entries;
//   * a refusal says why.
//
// Input: [u8 n][n layer bytes][u8 flags: bit0 has_rep, bit1 has_def][u16 num_items]
//        then u16 pairs (rep, def) until the input ends.

#include "nanolance/repdef.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace rd = nano_lance::repdef;
    std::size_t at = 0;
    if (size < 1U) {
        return 0;
    }
    const std::size_t n = data[at++] % 8U;
    if (size < at + n + 3U) {
        return 0;
    }
    std::vector<std::uint8_t> layers(data + at, data + at + n);
    at += n;
    const auto flags = data[at++];
    const bool has_rep = (flags & 1U) != 0U;
    const bool has_def = (flags & 2U) != 0U;
    std::uint16_t num_items = 0;
    std::memcpy(&num_items, data + at, 2U);
    at += 2U;
    std::vector<std::uint16_t> rep;
    std::vector<std::uint16_t> def;
    while (at + 4U <= size) {
        std::uint16_t r = 0;
        std::uint16_t d = 0;
        std::memcpy(&r, data + at, 2U);
        std::memcpy(&d, data + at + 2U, 2U);
        at += 4U;
        // Small level values are the interesting ones; keep most of them in range.
        rep.push_back(static_cast<std::uint16_t>(r % 5U));
        def.push_back(static_cast<std::uint16_t>(d % 7U));
    }
    if (!has_rep) {
        rep.clear();
    }
    if (!has_def) {
        def.clear();
    }

    std::vector<rd::UnraveledLayer> out;
    std::string error;
    if (!rd::unravel(rep, has_rep, def, has_def, layers, num_items, out, error)) {
        assert(!error.empty() && "a refusal must say why");
        return 0;
    }
    assert(out.size() == layers.size());
    assert(out[0].length == num_items);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto& layer = out[i];
        assert(layer.validity.empty() || layer.validity.size() * 8U >= layer.length);
        assert(layer.null_count <= layer.length);
        assert(!layer.validity.empty() || layer.null_count == 0U);
        if (rd::is_list_layer(layer.kind)) {
            assert(layer.offsets.size() == layer.length + 1U);
            assert(layer.offsets.front() == 0);
            for (std::size_t k = 1; k < layer.offsets.size(); ++k) {
                assert(layer.offsets[k] >= layer.offsets[k - 1U]);
            }
            assert(static_cast<std::uint64_t>(layer.offsets.back()) == out[i - 1U].length);
        } else {
            assert(layer.offsets.empty());
            assert(i == 0U || layer.length == out[i - 1U].length);
        }
    }
    return 0;
}
