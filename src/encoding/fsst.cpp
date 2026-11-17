// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// nanolance's calling convention over nanom's FSST (see include/nanolance/fsst.hpp).

#include "nanolance/fsst.hpp"

#include <cstdlib>
#include <cstring>
#include <span>

namespace nano_lance::fsst {

namespace codec = ::nanom::codec::fsst;

namespace {

std::span<const std::byte> as_bytes(const std::uint8_t* data, std::size_t size) {
    return {reinterpret_cast<const std::byte*>(data), size};
}

/// nanom's error, with what nanolance's messages have always said (the size expected, the symbol,
/// the code) filled in from the bytes it points at.
std::string describe_table_error(const ::nanom::codec::codec_error& e, const std::vector<std::uint8_t>& bytes) {
    const std::string what = e.what;
    if (what.find("size") != std::string::npos) {
        return "FSST symbol table is " + std::to_string(bytes.size()) + " bytes, expected " +
               std::to_string(kSymbolTableBytes);
    }
    if (what.find("magic") != std::string::npos) {
        return "FSST symbol table has the wrong magic";
    }
    const auto count = bytes.size() >= 8U ? static_cast<std::size_t>(bytes[0]) : 0U;
    const auto symbol = e.at - 8U - count * kMaxSymbolLength;
    return "FSST symbol " + std::to_string(symbol) + " declares length " + std::to_string(bytes[e.at]) +
           ", expected 1..8";
}

std::string describe_decode_error(const ::nanom::codec::codec_error& e, const SymbolTable& table,
                                  const std::uint8_t* data) {
    if (std::string(e.what).find("escape") != std::string::npos) {
        return "FSST escape at the end of a value has no payload byte";
    }
    return "FSST code " + std::to_string(data[e.at]) + " is not in the symbol table (" +
           std::to_string(table.symbol_count) + " symbols)";
}

}  // namespace

bool parse_symbol_table(const std::vector<std::uint8_t>& bytes, SymbolTable& out, std::string& error) {
    const auto parsed = codec::parse_symbol_table(as_bytes(bytes.data(), bytes.size()), out);
    if (!parsed) {
        error = describe_table_error(parsed.error(), bytes);
        return false;
    }
    return true;
}

bool decode_checked(const SymbolTable& table, const std::uint8_t* data, std::size_t size, std::uint8_t*& dst,
                    std::string& error) {
    auto* out = reinterpret_cast<std::byte*>(dst);
    ::nanom::codec::codec_error why;
    if (!codec::decode_unchecked(table, as_bytes(data, size), out, &why)) {
        error = describe_decode_error(why, table, data);
        return false;
    }
    dst = reinterpret_cast<std::uint8_t*>(out);
    return true;
}

bool decompress_value(const SymbolTable& table, const std::uint8_t* data, std::size_t size,
                      std::vector<std::uint8_t>& out, std::string& error) {
    const auto at = out.size();
    out.resize(at + codec::max_decoded_size(size));
    auto* dst = out.data() + at;
    if (!decode_checked(table, data, size, dst, error)) {
        out.resize(at);
        return false;
    }
    out.resize(static_cast<std::size_t>(dst - out.data()));
    return true;
}

#if defined(NANOLANCE_FSST_AVX512)
namespace {

/// The first byte no multi-byte symbol contains, or -1. The kernel appends such a byte after each value: a symbol
/// holding it could match past the value's end. cwida's encoder picks its terminator the same way.
int pick_terminator(const Encoder& encoder) {
    bool inside[256] = {};
    for (std::uint32_t i = 0; i < encoder.symbol_count; ++i) {
        if (encoder.lengths[i] < 2) {
            continue;
        }
        for (unsigned k = 0; k < encoder.lengths[i]; ++k) {
            inside[(encoder.symbols[i] >> (8U * k)) & 0xFFU] = true;
        }
    }
    for (int b = 0; b < 256; ++b) {
        if (!inside[b]) {
            return b;
        }
    }
    return -1;
}

}  // namespace
#endif

void attach_batch_table(Encoder& encoder) {
    encoder.batch.reset();
#if defined(NANOLANCE_FSST_AVX512)
    const int terminator = pick_terminator(encoder);
    if (terminator < 0 || encoder.symbol_count == 0) {
        return;  // no kernel for this table: the portable compressors serve it
    }
    if (const BatchTable* table = kernel_build_table(encoder.symbols.data(), encoder.lengths.data(),
                                                     encoder.symbol_count, static_cast<std::uint8_t>(terminator))) {
        encoder.batch = std::shared_ptr<const BatchTable>(table, kernel_destroy_table);
    }
#endif
}

bool train(const std::vector<std::pair<const std::uint8_t*, std::size_t>>& values, Encoder& out) {
    const bool trained = codec::train(
        values.size(), [&](std::size_t i) { return as_bytes(values[i].first, values[i].second); }, out);
    attach_batch_table(out);
    return trained;
}

void compress_value(const Encoder& encoder, const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    const auto at = out.size();
    out.resize(at + codec::max_compressed_size(size));
    out.resize(at + compress_into(encoder, data, size, out.data() + at));
}

std::vector<std::uint8_t> serialize(const Encoder& encoder) {
    const auto table = codec::serialize(encoder);
    const auto* p = reinterpret_cast<const std::uint8_t*>(table.data());
    return {p, p + table.size()};
}

void compress_many_portable(const Encoder& encoder, Item* items, std::size_t count) {
    std::size_t i = 0;
    while (i < count) {
        const bool four = count - i >= 4U && items[i].padded && items[i + 1U].padded && items[i + 2U].padded &&
                          items[i + 3U].padded;
        if (four) {
            const std::uint8_t* data[4];
            std::size_t sizes[4];
            std::uint8_t* to[4];
            std::size_t written[4];
            for (std::size_t k = 0; k < 4U; ++k) {
                data[k] = items[i + k].data;
                sizes[k] = items[i + k].size;
                to[k] = items[i + k].dst;
            }
            compress_into_padded4(encoder, data, sizes, to, written);
            for (std::size_t k = 0; k < 4U; ++k) {
                items[i + k].written = written[k];
            }
            i += 4U;
            continue;
        }
        Item& item = items[i];
        item.written = item.padded ? compress_into_padded(encoder, item.data, item.size, item.dst)
                                   : compress_into(encoder, item.data, item.size, item.dst);
        ++i;
    }
}

#if defined(NANOLANCE_FSST_AVX512)
namespace {

/// The kernel runs on this CPU and when NANOLANCE_FSST_SIMD is not "0" (read once per process).
bool kernel_usable() {
    static const bool usable = [] {
        const char* off = std::getenv("NANOLANCE_FSST_SIMD");
        if (off != nullptr && std::strcmp(off, "0") == 0) {
            return false;
        }
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq") &&
               __builtin_cpu_supports("popcnt");
    }();
    return usable;
}

}  // namespace

bool compress_many_avx512(const Encoder& encoder, Item* items, std::size_t count) {
    if (encoder.batch == nullptr || !kernel_usable()) {
        return false;
    }
    kernel_compress(*encoder.batch, items, count);
    return true;
}
#else
bool compress_many_avx512(const Encoder&, Item*, std::size_t) {
    return false;
}
#endif

void compress_many(const Encoder& encoder, Item* items, std::size_t count) {
    // The kernel pays from a mean of about 128 bytes a value: below that its per-value work (staging, the restart
    // walk) costs more than the lanes save (measured on this machine: -5% at 64 bytes, +13% at 128, +25% at 256).
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < count; ++i) {
        bytes += items[i].size;
    }
    constexpr std::size_t kKernelMinMeanBytes = 128;
    if (count == 0 || bytes < kKernelMinMeanBytes * count || !compress_many_avx512(encoder, items, count)) {
        compress_many_portable(encoder, items, count);
    }
}

}  // namespace nano_lance::fsst
