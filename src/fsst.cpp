// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// nanolance's calling convention over nanom's FSST (see include/nanolance/fsst.hpp).

#include "nanolance/fsst.hpp"

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

bool train(const std::vector<std::pair<const std::uint8_t*, std::size_t>>& values, Encoder& out) {
    return codec::train(values.size(), [&](std::size_t i) { return as_bytes(values[i].first, values[i].second); },
                        out);
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

}  // namespace nano_lance::fsst
