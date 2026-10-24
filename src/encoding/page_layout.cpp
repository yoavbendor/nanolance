// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/page_layout.hpp"

#include <nanom/formats/lance_encodings.hpp>

#include <span>
#include <string_view>

// The descriptor's wire format -- lance.encodings21's PageLayout and CompressiveEncoding messages --
// is declared once in nanom/formats/lance_encodings.hpp and read by nanom's protobuf codec, which
// bounds-checks every length, rejects malformed varints and truncated submessages, and caps nesting.
// This file turns that wire model into the Compressive / MiniBlock / Constant / FullZip nodes the
// decoder dispatches on.

namespace nano_lance::page_layout {
namespace {

namespace nm = ::nanom;
namespace wire = ::nanom_formats::lance;

/// Lance descriptors are a handful of nodes deep (General -> ByteStreamSplit -> Flat is the deepest
/// nanolance writes). This cap exists so a hostile descriptor cannot drive unbounded recursion; it
/// is far above anything legitimate.
constexpr int kMaxDepth = 16;

std::vector<std::uint8_t> owned(nm::bytes b) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(b.data());
    return std::vector<std::uint8_t>(p, p + b.size());
}

/// The field number of the first length-delimited record a pb_unknown holds: in a oneof message,
/// the variant this build does not model.
std::uint32_t first_unknown_variant(const nm::unknown_fields& u) {
    std::uint32_t field = 0;
    nm::for_each_unknown(u, [&](std::uint64_t f, std::uint8_t wire_type) {
        if (field == 0U && wire_type == 2U) {
            field = static_cast<std::uint32_t>(f);
        }
    });
    return field;
}

bool convert(const wire::CompressiveEncoding& w, Compressive& out, int depth, std::string& error);

/// A child node: its bytes were only length-checked with the parent, so they are decoded (and
/// fully checked) here, one level deeper.
bool child(const nm::pb_lazy<wire::CompressiveEncoding>& lazy, std::unique_ptr<Compressive>& out, int depth,
           std::string& error) {
    if (!lazy) {
        return true;
    }
    if (depth + 1 > kMaxDepth) {
        error = "page layout: encoding tree nested too deeply";
        return false;
    }
    wire::CompressiveEncoding w;
    if (auto r = lazy.decode_into(w); !r) {
        error = std::string("page layout: malformed CompressiveEncoding (expected ") + r.error().expected + ")";
        return false;
    }
    auto node = std::make_unique<Compressive>();
    if (!convert(w, *node, depth + 1, error)) {
        return false;
    }
    out = std::move(node);
    return true;
}

/// One CompressiveEncoding node. Exactly one variant is set in a valid message; an empty one stays
/// kUnknown (callers treat it as "cannot decode"), a variant this build does not model is kept by
/// field number so the caller refuses it by name.
bool convert(const wire::CompressiveEncoding& w, Compressive& out, int depth, std::string& error) {
    if (depth > kMaxDepth) {
        error = "page layout: encoding tree nested too deeply";
        return false;
    }
    const auto members = wire::oneof_members(w);
    if (members > 1U) {
        error = "page layout: CompressiveEncoding sets more than one variant";
        return false;
    }
    if (members == 0U) {
        return true;
    }
    if (const auto& v = *w.flat) {
        out.kind = CompressiveKind::kFlat;
        out.wire_field = 1U;
        out.bits_per_value = static_cast<std::uint32_t>(*v->bits_per_value);
        return true;
    }
    if (const auto& v = *w.variable) {
        out.kind = CompressiveKind::kVariable;
        out.wire_field = 2U;
        return child(*v->offsets, out.values, depth, error);
    }
    if (const auto& v = *w.out_of_line_bitpacking) {
        // The wrapper Lance puts around a definition-level buffer: f1 is the level's uncompressed
        // width (16 bits), f3 how the levels are actually stored (Flat(1) when the only levels are 0
        // and 1, i.e. a plain nullable column).
        out.kind = CompressiveKind::kBitpacked;
        out.wire_field = 4U;
        out.bits_per_value = static_cast<std::uint32_t>(*v->uncompressed_bits_per_value);
        return child(*v->values, out.values, depth, error);
    }
    if (const auto& v = *w.inline_bitpacking) {
        out.kind = CompressiveKind::kInlineBitpacking;
        out.wire_field = 5U;
        out.bits_per_value = static_cast<std::uint32_t>(*v->uncompressed_bits_per_value);
        return true;
    }
    if (const auto& v = *w.fsst) {
        // The symbol table is kept raw: validating it is fsst::parse_symbol_table's job.
        out.kind = CompressiveKind::kFsst;
        out.wire_field = 6U;
        out.symbol_table = owned(*v->symbol_table);
        return child(*v->values, out.values, depth, error);
    }
    if (const auto& v = *w.rle) {
        out.kind = CompressiveKind::kRle;
        out.wire_field = 8U;
        return child(*v->values, out.values, depth, error) && child(*v->run_lengths, out.lengths, depth, error);
    }
    if (const auto& v = *w.byte_stream_split) {
        out.kind = CompressiveKind::kByteStreamSplit;
        out.wire_field = 9U;
        return child(*v->values, out.values, depth, error);
    }
    if (const auto& v = *w.general) {
        out.kind = CompressiveKind::kGeneral;
        out.wire_field = 10U;
        const auto scheme = static_cast<std::uint32_t>(v->compression->has_value() ? *v->compression->value().scheme : 0U);
        out.wire_scheme = scheme;
        out.scheme = scheme == static_cast<std::uint32_t>(BufferScheme::kZstd)  ? BufferScheme::kZstd
                     : scheme == static_cast<std::uint32_t>(BufferScheme::kLz4) ? BufferScheme::kLz4
                     : scheme == 0U                                             ? BufferScheme::kNone
                                                                                : BufferScheme::kUnknownScheme;
        return child(*v->values, out.values, depth, error);
    }
    if (const auto& v = *w.fixed_size_list) {
        out.kind = CompressiveKind::kFixedSizeList;
        out.wire_field = 11U;
        out.items_per_value = *v->items_per_value;
        out.has_validity = *v->has_validity != 0U;
        return child(*v->values, out.values, depth, error);
    }
    // Parsed, just not modeled. Recording the wire field lets the caller refuse by name instead of
    // misreading the page's buffers.
    out.kind = CompressiveKind::kUnknown;
    out.wire_field = first_unknown_variant(*w.unknown);
    return true;
}

/// A layout's own encoding (the root of a tree): depth starts at 0 again, as it always has.
bool convert_root(const nm::pb_lazy<wire::CompressiveEncoding>& lazy, std::unique_ptr<Compressive>& out,
                  std::string& error) {
    return child(lazy, out, -1, error);
}

bool convert(const wire::MiniBlockLayout& w, MiniBlock& out, std::string& error) {
    // rep_compression: its mere presence is load-bearing -- the chunk header reserves a slot for the
    // repetition buffer's size when this field exists.
    out.has_repetition = w.rep_compression->has_value();
    out.num_dictionary_items = *w.num_dictionary_items;
    out.layers = owned(*w.layers);
    out.num_buffers = static_cast<std::uint32_t>(*w.num_buffers);
    out.repetition_index_depth = static_cast<std::uint32_t>(*w.repetition_index_depth);
    out.num_items = *w.num_items;
    out.has_large_chunk = *w.has_large_chunk != 0U;
    return convert_root(*w.rep_compression, out.rep_compression, error) &&
           convert_root(*w.def_compression, out.repdef_compression, error) &&
           convert_root(*w.value_compression, out.value_compression, error) &&
           convert_root(*w.dictionary, out.dictionary, error);
}

bool convert(const wire::ConstantLayout& w, Constant& out, std::string& error) {
    out.layers = owned(*w.layers);
    if (const auto& value = *w.inline_value) {
        out.inline_value = owned(*value);
    }
    // A constant page can be nullable: the same value in every non-null row, with the definition
    // levels in a buffer of their own. Skipping these fields meant a nullable constant column read
    // back with every row valid -- silent corruption, not a refusal.
    out.num_rep_values = *w.num_rep_values;
    out.num_def_values = *w.num_def_values;
    return convert_root(*w.rep_compression, out.rep_compression, error) &&
           convert_root(*w.def_compression, out.def_compression, error);
}

bool convert(const wire::FullZipLayout& w, FullZip& out, std::string& error) {
    out.bits_rep = static_cast<std::uint32_t>(*w.bits_rep);
    out.bits_def = static_cast<std::uint32_t>(*w.bits_def);
    out.bits_per_value = static_cast<std::uint32_t>(*w.bits_per_value);
    out.bits_per_offset = static_cast<std::uint32_t>(*w.bits_per_offset);
    out.num_items = *w.num_items;
    out.num_visible_items = *w.num_visible_items;
    out.layers = owned(*w.layers);
    return convert_root(*w.value_compression, out.value_compression, error);
}

template <class M>
bool decode_wire(nm::bytes bytes, M& out, const char* what, std::string& error) {
    if (auto r = nm::protobuf_decode(nm::from(bytes), out); !r) {
        error = std::string("page layout: malformed ") + what + " (expected " + r.error().expected + ")";
        return false;
    }
    return true;
}

/// Parse into `out`, which the caller has already reset. Split from decode_page_layout so that
/// function can build into a scratch value and publish it only on success -- see the note there.
bool decode_page_layout_into(const std::vector<std::uint8_t>& encoding, PageLayout& out, std::string& error) {
    // Outer wrapper: f1 type_url string, f2 the PageLayout payload.
    wire::EncodingAny any;
    if (!decode_wire(nm::bytes(reinterpret_cast<const std::byte*>(encoding.data()), encoding.size()), any,
                     "encoding wrapper", error)) {
        return false;
    }
    const std::string_view url = *any.type_url;
    if (!url.empty() && url.find("PageLayout") == std::string_view::npos) {
        error = "page layout: unexpected encoding type url '" + std::string(url) + "'";
        return false;
    }
    if (!*any.value) {
        return true;  // wrapper with no payload; treated as "no descriptor"
    }
    wire::PageLayout layout;
    if (!decode_wire(**any.value, layout, "PageLayout", error)) {
        return false;
    }
    const auto members = wire::oneof_members(layout);
    if (members > 1U) {
        error = "page layout: PageLayout sets more than one layout";
        return false;
    }
    if (const auto& v = *layout.mini_block_layout) {
        out.kind = LayoutKind::kMiniBlock;
        return convert(*v, out.mini_block, error);
    }
    if (const auto& v = *layout.constant_layout) {
        out.kind = LayoutKind::kConstant;
        return convert(*v, out.constant, error);
    }
    if (const auto& v = *layout.full_zip_layout) {
        // Also what a lance.blob.v2 packed column's pages use; the decoder routes those by the
        // column's `lance-encoding:blob` metadata before it ever looks at the layout.
        out.kind = LayoutKind::kFullZip;
        return convert(*v, out.full_zip, error);
    }
    // Field 4 (BlobLayout), 5 (SparseLayout, file version 2.3+) and whatever Lance adds later are
    // parsed but not modeled: record the field so a caller refuses by name rather than misreading
    // the page's buffers.
    out.kind = LayoutKind::kNone;
    out.unknown_layout_field = first_unknown_variant(*layout.unknown);
    return true;
}

}  // namespace

bool decode_page_layout(const std::vector<std::uint8_t>& encoding, PageLayout& out, std::string& error) {
    out = PageLayout{};
    error.clear();
    if (encoding.empty()) {
        return true;  // no descriptor recorded; caller falls back
    }

    // Build into a scratch value and publish only on success. The layout KIND is picked before its
    // body is converted, so writing straight into `out` would leave a failed parse reporting a kind
    // with an empty body -- and a caller that checks the kind before the return value would select a
    // decoder from a descriptor that did not parse. Found by tests/fuzz/fuzz_page_layout.cpp.
    PageLayout scratch;
    if (!decode_page_layout_into(encoding, scratch, error)) {
        return false;  // `out` stays reset
    }
    out = std::move(scratch);
    return true;
}

namespace {

void describe_compressive(const Compressive* node, std::string& out) {
    if (node == nullptr) {
        out += "none";
        return;
    }
    switch (node->kind) {
        case CompressiveKind::kFlat:
            out += "Flat(" + std::to_string(node->bits_per_value) + ")";
            return;
        case CompressiveKind::kInlineBitpacking:
            out += "InlineBitpacking(" + std::to_string(node->bits_per_value) + ")";
            return;
        case CompressiveKind::kBitpacked:
            out += "Bitpacked(" + std::to_string(node->bits_per_value) + ",";
            describe_compressive(node->values.get(), out);
            out += ")";
            return;
        case CompressiveKind::kVariable:
            out += "Variable{offsets=";
            describe_compressive(node->values.get(), out);
            out += "}";
            return;
        case CompressiveKind::kRle:
            out += "Rle{values=";
            describe_compressive(node->values.get(), out);
            out += ",lengths=";
            describe_compressive(node->lengths.get(), out);
            out += "}";
            return;
        case CompressiveKind::kFsst:
            // The symbol table's SIZE, not its bytes: it is 2312 bytes of table, and the one thing a
            // reader of this line wants to know is whether one is present and well-sized.
            out += "Fsst{symbols=" + std::to_string(node->symbol_table.size()) + "B,values=";
            describe_compressive(node->values.get(), out);
            out += "}";
            return;
        case CompressiveKind::kByteStreamSplit:
            out += "ByteStreamSplit{";
            describe_compressive(node->values.get(), out);
            out += "}";
            return;
        case CompressiveKind::kGeneral:
            out += "General{";
            out += node->scheme == BufferScheme::kZstd   ? "ZSTD"
                   : node->scheme == BufferScheme::kLz4  ? "LZ4"
                   : node->scheme == BufferScheme::kNone ? "NONE"
                                                         : "scheme " + std::to_string(node->wire_scheme);
            out += ",";
            describe_compressive(node->values.get(), out);
            out += "}";
            return;
        case CompressiveKind::kFixedSizeList:
            out += "FixedSizeList{" + std::to_string(node->items_per_value) + "x";
            describe_compressive(node->values.get(), out);
            out += node->has_validity ? ",validity}" : "}";
            return;
        case CompressiveKind::kUnknown:
        default:
            out += "Unknown(field " + std::to_string(node->wire_field) + ")";
            return;
    }
}

/// `RepDefLayer` names, outermost layer last, as Lance stores them.
void describe_layers(const std::vector<std::uint8_t>& layers, std::string& out) {
    out += "[";
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (i != 0U) {
            out += ",";
        }
        switch (layers[i]) {
            case 1U: out += "valid-item"; break;
            case 2U: out += "valid-list"; break;
            case 3U: out += "nullable-item"; break;
            case 4U: out += "nullable-list"; break;
            case 5U: out += "emptyable-list"; break;
            case 6U: out += "null+empty-list"; break;
            default: out += "layer" + std::to_string(layers[i]); break;
        }
    }
    out += "]";
}

}  // namespace

std::string describe_encoding(const Compressive& node) {
    std::string out;
    describe_compressive(&node, out);
    return out;
}

std::string describe(const PageLayout& layout) {
    std::string out;
    switch (layout.kind) {
        case LayoutKind::kMiniBlock: {
            out = "MiniBlock{values=";
            describe_compressive(layout.mini_block.value_compression.get(), out);
            if (layout.mini_block.rep_compression) {
                out += ",rep=";
                describe_compressive(layout.mini_block.rep_compression.get(), out);
            }
            if (layout.mini_block.repdef_compression) {
                out += ",repdef=";
                describe_compressive(layout.mini_block.repdef_compression.get(), out);
            }
            if (layers_have_definition_levels(layout.mini_block.layers)) {
                out += ",nullable";
            }
            if (layout.mini_block.dictionary) {
                out += ",dict=";
                describe_compressive(layout.mini_block.dictionary.get(), out);
                out += "x" + std::to_string(layout.mini_block.num_dictionary_items);
            }
            if (layout.mini_block.has_repetition) {
                out += ",layers=";
                describe_layers(layout.mini_block.layers, out);
            }
            out += ",rows=" + std::to_string(layout.mini_block.num_items);
            out += ",buffers=" + std::to_string(layout.mini_block.num_buffers);
            out += "}";
            return out;
        }
        case LayoutKind::kFullZip: {
            const auto& fz = layout.full_zip;
            out = "FullZip{values=";
            describe_compressive(fz.value_compression.get(), out);
            out += fz.bits_per_offset != 0U ? ",offset_bits=" + std::to_string(fz.bits_per_offset)
                                            : ",value_bits=" + std::to_string(fz.bits_per_value);
            if (fz.bits_rep != 0U) {
                out += ",rep_bits=" + std::to_string(fz.bits_rep);
            }
            if (fz.bits_def != 0U) {
                out += ",def_bits=" + std::to_string(fz.bits_def);
            }
            if (!fz.layers.empty()) {
                out += ",layers=";
                describe_layers(fz.layers, out);
            }
            out += ",rows=" + std::to_string(fz.num_items);
            if (fz.num_visible_items != fz.num_items) {
                out += ",visible=" + std::to_string(fz.num_visible_items);
            }
            out += "}";
            return out;
        }
        case LayoutKind::kConstant:
            out = "Constant{";
            // With no inline value and a definition layer, the descriptor alone cannot say whether
            // the page is all-null or a nullable constant whose value sits in a buffer: that is
            // decided by the page's buffer count (0/2 vs 1/3). Saying "all-null" here was the same
            // mistake the decoder used to make, and it mislabelled a 2999-null + 1-value column.
            out += layout.constant.inline_value ? "inline " + std::to_string(layout.constant.inline_value->size()) + "B"
                   : layers_have_definition_levels(layout.constant.layers) ? "no-inline,nullable (all-null unless buffered; see -v)"
                                                                          : "buffered";
            // A constant page with definition levels is the nullable case -- the same value in every
            // non-null row. Worth naming in the dump: it reads identically to the non-null case
            // everywhere except the validity bitmap, which is exactly how it went unnoticed.
            if (layout.constant.num_def_values != 0U || layout.constant.def_compression != nullptr) {
                out += ",def=";
                out += layout.constant.def_compression != nullptr
                           ? describe_encoding(*layout.constant.def_compression)
                           : "raw u16";
                out += "x" + std::to_string(layout.constant.num_def_values);
            }
            out += "}";
            return out;
        case LayoutKind::kNone:
        default:
            return layout.unknown_layout_field != 0U
                       ? "Unsupported layout (PageLayout field " + std::to_string(layout.unknown_layout_field) + ")"
                       : "none";
    }
}

}  // namespace nano_lance::page_layout
