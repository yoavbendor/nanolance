// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// encoding/lance_descriptors: the page descriptors the writer builds, parsed back by the reader's
// page_layout parse. Every CompressiveEncoding builder (flat, variable, out-of-line and inline
// bitpacking, fsst, rle, byte-stream split, general, fixed-size list with and without validity) as
// one tree composed bottom-up; the three layouts (MiniBlock, Constant with and without an inline value,
// FullZip) with each of their fields; the type urls of the page and column wrappers; and the
// canonical-protobuf property that zero fields are left out (an empty Flat(0) is two bytes).

#include "encoding/lance_descriptors.hpp"
#include "nanolance/page_layout.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

namespace d = nano_lance::descriptor;
namespace pl = nano_lance::page_layout;
using Bytes = std::vector<std::uint8_t>;

/// A CompressiveEncoding's bytes as a MiniBlock's value compression, parsed back to its tree.
pl::PageLayout parse(const Bytes& page_encoding) {
    pl::PageLayout layout;
    std::string error;
    require(pl::decode_page_layout(page_encoding, layout, error), "parse: " + error);
    return layout;
}

std::unique_ptr<pl::Compressive> parse_value(const Bytes& encoding) {
    d::MiniBlock m;
    m.value_compression = encoding;
    m.num_buffers = 1;
    auto layout = parse(d::page_encoding(m));
    require(layout.kind == pl::LayoutKind::kMiniBlock, "a MiniBlock page");
    require(layout.mini_block.value_compression != nullptr, "value compression present");
    return std::move(layout.mini_block.value_compression);
}

void test_compressive_builders() {
    {
        const auto n = parse_value(d::flat(64));
        require(n->kind == pl::CompressiveKind::kFlat && n->wire_field == 1U && n->bits_per_value == 64U, "flat(64)");
    }
    {
        const auto n = parse_value(d::variable(d::flat(32)));
        require(n->kind == pl::CompressiveKind::kVariable && n->values != nullptr &&
                    n->values->kind == pl::CompressiveKind::kFlat && n->values->bits_per_value == 32U,
                "variable(flat(32))");
    }
    {
        const auto n = parse_value(d::out_of_line_bitpacking(16, d::flat(1)));
        require(n->kind == pl::CompressiveKind::kBitpacked && n->wire_field == 4U && n->bits_per_value == 16U &&
                    n->values != nullptr && n->values->bits_per_value == 1U,
                "out_of_line_bitpacking(16, flat(1))");
    }
    {
        const auto n = parse_value(d::inline_bitpacking(32));
        require(n->kind == pl::CompressiveKind::kInlineBitpacking && n->wire_field == 5U && n->bits_per_value == 32U,
                "inline_bitpacking(32)");
    }
    {
        const Bytes table = {1, 2, 3, 4, 5};
        const auto n = parse_value(d::fsst(table, d::variable(d::flat(32))));
        require(n->kind == pl::CompressiveKind::kFsst && n->symbol_table == table && n->values != nullptr &&
                    n->values->kind == pl::CompressiveKind::kVariable,
                "fsst keeps its symbol table and values");
    }
    {
        const auto n = parse_value(d::rle(d::flat(64), d::flat(8)));
        require(n->kind == pl::CompressiveKind::kRle && n->values != nullptr && n->lengths != nullptr &&
                    n->values->bits_per_value == 64U && n->lengths->bits_per_value == 8U,
                "rle(flat(64), flat(8))");
    }
    {
        // The tree the header's example composes.
        const auto n = parse_value(d::general(d::kZstd, d::byte_stream_split(d::flat(64))));
        require(n->kind == pl::CompressiveKind::kGeneral && n->scheme == pl::BufferScheme::kZstd &&
                    n->values != nullptr && n->values->kind == pl::CompressiveKind::kByteStreamSplit &&
                    n->values->values != nullptr && n->values->values->bits_per_value == 64U,
                "general(zstd, bss(flat(64)))");
        const auto lz4 = parse_value(d::general(d::kLz4, d::flat(8)));
        require(lz4->scheme == pl::BufferScheme::kLz4 && lz4->wire_scheme == 1U, "general(lz4)");
        const auto none = parse_value(d::general(d::kNone, d::flat(8)));
        require(none->scheme == pl::BufferScheme::kNone && none->wire_scheme == 0U, "general(none)");
    }
    {
        const auto n = parse_value(d::fixed_size_list(8, d::flat(32)));
        require(n->kind == pl::CompressiveKind::kFixedSizeList && n->items_per_value == 8U && !n->has_validity &&
                    n->values != nullptr && n->values->bits_per_value == 32U,
                "fixed_size_list(8, flat(32))");
        const auto v = parse_value(d::fixed_size_list(4, d::flat(32), true));
        require(v->has_validity && v->items_per_value == 4U, "fixed_size_list with validity");
    }
    {
        // Constant (field 3) is read from the unknown fields: with a value, its bytes; without, none.
        const auto c = parse_value(d::constant({0x01, 0x00, 0x00, 0x00}));
        require(c->kind == pl::CompressiveKind::kConstant && c->wire_field == 3U && c->has_constant_value &&
                    c->constant_value == Bytes({0x01, 0x00, 0x00, 0x00}),
                "constant with a value");
        const auto n = parse_value(d::constant({}, false));
        require(n->kind == pl::CompressiveKind::kConstant && !n->has_constant_value && n->constant_value.empty(),
                "constant without a value");
        // Inside a FixedSizeList, as an all-null list's items are written.
        const auto f = parse_value(d::fixed_size_list(2, d::constant({}, false), true));
        require(f->kind == pl::CompressiveKind::kFixedSizeList && f->values != nullptr &&
                    f->values->kind == pl::CompressiveKind::kConstant && !f->values->has_constant_value,
                "fixed_size_list of a constant without a value");
    }
    // Canonical protobuf: Flat(0) is CompressiveEncoding{ f1 Flat{} } -- the zero is left out.
    require(d::flat(0) == Bytes({0x0a, 0x00}), "a zero field is not written");
    require(d::flat(1) == Bytes({0x0a, 0x02, 0x08, 0x01}), "Flat(1) as Lance writes it");
}

void test_mini_block_layout() {
    d::MiniBlock m;
    m.rep_compression = d::out_of_line_bitpacking(16, d::flat(2));
    m.def_compression = d::out_of_line_bitpacking(16, d::flat(1));
    m.value_compression = d::flat(32);
    m.dictionary = d::variable(d::flat(32));
    m.num_dictionary_items = 17;
    m.layers = {3, 4};
    m.num_buffers = 1;
    m.repetition_index_depth = 1;
    m.num_items = 1000;
    m.has_large_chunk = true;
    const auto bytes = d::page_encoding(m);
    const std::string url = "/lance.encodings21.PageLayout";
    require(bytes.size() > 2U + url.size() && bytes[0] == 0x0a && bytes[1] == url.size() &&
                std::string(bytes.begin() + 2, bytes.begin() + 2 + static_cast<std::ptrdiff_t>(url.size())) == url,
            "the page encoding is wrapped with the PageLayout type url");
    const auto layout = parse(bytes);
    require(layout.kind == pl::LayoutKind::kMiniBlock, "MiniBlock kind");
    const auto& mb = layout.mini_block;
    require(mb.has_repetition && mb.rep_compression != nullptr && mb.rep_compression->values->bits_per_value == 2U,
            "rep compression");
    require(mb.repdef_compression != nullptr && mb.repdef_compression->values->bits_per_value == 1U, "def compression");
    require(mb.value_compression != nullptr && mb.value_compression->bits_per_value == 32U, "value compression");
    require(mb.dictionary != nullptr && mb.dictionary->kind == pl::CompressiveKind::kVariable, "dictionary");
    require(mb.num_dictionary_items == 17U && mb.num_buffers == 1U && mb.repetition_index_depth == 1U &&
                mb.num_items == 1000U && mb.has_large_chunk,
            "MiniBlock scalars");
    require(mb.layers == Bytes({3, 4}) && pl::layers_have_definition_levels(mb.layers), "layers");

    d::MiniBlock plain;
    plain.value_compression = d::flat(64);
    plain.num_buffers = 1;
    plain.num_items = 5;
    const auto p = parse(d::page_encoding(plain));
    require(!p.mini_block.has_repetition && p.mini_block.repdef_compression == nullptr &&
                p.mini_block.dictionary == nullptr && !p.mini_block.has_large_chunk,
            "absent encodings stay absent");
}

void test_constant_layout() {
    const Bytes value = {7, 0, 0, 0, 0, 0, 0, 0};
    d::Constant c;
    c.layers = {1};
    c.inline_value = &value;
    auto layout = parse(d::page_encoding(c));
    require(layout.kind == pl::LayoutKind::kConstant, "Constant kind");
    require(layout.constant.inline_value.has_value() && *layout.constant.inline_value == value, "inline value");
    require(layout.constant.layers == Bytes({1}), "constant layers");

    d::Constant all_null;
    all_null.layers = {3};
    all_null.def_compression = d::out_of_line_bitpacking(16, d::flat(1));
    all_null.num_def_values = 9;
    layout = parse(d::page_encoding(all_null));
    require(!layout.constant.inline_value.has_value(), "an all-null constant has no inline value");
    require(layout.constant.def_compression != nullptr && layout.constant.rep_compression == nullptr &&
                layout.constant.num_def_values == 9U && layout.constant.num_rep_values == 0U,
            "constant level compression and counts");
}

void test_full_zip_layout() {
    d::FullZip f;
    f.bits_rep = 2;
    f.bits_def = 1;
    f.bits_per_offset = 32;
    f.num_items = 40;
    f.num_visible_items = 38;
    f.value_compression = d::general(d::kZstd, d::variable(d::flat(32)));
    f.layers = {3, 4};
    const auto layout = parse(d::page_encoding(f));
    require(layout.kind == pl::LayoutKind::kFullZip, "FullZip kind");
    const auto& z = layout.full_zip;
    require(z.bits_rep == 2U && z.bits_def == 1U && z.bits_per_offset == 32U && z.bits_per_value == 0U &&
                z.num_items == 40U && z.num_visible_items == 38U && z.layers == Bytes({3, 4}),
            "FullZip scalars");
    require(z.value_compression != nullptr && z.value_compression->kind == pl::CompressiveKind::kGeneral,
            "FullZip value compression");

    d::FullZip fixed;
    fixed.bits_per_value = 512;
    fixed.num_items = 3;
    fixed.num_visible_items = 3;
    fixed.value_compression = d::flat(512);
    const auto fz = parse(d::page_encoding(fixed));
    require(fz.full_zip.bits_per_value == 512U && fz.full_zip.bits_per_offset == 0U, "fixed-width FullZip");
}

void test_column_encoding() {
    const auto bytes = d::column_encoding();
    const std::string url = "/lance.encodings.ColumnEncoding";
    require(bytes.size() == 2U + url.size() + 2U + 2U, "ColumnEncoding wrapper size");
    require(bytes[0] == 0x0a && bytes[1] == url.size() &&
                std::string(bytes.begin() + 2, bytes.begin() + 2 + static_cast<std::ptrdiff_t>(url.size())) == url,
            "ColumnEncoding type url");
    const Bytes tail(bytes.end() - 4, bytes.end());
    require(tail == Bytes({0x12, 0x02, 0x0a, 0x00}), "ColumnEncoding{ values = {} }");
}

}  // namespace

int main() {
    test_compressive_builders();
    test_mini_block_layout();
    test_constant_layout();
    test_full_zip_layout();
    test_column_encoding();
    std::cout << "lance_descriptors: ok\n";
    return 0;
}
