// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Byte-stream-split transform for fixed-width columns (float/double), shared by the writer
// (data_file_writer.cpp) and reader (lance_column_decoder.cpp). Splitting `count` `vlen`-byte
// little-endian values into `vlen` contiguous byte-planes (plane b holds byte b of every value, in
// order) groups same-significance bytes together — mantissa bytes next to mantissa bytes, exponent
// bytes next to exponent bytes — which is what lets the zstd frame that follows compress meaningfully
// better than compressing the raw interleaved values. This is stock Lance's own `ByteStreamSplit`
// encoding (restricted there to 32/64-bit values); the wire format was verified byte-for-byte against
// a real `lance` 8.0.0-written file (see the writer-side page_layout_bytes_bss_zstd for the protobuf
// shape this pairs with).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nano_lance::bss {

// Split `count` `vlen`-byte values at `data` into `vlen` contiguous byte-planes, into `out`. Every
// byte of `out` is unconditionally overwritten, so a caller reusing `out` across equal-sized chunks
// pays no re-zeroing at all after the first call (resize() to the same or a smaller size touches
// nothing) -- a fresh std::vector per chunk instead zero-fills the whole buffer and then immediately
// throws that work away (measured at ~44% of a float-column write's instructions).
inline void transpose(const std::uint8_t* data, std::size_t vlen, std::size_t count,
                      std::vector<std::uint8_t>& out) {
    out.resize(vlen * count);
    for (std::size_t b = 0; b < vlen; ++b) {
        std::uint8_t* plane = out.data() + b * count;
        for (std::size_t i = 0; i < count; ++i) {
            plane[i] = data[i * vlen + b];
        }
    }
}

// Convenience overload returning a fresh buffer (one-shot callers / tests).
inline std::vector<std::uint8_t> transpose(const std::uint8_t* data, std::size_t vlen, std::size_t count) {
    std::vector<std::uint8_t> out;
    transpose(data, vlen, count, out);
    return out;
}

// Inverse of transpose(): reassemble `count` `vlen`-byte values from `vlen` contiguous byte-planes at
// `planes` into `out` (caller-owned, must hold vlen*count bytes).
inline void untranspose(const std::uint8_t* planes, std::size_t vlen, std::size_t count, std::uint8_t* out) {
    for (std::size_t b = 0; b < vlen; ++b) {
        const std::uint8_t* plane = planes + b * count;
        for (std::size_t i = 0; i < count; ++i) {
            out[i * vlen + b] = plane[i];
        }
    }
}

}  // namespace nano_lance::bss
