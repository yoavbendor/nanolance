// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The sparse structural layout (PageLayout field 5) and the child blocks it nests, without Rust:
//   - the descriptor parses (a hand-built SparseLayout) and its value descriptor reads back as a mini-block;
//   - unravel turns hand-built layers into the per-layer offsets and validity repdef::unravel gives: a
//     nullable leaf with both null polarities, a list with empty and null slots, a list of lists, a nullable
//     struct over a non-null leaf, and the refusals (a null slot with children, counts that do not add up,
//     an item count that does not match, a buffer count that does not match);
//   - chunk_items reads the value chunks' 8-byte metadata and refuses a count or size that does not add up;
//   - child_block decodes flat, inline-bitpacked, bitpacked and general blocks, and RLE chunks whose run
//     values and run lengths have each their own width and encoding.

#include "nanolance/child_block.hpp"
#include "nanolance/page_layout.hpp"
#include "nanolance/repdef.hpp"
#include "nanolance/sparse_layout.hpp"
#include "encoding/values/bitpack.hpp"
#include "encoding/values/general.hpp"

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

using Bytes = std::vector<std::uint8_t>;
using namespace nano_lance;
using page_layout::Compressive;
using page_layout::CompressiveKind;
using page_layout::SparseLayer;
using page_layout::SparseLayout;
using page_layout::SparsePositions;

Compressive flat(std::uint32_t bits) {
    Compressive c;
    c.kind = CompressiveKind::kFlat;
    c.bits_per_value = bits;
    return c;
}

/// `values` as little-endian u64 bytes.
Bytes u64_bytes(const std::vector<std::uint64_t>& values) {
    Bytes out;
    for (const auto v : values) {
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    return out;
}

/// The delta-coded explicit positions of `positions` (first absolute, the rest steps), as a u64 buffer.
Bytes positions_buffer(const std::vector<std::uint64_t>& positions) {
    std::vector<std::uint64_t> deltas;
    std::uint64_t previous = 0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        deltas.push_back(i == 0 ? positions[i] : positions[i] - previous);
        previous = positions[i];
    }
    return u64_bytes(deltas);
}

SparsePositions explicit_positions(std::uint64_t count) {
    SparsePositions p;
    p.kind = SparsePositions::Kind::kExplicit;
    p.num_positions = count;
    p.encoding = std::make_unique<Compressive>(flat(64));
    return p;
}

SparsePositions empty_positions() {
    SparsePositions p;
    p.kind = SparsePositions::Kind::kEmpty;
    return p;
}

SparseLayer validity_layer(std::uint64_t slots, bool null_positions, SparsePositions positions) {
    SparseLayer l;
    l.kind = SparseLayer::Kind::kValidity;
    l.num_slots = slots;
    l.validity.null_positions = null_positions;
    l.validity.positions = std::move(positions);
    return l;
}

SparseLayer list_layer(std::uint64_t slots, std::uint64_t child_slots, SparsePositions non_empty,
                       std::uint64_t constant_count, SparsePositions validity_positions, bool null_positions,
                       bool explicit_counts, std::uint64_t num_counts) {
    SparseLayer l;
    l.kind = SparseLayer::Kind::kList;
    l.num_slots = slots;
    l.num_child_slots = child_slots;
    l.non_empty = std::move(non_empty);
    if (explicit_counts) {
        l.counts.kind = page_layout::SparseCounts::Kind::kExplicit;
        l.counts.encoding = std::make_unique<Compressive>(flat(64));
        (void)num_counts;
    } else {
        l.counts.kind = page_layout::SparseCounts::Kind::kConstant;
        l.counts.value = constant_count;
    }
    l.validity.null_positions = null_positions;
    l.validity.positions = std::move(validity_positions);
    return l;
}

SparseLayout layout(std::uint64_t num_items, std::uint64_t visible, std::vector<SparseLayer> layers) {
    SparseLayout l;
    l.num_items = num_items;
    l.num_visible_items = visible;
    l.num_buffers = 1;
    l.layers = std::move(layers);
    return l;
}

// --- protobuf writing for hand-built descriptors ----------------------------------------------------

void put_varint(Bytes& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}
void put_varint_field(Bytes& out, std::uint32_t field, std::uint64_t v) {
    put_varint(out, (static_cast<std::uint64_t>(field) << 3U));
    put_varint(out, v);
}
void put_bytes_field(Bytes& out, std::uint32_t field, const Bytes& bytes) {
    put_varint(out, (static_cast<std::uint64_t>(field) << 3U) | 2U);
    put_varint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

Bytes flat_encoding_bytes(std::uint64_t bits) {
    Bytes flat_msg;
    put_varint_field(flat_msg, 1, bits);
    Bytes enc;
    put_bytes_field(enc, 1, flat_msg);  // CompressiveEncoding field 1: Flat
    return enc;
}

/// A complete descriptor for a sparse page with one nullable leaf: EncodingAny{PageLayout{SparseLayout}}.
Bytes sparse_descriptor(std::uint64_t rows, bool has_validity) {
    Bytes structural;
    if (has_validity) {
        // SparseValidityLayer { num_slots = 1, validity = 2 { meaning = 1 (null positions), positions = 2 {} } }
        Bytes validity_set;
        put_varint_field(validity_set, 1, 1);
        Bytes positions;
        put_bytes_field(positions, 4, Bytes{});  // SparsePositionSet.empty
        put_bytes_field(validity_set, 2, positions);
        Bytes validity_layer;
        put_varint_field(validity_layer, 1, rows);
        put_bytes_field(validity_layer, 2, validity_set);
        Bytes layer;
        put_bytes_field(layer, 1, validity_layer);  // SparseStructuralLayer.validity
        put_bytes_field(structural, 6, layer);
    }
    Bytes sparse;
    put_bytes_field(sparse, 1, flat_encoding_bytes(32));
    put_varint_field(sparse, 2, 1);      // num_buffers
    put_varint_field(sparse, 3, rows);   // num_items
    put_varint_field(sparse, 4, rows);   // num_visible_items
    put_varint_field(sparse, 5, 1);      // has_large_chunk
    sparse.insert(sparse.end(), structural.begin(), structural.end());
    Bytes page;
    put_bytes_field(page, 5, sparse);  // PageLayout.sparse_layout
    Bytes any;
    const std::string url = "type.googleapis.com/lance.encodings21.PageLayout";
    put_varint(any, (1U << 3U) | 2U);
    put_varint(any, url.size());
    any.insert(any.end(), url.begin(), url.end());
    put_bytes_field(any, 2, page);
    return any;
}

// --- tests -----------------------------------------------------------------------------------------

void test_descriptor_parses_as_sparse() {
    page_layout::PageLayout out;
    std::string error;
    const auto bytes = sparse_descriptor(6, true);
    require(page_layout::decode_page_layout(bytes, out, error), "sparse descriptor parses: " + error);
    require(out.kind == page_layout::LayoutKind::kSparse, "descriptor is a sparse layout");
    require(out.sparse.num_visible_items == 6 && out.sparse.num_items == 6, "sparse counts");
    require(out.sparse.layers.size() == 1 && out.sparse.layers[0].kind == SparseLayer::Kind::kValidity,
            "one validity layer");
    require(out.sparse.layers[0].validity.null_positions, "null-positions meaning");
    require(out.sparse.layers[0].validity.positions.kind == SparsePositions::Kind::kEmpty, "empty positions");
    require(out.sparse.value_compression != nullptr && out.sparse.value_compression->kind == CompressiveKind::kFlat,
            "value compression is Flat");
    require(out.sparse.value_compression->bits_per_value == 32, "value width");
    require(page_layout::describe(out).find("Sparse{") == 0U, "describe names the sparse layout");
}

void test_value_descriptor_reads_back_as_mini_block() {
    page_layout::PageLayout sparse_page;
    std::string error;
    require(page_layout::decode_page_layout(sparse_descriptor(4, false), sparse_page, error),
            "descriptor parses: " + error);
    const auto value_descriptor = sparse::value_descriptor(sparse_page.sparse, 4);
    page_layout::PageLayout mini;
    require(page_layout::decode_page_layout(value_descriptor, mini, error), "value descriptor parses: " + error);
    require(mini.kind == page_layout::LayoutKind::kMiniBlock, "value descriptor is a mini-block");
    require(mini.mini_block.value_compression != nullptr &&
                mini.mini_block.value_compression->kind == CompressiveKind::kFlat &&
                mini.mini_block.value_compression->bits_per_value == 32,
            "value descriptor carries the value encoding");
    require(mini.mini_block.num_buffers == 1 && mini.mini_block.num_items == 4, "value descriptor counts");
    require(mini.mini_block.has_large_chunk, "value descriptor keeps the u32 chunk grammar");
    require(!mini.mini_block.has_repetition && mini.mini_block.repdef_compression == nullptr,
            "value descriptor has no levels");
}

void test_flat_nullable_leaf_both_polarities() {
    // Six rows, nulls at 1 and 4, given as null positions.
    {
        std::vector<SparseLayer> layers;
        layers.push_back(validity_layer(6, true, explicit_positions(2)));
        auto sp = layout(6, 6, std::move(layers));
        sp.layers[0].validity.positions = explicit_positions(2);
        std::vector<repdef::UnraveledLayer> out;
        std::string error;
        require(sparse::unravel(sp, 6, true, {positions_buffer({1, 4})}, out, error), "null positions: " + error);
        require(out.size() == 1 && out[0].length == 6, "flat leaf is one level of six items");
        require(out[0].null_count == 2 && out[0].validity.size() == 1, "two nulls");
        require(out[0].validity[0] == 0x2D, "validity bits: rows 1 and 4 null");  // 0010 1101
    }
    // Six rows, mostly null: valid positions given (only 0 and 2, 3, 5 valid -> valid positions 0,2,3,5).
    {
        std::vector<SparseLayer> layers;
        layers.push_back(validity_layer(6, false, explicit_positions(4)));
        auto sp = layout(6, 6, std::move(layers));
        std::vector<repdef::UnraveledLayer> out;
        std::string error;
        require(sparse::unravel(sp, 6, true, {positions_buffer({0, 2, 3, 5})}, out, error), "valid positions: " + error);
        require(out[0].null_count == 2, "valid positions leave two nulls");
        require(out[0].validity[0] == 0x2D, "validity bits from valid positions 0,2,3,5");  // 0010 1101
    }
    // All valid: no bitmap at all, as the level-encoded path has it.
    {
        std::vector<SparseLayer> layers;
        layers.push_back(validity_layer(3, true, empty_positions()));
        auto sp = layout(3, 3, std::move(layers));
        std::vector<repdef::UnraveledLayer> out;
        std::string error;
        require(sparse::unravel(sp, 3, true, {}, out, error), "all valid: " + error);
        require(out[0].null_count == 0 && out[0].validity.empty(), "all valid leaves no bitmap");
    }
}

void test_flat_non_nullable_leaf_has_no_layers() {
    auto sp = layout(4, 4, {});
    std::vector<repdef::UnraveledLayer> out;
    std::string error;
    require(sparse::unravel(sp, 4, false, {}, out, error), "flat non-null: " + error);
    require(out.size() == 1 && out[0].length == 4 && out[0].null_count == 0, "one level of four items");
    require(!sparse::unravel(sp, 5, false, {}, out, error), "rows must match the flat page");
}

void test_list_with_empty_slots() {
    // rows: [ [1,2], [], [3,4,5] ] -- rows 0 and 2 non-empty, row 1 valid and empty.
    std::vector<SparseLayer> layers;
    layers.push_back(list_layer(3, 5, explicit_positions(2), 0, empty_positions(), true, true, 2));
    auto sp = layout(6, 5, std::move(layers));  // items: 5 leaves + one placeholder for the empty row
    sp.layers[0].non_empty = explicit_positions(2);
    std::vector<repdef::UnraveledLayer> out;
    std::string error;
    require(sparse::unravel(sp, 3, false, {positions_buffer({0, 2}), u64_bytes({2, 3})}, out, error),
            "list with empty: " + error);
    require(out.size() == 2 && out[0].length == 5 && out[1].length == 3, "items and rows");
    require(out[1].offsets == std::vector<std::int64_t>({0, 2, 2, 5}), "offsets");
    require(out[1].kind == repdef::kEmptyableList && out[1].null_count == 0, "emptyable, no nulls");
}

void test_list_with_null_and_empty() {
    // rows: [null, [7,8], []] -- row 0 is null and empty, row 1 has two children, row 2 is an empty list.
    std::vector<SparseLayer> layers;
    SparsePositions valid = explicit_positions(2);  // valid positions 1 and 2
    layers.push_back(list_layer(3, 2, explicit_positions(1), 2, std::move(valid), false, false, 0));
    auto sp = layout(4, 2, std::move(layers));  // 2 leaves + placeholders for rows 0 and 2
    sp.layers[0].non_empty = explicit_positions(1);
    sp.layers[0].validity.positions = explicit_positions(2);
    sp.layers[0].validity.null_positions = false;  // the valid positions are 1 and 2
    std::vector<repdef::UnraveledLayer> out;
    std::string error;
    // Non-empty position 1 (count 2); validity valid positions 1,2.
    require(sparse::unravel(sp, 3, false,
                            {positions_buffer({1}), positions_buffer({1, 2})},
                            out, error),
            "list with null and empty: " + error);
    require(out[1].null_count == 1 && out[1].kind == repdef::kNullAndEmptyList, "null and empty list");
    require(out[1].offsets == std::vector<std::int64_t>({0, 0, 2, 2}), "null slot takes no children");
}

void test_list_of_lists() {
    // List<List<int32>>, two rows: row 0 = [[1], [2, 3]], row 1 = []. Leaf: 3 values, not nullable.
    // Outer layer: 2 rows, 2 inner lists, row 0 non-empty with 2 children (a constant count).
    // Inner layer: 2 inner lists, 3 leaves, both non-empty with counts 1 and 2 (explicit).
    std::vector<SparseLayer> layers;
    layers.push_back(list_layer(2, 2, explicit_positions(1), 2, empty_positions(), true, false, 0));
    layers.push_back(list_layer(2, 3, explicit_positions(2), 0, empty_positions(), true, true, 2));
    layers[1].counts.kind = page_layout::SparseCounts::Kind::kExplicit;
    layers[1].counts.encoding = std::make_unique<Compressive>(flat(64));
    auto sp = layout(3 + 0, 3, std::move(layers));  // 3 leaves; the empty outer row takes no placeholder here
    sp.num_items = 3 + (2 - 1) + (2 - 2);            // outer: one empty slot (row 1); inner: none
    std::vector<repdef::UnraveledLayer> out;
    std::string error;
    // Buffers in layer order: outer non-empty (constant count, so no counts buffer), inner non-empty,
    // inner counts.
    require(sparse::unravel(sp, 2, false,
                            {positions_buffer({0}), positions_buffer({0, 1}), u64_bytes({1, 2})}, out, error),
            "list of lists: " + error);
    require(out.size() == 3, "three levels: leaves, inner lists, outer rows");
    require(out[0].length == 3 && out[1].length == 2 && out[2].length == 2, "lengths 3, 2, 2");
    require(out[1].offsets == std::vector<std::int64_t>({0, 1, 3}), "inner offsets");
    require(out[2].offsets == std::vector<std::int64_t>({0, 2, 2}), "outer offsets");
    require(repdef::is_list_layer(out[1].kind) && repdef::is_list_layer(out[2].kind), "both layers are lists");
    require(out[2].kind == repdef::kEmptyableList, "row 1 is an empty list");
}

void test_null_slot_with_children_is_refused() {
    std::vector<SparseLayer> layers;
    // Slot 1 is null (valid positions = {0}) yet has one child.
    layers.push_back(list_layer(2, 1, explicit_positions(1), 1, explicit_positions(1), false, false, 0));
    auto sp = layout(2, 1, std::move(layers));
    sp.layers[0].non_empty = explicit_positions(1);
    sp.layers[0].validity.positions = explicit_positions(1);
    std::vector<repdef::UnraveledLayer> out;
    std::string error;
    require(!sparse::unravel(sp, 2, false, {positions_buffer({1}), positions_buffer({0})}, out, error),
            "a null slot with children is refused");
}

void test_count_and_item_mismatches_are_refused() {
    std::vector<SparseLayer> layers;
    layers.push_back(list_layer(2, 4, explicit_positions(2), 0, empty_positions(), true, true, 2));
    auto sp = layout(2 + 0, 3, std::move(layers));  // child slots 4 but the leaf has 3
    sp.layers[0].non_empty = explicit_positions(2);
    std::vector<repdef::UnraveledLayer> out;
    std::string error;
    require(!sparse::unravel(sp, 2, false, {positions_buffer({0, 1}), u64_bytes({2, 2})}, out, error),
            "child domain mismatch is refused");

    // Item count: one empty slot means num_items must be leaves + 1.
    std::vector<SparseLayer> ok;
    ok.push_back(list_layer(2, 2, explicit_positions(1), 2, empty_positions(), true, false, 0));
    ok[0].non_empty = explicit_positions(1);
    auto wrong_items = layout(2, 2, std::move(ok));  // should be 3
    require(!sparse::unravel(wrong_items, 2, false, {positions_buffer({0})}, out, error),
            "item count mismatch is refused");

    // A buffer missing for an explicit set.
    std::vector<SparseLayer> missing;
    missing.push_back(validity_layer(2, true, explicit_positions(1)));
    auto sp2 = layout(2, 2, std::move(missing));
    require(!sparse::unravel(sp2, 2, true, {}, out, error), "missing structural buffer is refused");
    require(!sparse::unravel(sp2, 2, true, {positions_buffer({0}), positions_buffer({0})}, out, error),
            "extra structural buffer is refused");
}

void test_fixed_size_list_is_refused_by_name() {
    SparseLayer f;
    f.kind = SparseLayer::Kind::kFixedSizeList;
    f.num_slots = 2;
    f.dimension = 2;
    f.validity.positions = empty_positions();
    std::vector<SparseLayer> layers;
    layers.push_back(std::move(f));
    auto sp = layout(4, 4, std::move(layers));
    std::vector<repdef::UnraveledLayer> out;
    std::string error;
    require(!sparse::unravel(sp, 2, false, {}, out, error), "fixed-size-list layers are refused");
    require(error.find("fixed-size-list") != std::string::npos, "the refusal names the layer: " + error);
}

void test_chunk_items() {
    // Two chunks: 4 values in 16 bytes (words 1 -> 16 bytes), 2 values in 8 bytes.
    Bytes meta;
    const auto entry = [&](std::uint32_t words_minus_one, std::uint32_t values) {
        for (int i = 0; i < 4; ++i) meta.push_back(static_cast<std::uint8_t>(words_minus_one >> (8 * i)));
        for (int i = 0; i < 4; ++i) meta.push_back(static_cast<std::uint8_t>(values >> (8 * i)));
    };
    entry(1, 4);
    entry(0, 2);
    std::vector<std::uint64_t> items;
    std::string error;
    require(sparse::chunk_items(meta, 24, 6, items, error), "chunk items: " + error);
    require(items == std::vector<std::uint64_t>({4, 2}), "per-chunk counts");
    require(!sparse::chunk_items(meta, 24, 7, items, error), "visible count mismatch refused");
    require(!sparse::chunk_items(meta, 16, 6, items, error), "value byte count mismatch refused");
    require(!sparse::chunk_items(Bytes(5, 0), 8, 1, items, error), "partial entries refused");
}

void test_child_block_flat_inline_bitpacked_and_general() {
    std::string error;
    std::vector<std::uint8_t> out;
    // Flat(16) with a count, and without one.
    const Bytes flat16 = {1, 0, 2, 0, 3, 0};
    Compressive f = flat(16);
    require(child_block::decode(f, flat16, 3, 16, out, error) && out == flat16, "flat with count");
    require(child_block::decode(f, flat16, std::nullopt, 16, out, error) && out == flat16, "flat without count");
    require(!child_block::decode(f, flat16, 2, 16, out, error), "flat with the wrong count refused");
    require(!child_block::decode(f, flat16, 3, 32, out, error), "flat with the wrong width refused");

    // InlineBitpacking(32): an inline block of values, needs the count.
    const Bytes values = {5, 0, 0, 0, 9, 0, 0, 0, 7, 0, 0, 0};
    Bytes block;
    bitpack::encode_block(values.data(), 3, 4, block);
    Compressive ib;
    ib.kind = CompressiveKind::kInlineBitpacking;
    ib.bits_per_value = 32;
    require(child_block::needs_count(ib), "inline bitpacking needs its count");
    require(child_block::decode(ib, block, 3, 32, out, error) && out == values, "inline bitpacked round trip: " + error);
    require(!child_block::decode(ib, block, std::nullopt, 32, out, error), "inline bitpacking without a count refused");

    // General(zstd, Flat(32)): framed, self-sizing.
    Bytes raw = {1, 0, 0, 0, 2, 0, 0, 0};
    Bytes framed;
    require(general::zstd_frame(raw, 1, framed, error), "zstd frame: " + error);
    Compressive gen;
    gen.kind = CompressiveKind::kGeneral;
    gen.scheme = page_layout::BufferScheme::kZstd;
    gen.values = std::make_unique<Compressive>(flat(32));
    require(!child_block::needs_count(gen), "general over flat is self-sizing");
    require(child_block::decode(gen, framed, std::nullopt, 32, out, error) && out == raw, "general round trip: " + error);
    require(child_block::value_bits(gen) == 32, "general reports its inner width");
}

void test_rle_chunk_widths_and_child_encodings() {
    std::string error;
    std::vector<std::uint8_t> out;
    // Values 0x0102 x2 and 0x0304 x3, lengths as u16.
    const Bytes values = {0x02, 0x01, 0x04, 0x03};
    const Bytes lengths = {2, 0, 3, 0};
    require(child_block::decode_rle_chunk(flat(16), flat(16), values, lengths, 16, 16, out, error),
            "u16 lengths: " + error);
    require(out == Bytes({0x02, 0x01, 0x02, 0x01, 0x04, 0x03, 0x04, 0x03, 0x04, 0x03}), "runs expanded");

    // Values bitpacked (inline, so they need the run count): the lengths give it.
    const Bytes run_values = {1, 0, 0, 0, 2, 0, 0, 0};
    Bytes packed;
    bitpack::encode_block(run_values.data(), 2, 4, packed);
    Compressive ib;
    ib.kind = CompressiveKind::kInlineBitpacking;
    ib.bits_per_value = 32;
    out.clear();
    require(child_block::decode_rle_chunk(ib, flat(8), packed, Bytes({3, 1}), 32, 8, out, error),
            "bitpacked values: " + error);
    require(out == Bytes({1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0}), "bitpacked-values runs");

    // Two children that both need the run count are refused.
    require(!child_block::decode_rle_chunk(ib, ib, packed, packed, 32, 32, out, error),
            "both children need the count refused");
    // Lengths that do not match the values' runs are refused.
    require(!child_block::decode_rle_chunk(flat(16), flat(8), values, Bytes({2, 3, 4}), 16, 8, out, error),
            "run count mismatch refused");
    require(error.find("run count") != std::string::npos, "the refusal names the mismatch: " + error);
}

}  // namespace

int main() {
    test_descriptor_parses_as_sparse();
    test_value_descriptor_reads_back_as_mini_block();
    test_flat_nullable_leaf_both_polarities();
    test_flat_non_nullable_leaf_has_no_layers();
    test_list_with_empty_slots();
    test_list_with_null_and_empty();
    test_list_of_lists();
    test_null_slot_with_children_is_refused();
    test_count_and_item_mismatches_are_refused();
    test_fixed_size_list_is_refused_by_name();
    test_chunk_items();
    test_child_block_flat_inline_bitpacked_and_general();
    test_rle_chunk_widths_and_child_encodings();
    std::cout << "sparse layout and child block tests passed\n";
    return 0;
}
