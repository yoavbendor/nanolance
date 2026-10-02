// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Building Lance page descriptors -- the /lance.encodings21.PageLayout a writer puts on every page --
// from nanom's model of them (nanom/formats/lance_encodings.hpp) instead of by hand.
//
// Each CompressiveEncoding builder returns that encoding's bytes, so trees compose bottom-up:
//
//     general(kZstd, byte_stream_split(flat(64)))
//
// and a layout takes its encodings as such bytes. nanom computes every length prefix and varint,
// and writes canonical protobuf (fields in number order, zeros left out) -- what Lance writes.
// Internal to the writer; not installed.
#pragma once

#include <nanom/formats/lance_encodings.hpp>
#include <nanom/protobuf_encode.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace nano_lance::descriptor {

using Bytes = std::vector<std::uint8_t>;
namespace wire = ::nanom_formats::lance;

/// CompressionScheme in General's BufferCompression.
inline constexpr std::uint64_t kNone = 0;
inline constexpr std::uint64_t kLz4 = 1;
inline constexpr std::uint64_t kZstd = 2;

namespace detail {

struct u8_sink {
    Bytes* out;
    bool put(const std::byte* p, std::size_t n) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(p);
        out->insert(out->end(), b, b + n);
        return true;
    }
};

template <class M>
Bytes encode(const M& m) {
    Bytes out;
    u8_sink sink{&out};
    (void)::nanom::protobuf_encode(m, sink);  // a descriptor is far below every protobuf limit
    return out;
}

inline ::nanom::bytes view(const Bytes& b) {
    return ::nanom::bytes(reinterpret_cast<const std::byte*>(b.data()), b.size());
}

/// An encoding given as its bytes, written back as they are. Empty means absent.
inline ::nanom::pb_lazy<wire::CompressiveEncoding> lazy(const Bytes& encoding) {
    ::nanom::pb_lazy<wire::CompressiveEncoding> l;
    if (!encoding.empty()) {
        l.set_region(reinterpret_cast<const std::byte*>(encoding.data()), encoding.size());
    }
    return l;
}

template <class Variant, class Member>
Bytes compressive(Member wire::CompressiveEncoding::*member, const Variant& v) {
    wire::CompressiveEncoding e;
    (e.*member).v = v;
    return encode(e);
}

}  // namespace detail

// --- CompressiveEncoding ----------------------------------------------------------------------

/// Flat{ bits_per_value }.
inline Bytes flat(std::uint64_t bits) {
    wire::Flat v;
    v.bits_per_value = bits;
    return detail::compressive(&wire::CompressiveEncoding::flat, v);
}
/// Variable{ offsets }.
inline Bytes variable(const Bytes& offsets) {
    wire::Variable v;
    v.offsets = detail::lazy(offsets);
    return detail::compressive(&wire::CompressiveEncoding::variable, v);
}
/// OutOfLineBitpacking{ uncompressed_bits_per_value, values }: how Lance stores levels.
inline Bytes out_of_line_bitpacking(std::uint64_t uncompressed_bits, const Bytes& values) {
    wire::OutOfLineBitpacking v;
    v.uncompressed_bits_per_value = uncompressed_bits;
    v.values = detail::lazy(values);
    return detail::compressive(&wire::CompressiveEncoding::out_of_line_bitpacking, v);
}
/// InlineBitpacking{ uncompressed_bits_per_value }.
inline Bytes inline_bitpacking(std::uint64_t uncompressed_bits) {
    wire::InlineBitpacking v;
    v.uncompressed_bits_per_value = uncompressed_bits;
    return detail::compressive(&wire::CompressiveEncoding::inline_bitpacking, v);
}
/// Fsst{ symbol_table, values }.
inline Bytes fsst(const Bytes& symbol_table, const Bytes& values) {
    wire::Fsst v;
    v.symbol_table = detail::view(symbol_table);
    v.values = detail::lazy(values);
    return detail::compressive(&wire::CompressiveEncoding::fsst, v);
}
/// Rle{ values, run_lengths }.
inline Bytes rle(const Bytes& values, const Bytes& run_lengths) {
    wire::Rle v;
    v.values = detail::lazy(values);
    v.run_lengths = detail::lazy(run_lengths);
    return detail::compressive(&wire::CompressiveEncoding::rle, v);
}
/// ByteStreamSplit{ values }.
inline Bytes byte_stream_split(const Bytes& values) {
    wire::ByteStreamSplit v;
    v.values = detail::lazy(values);
    return detail::compressive(&wire::CompressiveEncoding::byte_stream_split, v);
}
/// General{ BufferCompression{ scheme }, values }.
inline Bytes general(std::uint64_t scheme, const Bytes& values) {
    wire::General v;
    v.compression = wire::BufferCompression{};
    v.compression->value().scheme = scheme;
    v.values = detail::lazy(values);
    return detail::compressive(&wire::CompressiveEncoding::general, v);
}
/// FixedSizeList{ items_per_value, values }.
inline Bytes fixed_size_list(std::uint64_t items_per_value, const Bytes& values) {
    wire::FixedSizeList21 v;
    v.items_per_value = items_per_value;
    v.values = detail::lazy(values);
    return detail::compressive(&wire::CompressiveEncoding::fixed_size_list, v);
}

// --- layouts ----------------------------------------------------------------------------------

/// MiniBlockLayout. Encodings are given as their bytes; empty means absent.
struct MiniBlock {
    Bytes rep_compression;
    Bytes def_compression;
    Bytes value_compression;
    Bytes dictionary;
    std::uint64_t num_dictionary_items = 0;
    Bytes layers;  ///< RepDefLayer values, one byte each
    std::uint64_t num_buffers = 0;
    std::uint64_t repetition_index_depth = 0;
    std::uint64_t num_items = 0;
    bool has_large_chunk = false;
};

/// ConstantLayout.
struct Constant {
    Bytes layers;
    const Bytes* inline_value = nullptr;  ///< the value, when stored in the descriptor
    Bytes rep_compression;
    Bytes def_compression;
    std::uint64_t num_rep_values = 0;
    std::uint64_t num_def_values = 0;
};

/// FullZipLayout.
struct FullZip {
    std::uint64_t bits_rep = 0;
    std::uint64_t bits_def = 0;
    std::uint64_t bits_per_value = 0;
    std::uint64_t bits_per_offset = 0;
    std::uint64_t num_items = 0;
    std::uint64_t num_visible_items = 0;
    Bytes value_compression;
    Bytes layers;
};

namespace detail {

/// ColumnPage.encoding: the PageLayout in its type-url wrapper.
inline Bytes page_encoding(const wire::PageLayout& layout) {
    const auto payload = encode(layout);
    wire::EncodingAny any;
    any.type_url = std::string_view("/lance.encodings21.PageLayout");
    any.value = view(payload);
    return encode(any);
}

}  // namespace detail

inline Bytes page_encoding(const MiniBlock& m) {
    wire::MiniBlockLayout w;
    w.rep_compression = detail::lazy(m.rep_compression);
    w.def_compression = detail::lazy(m.def_compression);
    w.value_compression = detail::lazy(m.value_compression);
    w.dictionary = detail::lazy(m.dictionary);
    w.num_dictionary_items = m.num_dictionary_items;
    w.layers = detail::view(m.layers);
    w.num_buffers = m.num_buffers;
    w.repetition_index_depth = m.repetition_index_depth;
    w.num_items = m.num_items;
    w.has_large_chunk = m.has_large_chunk ? 1U : 0U;
    wire::PageLayout layout;
    layout.mini_block_layout = w;
    return detail::page_encoding(layout);
}

inline Bytes page_encoding(const Constant& c) {
    wire::ConstantLayout w;
    w.layers = detail::view(c.layers);
    if (c.inline_value != nullptr) {
        w.inline_value = detail::view(*c.inline_value);
    }
    w.rep_compression = detail::lazy(c.rep_compression);
    w.def_compression = detail::lazy(c.def_compression);
    w.num_rep_values = c.num_rep_values;
    w.num_def_values = c.num_def_values;
    wire::PageLayout layout;
    layout.constant_layout = w;
    return detail::page_encoding(layout);
}

inline Bytes page_encoding(const FullZip& f) {
    wire::FullZipLayout w;
    w.bits_rep = f.bits_rep;
    w.bits_def = f.bits_def;
    w.bits_per_value = f.bits_per_value;
    w.bits_per_offset = f.bits_per_offset;
    w.num_items = f.num_items;
    w.num_visible_items = f.num_visible_items;
    w.value_compression = detail::lazy(f.value_compression);
    w.layers = detail::view(f.layers);
    wire::PageLayout layout;
    layout.full_zip_layout = w;
    return detail::page_encoding(layout);
}

/// A format 2.x column's own encoding: ColumnEncoding{ values = {} } in its type-url wrapper.
inline Bytes column_encoding() {
    const Bytes values{0x0a, 0x00};  // ColumnEncoding{ f1 values = Empty }
    wire::EncodingAny any;
    any.type_url = std::string_view("/lance.encodings.ColumnEncoding");
    any.value = detail::view(values);
    return detail::encode(any);
}

}  // namespace nano_lance::descriptor
