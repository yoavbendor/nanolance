// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The AVX-512 FSST batch kernel's interface (src/encoding/fsst_avx512.cpp). It includes nothing else
// on purpose: that translation unit is compiled with AVX-512 flags, so the code it emits for any
// shared header could be linked into a caller that runs on a CPU without AVX-512.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nano_lance::fsst {

/// One value to compress into `dst`, which must have room for 2 * size bytes. `padded` says that 7 bytes
/// past the value are readable (the portable compressors read them; the kernel never reads past a value).
struct Item {
    const std::uint8_t* data;
    std::size_t size;
    std::uint8_t* dst;
    bool padded;
    std::size_t written;  ///< set by the compressor: the bytes written at dst
};

/// A trained table in the kernel's layout.
struct BatchTable;

/// Lays out the table for the kernel: symbol `i` is code `i`, `lengths[i]` its length, `terminator` the byte
/// the kernel appends after each value. Null when the table cannot be laid out: a hash collision (nanom's
/// own table never has one) or an allocation failure. Release it with kernel_destroy_table.
BatchTable* kernel_build_table(const std::uint64_t* symbols, const std::uint8_t* lengths, unsigned count,
                               std::uint8_t terminator);
void kernel_destroy_table(const BatchTable* table);

/// Compresses every item with the kernel. Each value's output is the one compress_into gives it, byte for byte:
/// the kernel parses 511-byte jobs, and a job's codes are kept only while the parse cannot differ from the
/// whole value's (see the driver in fsst_avx512.cpp).
void kernel_compress(const BatchTable& table, Item* items, std::size_t count);

}  // namespace nano_lance::fsst
