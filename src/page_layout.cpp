// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/page_layout.hpp"

#include <cstring>

namespace nano_lance::page_layout {
namespace {

constexpr std::uint8_t kWireVarint = 0;
constexpr std::uint8_t kWireBytes = 2;

/// Lance descriptors are a handful of nodes deep (General -> ByteStreamSplit -> Flat is the deepest
/// nanolance writes). This cap exists so a hostile descriptor cannot drive unbounded recursion; it
/// is far above anything legitimate.
constexpr int kMaxDepth = 16;

struct Cursor {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;

    bool done() const { return pos >= size; }
};

bool read_varint(Cursor& c, std::uint64_t& out) {
    std::uint64_t value = 0;
    unsigned shift = 0;
    while (c.pos < c.size) {
        const std::uint8_t byte = c.data[c.pos++];
        if (shift >= 64U) {
            return false;  // more continuation bytes than a u64 can hold
        }
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U) {
            out = value;
            return true;
        }
        shift += 7U;
    }
    return false;  // ran off the end mid-varint
}

/// Skip a field whose contents we do not model, so unknown tags never desynchronize the parse.
bool skip_field(Cursor& c, std::uint8_t wire_type) {
    std::uint64_t scratch = 0;
    switch (wire_type) {
        case kWireVarint:
            return read_varint(c, scratch);
        case 1:  // 64-bit
            if (c.size - c.pos < 8U) {
                return false;
            }
            c.pos += 8U;
            return true;
        case kWireBytes: {
            if (!read_varint(c, scratch) || scratch > c.size - c.pos) {
                return false;
            }
            c.pos += static_cast<std::size_t>(scratch);
            return true;
        }
        case 5:  // 32-bit
            if (c.size - c.pos < 4U) {
                return false;
            }
            c.pos += 4U;
            return true;
        default:
            return false;  // groups (3/4) and anything else: refuse rather than guess
    }
}

/// Read a length-delimited field's payload as a sub-cursor without copying.
bool read_submessage(Cursor& c, Cursor& out) {
    std::uint64_t len = 0;
    if (!read_varint(c, len) || len > c.size - c.pos) {
        return false;
    }
    out.data = c.data + c.pos;
    out.size = static_cast<std::size_t>(len);
    out.pos = 0;
    c.pos += static_cast<std::size_t>(len);
    return true;
}

bool read_bytes(Cursor& c, std::vector<std::uint8_t>& out) {
    Cursor sub;
    if (!read_submessage(c, sub)) {
        return false;
    }
    out.assign(sub.data, sub.data + sub.size);
    return true;
}

bool parse_compressive(Cursor c, Compressive& out, int depth, std::string& error);

/// A node whose only content is `f1 varint` (Flat::bits_per_value, InlineBitpacking::uncompressed_bits).
bool parse_bits_node(Cursor c, std::uint32_t& bits, std::string& error) {
    while (!c.done()) {
        std::uint64_t key = 0;
        if (!read_varint(c, key)) {
            error = "page layout: malformed tag in bit-width node";
            return false;
        }
        const auto field = static_cast<std::uint32_t>(key >> 3U);
        const auto wire = static_cast<std::uint8_t>(key & 0x07U);
        if (field == 1U && wire == kWireVarint) {
            std::uint64_t value = 0;
            if (!read_varint(c, value)) {
                error = "page layout: malformed bit width";
                return false;
            }
            bits = static_cast<std::uint32_t>(value);
        } else if (!skip_field(c, wire)) {
            error = "page layout: malformed bit-width node";
            return false;
        }
    }
    return true;
}

/// A node holding exactly one nested CompressiveEncoding at `child_field`.
bool parse_wrapper_node(Cursor c, std::uint32_t child_field, std::unique_ptr<Compressive>& child,
                        int depth, std::string& error) {
    while (!c.done()) {
        std::uint64_t key = 0;
        if (!read_varint(c, key)) {
            error = "page layout: malformed tag in wrapper node";
            return false;
        }
        const auto field = static_cast<std::uint32_t>(key >> 3U);
        const auto wire = static_cast<std::uint8_t>(key & 0x07U);
        if (field == child_field && wire == kWireBytes) {
            Cursor sub;
            if (!read_submessage(c, sub)) {
                error = "page layout: truncated nested encoding";
                return false;
            }
            auto node = std::make_unique<Compressive>();
            if (!parse_compressive(sub, *node, depth + 1, error)) {
                return false;
            }
            child = std::move(node);
        } else if (!skip_field(c, wire)) {
            error = "page layout: malformed wrapper node";
            return false;
        }
    }
    return true;
}

bool parse_rle(Cursor c, Compressive& out, int depth, std::string& error) {
    while (!c.done()) {
        std::uint64_t key = 0;
        if (!read_varint(c, key)) {
            error = "page layout: malformed tag in Rle";
            return false;
        }
        const auto field = static_cast<std::uint32_t>(key >> 3U);
        const auto wire = static_cast<std::uint8_t>(key & 0x07U);
        if ((field == 1U || field == 2U) && wire == kWireBytes) {
            Cursor sub;
            if (!read_submessage(c, sub)) {
                error = "page layout: truncated Rle child";
                return false;
            }
            auto node = std::make_unique<Compressive>();
            if (!parse_compressive(sub, *node, depth + 1, error)) {
                return false;
            }
            (field == 1U ? out.values : out.lengths) = std::move(node);
        } else if (!skip_field(c, wire)) {
            error = "page layout: malformed Rle";
            return false;
        }
    }
    return true;
}

/// Fsst{ f1 symbol_table (bytes), f2 values (CompressiveEncoding) }. The symbol table is kept raw:
/// validating it is fsst::parse_symbol_table's job, and a descriptor parser that also rejected a
/// malformed table would report the wrong kind of error for a page nothing is going to read anyway.
bool parse_fsst(Cursor c, Compressive& out, int depth, std::string& error) {
    while (!c.done()) {
        std::uint64_t key = 0;
        if (!read_varint(c, key)) {
            error = "page layout: malformed tag in Fsst";
            return false;
        }
        const auto field = static_cast<std::uint32_t>(key >> 3U);
        const auto wire = static_cast<std::uint8_t>(key & 0x07U);
        if (field == 1U && wire == kWireBytes) {
            if (!read_bytes(c, out.symbol_table)) {
                error = "page layout: truncated Fsst symbol table";
                return false;
            }
        } else if (field == 2U && wire == kWireBytes) {
            Cursor sub;
            if (!read_submessage(c, sub)) {
                error = "page layout: truncated Fsst values";
                return false;
            }
            auto node = std::make_unique<Compressive>();
            if (!parse_compressive(sub, *node, depth + 1, error)) {
                return false;
            }
            out.values = std::move(node);
        } else if (!skip_field(c, wire)) {
            error = "page layout: malformed Fsst";
            return false;
        }
    }
    return true;
}

bool parse_general(Cursor c, Compressive& out, int depth, std::string& error) {
    while (!c.done()) {
        std::uint64_t key = 0;
        if (!read_varint(c, key)) {
            error = "page layout: malformed tag in General";
            return false;
        }
        const auto field = static_cast<std::uint32_t>(key >> 3U);
        const auto wire = static_cast<std::uint8_t>(key & 0x07U);
        if (field == 1U && wire == kWireBytes) {  // BufferCompression{ f1 scheme }
            Cursor sub;
            if (!read_submessage(c, sub)) {
                error = "page layout: truncated BufferCompression";
                return false;
            }
            std::uint32_t scheme = 0;
            if (!parse_bits_node(sub, scheme, error)) {
                return false;
            }
            out.wire_scheme = scheme;
            out.scheme = scheme == static_cast<std::uint32_t>(BufferScheme::kZstd)  ? BufferScheme::kZstd
                         : scheme == static_cast<std::uint32_t>(BufferScheme::kLz4) ? BufferScheme::kLz4
                         : scheme == 0U                                             ? BufferScheme::kNone
                                                                                    : BufferScheme::kUnknownScheme;
        } else if (field == 3U && wire == kWireBytes) {  // values
            Cursor sub;
            if (!read_submessage(c, sub)) {
                error = "page layout: truncated General values";
                return false;
            }
            auto node = std::make_unique<Compressive>();
            if (!parse_compressive(sub, *node, depth + 1, error)) {
                return false;
            }
            out.values = std::move(node);
        } else if (!skip_field(c, wire)) {
            error = "page layout: malformed General";
            return false;
        }
    }
    return true;
}

bool parse_compressive(Cursor c, Compressive& out, int depth, std::string& error) {
    if (depth > kMaxDepth) {
        error = "page layout: encoding tree nested too deeply";
        return false;
    }
    while (!c.done()) {
        std::uint64_t key = 0;
        if (!read_varint(c, key)) {
            error = "page layout: malformed tag in CompressiveEncoding";
            return false;
        }
        const auto field = static_cast<std::uint32_t>(key >> 3U);
        const auto wire = static_cast<std::uint8_t>(key & 0x07U);
        if (wire != kWireBytes) {
            if (!skip_field(c, wire)) {
                error = "page layout: malformed CompressiveEncoding";
                return false;
            }
            continue;
        }
        Cursor sub;
        if (!read_submessage(c, sub)) {
            error = "page layout: truncated CompressiveEncoding variant";
            return false;
        }
        // The first recognized variant wins; a message carries exactly one in practice.
        out.wire_field = field;
        switch (field) {
            case 1U:
                out.kind = CompressiveKind::kFlat;
                if (!parse_bits_node(sub, out.bits_per_value, error)) {
                    return false;
                }
                break;
            case 2U:
                out.kind = CompressiveKind::kVariable;
                if (!parse_wrapper_node(sub, 1U, out.values, depth, error)) {
                    return false;
                }
                break;
            case 4U:
                // The wrapper Lance puts around a definition-level buffer: f1 is the level's
                // uncompressed width (16 bits), f3 how the levels are actually stored (Flat(1) when
                // the only levels are 0 and 1, i.e. a plain nullable column).
                out.kind = CompressiveKind::kBitpacked;
                if (!parse_bits_node(sub, out.bits_per_value, error) ||
                    !parse_wrapper_node(sub, 3U, out.values, depth, error)) {
                    return false;
                }
                break;
            case 5U:
                out.kind = CompressiveKind::kInlineBitpacking;
                if (!parse_bits_node(sub, out.bits_per_value, error)) {
                    return false;
                }
                break;
            case 6U:
                out.kind = CompressiveKind::kFsst;
                if (!parse_fsst(sub, out, depth, error)) {
                    return false;
                }
                break;
            case 8U:
                out.kind = CompressiveKind::kRle;
                if (!parse_rle(sub, out, depth, error)) {
                    return false;
                }
                break;
            case 9U:
                out.kind = CompressiveKind::kByteStreamSplit;
                if (!parse_wrapper_node(sub, 1U, out.values, depth, error)) {
                    return false;
                }
                break;
            case 10U:
                out.kind = CompressiveKind::kGeneral;
                if (!parse_general(sub, out, depth, error)) {
                    return false;
                }
                break;
            default:
                // Parsed successfully, just not modeled. Recording the wire field lets the caller
                // refuse by name instead of misreading the page's buffers.
                out.kind = CompressiveKind::kUnknown;
                break;
        }
        return true;
    }
    return true;  // empty message: kUnknown, which callers treat as "cannot decode"
}

bool parse_mini_block(Cursor c, MiniBlock& out, std::string& error) {
    while (!c.done()) {
        std::uint64_t key = 0;
        if (!read_varint(c, key)) {
            error = "page layout: malformed tag in MiniBlockLayout";
            return false;
        }
        const auto field = static_cast<std::uint32_t>(key >> 3U);
        const auto wire = static_cast<std::uint8_t>(key & 0x07U);
        if (wire == kWireBytes && field == 1U) {
            // rep_compression. Not modelled beyond its presence -- but the chunk header reserves a
            // slot for the repetition buffer's size when this field exists, so the flag is
            // load-bearing for parsing every chunk in the page.
            Cursor sub;
            if (!read_submessage(c, sub)) {
                error = "page layout: truncated MiniBlockLayout rep_compression";
                return false;
            }
            out.has_repetition = true;
        } else if (wire == kWireBytes && (field == 2U || field == 3U || field == 4U)) {
            Cursor sub;
            if (!read_submessage(c, sub)) {
                error = "page layout: truncated MiniBlockLayout encoding";
                return false;
            }
            auto node = std::make_unique<Compressive>();
            if (!parse_compressive(sub, *node, 0, error)) {
                return false;
            }
            (field == 2U   ? out.repdef_compression
             : field == 3U ? out.value_compression
                           : out.dictionary) = std::move(node);
        } else if (wire == kWireBytes && field == 6U) {
            if (!read_bytes(c, out.layers)) {
                error = "page layout: truncated MiniBlockLayout layers";
                return false;
            }
        } else if (wire == kWireVarint && (field == 5U || field == 7U || field == 9U || field == 10U)) {
            std::uint64_t value = 0;
            if (!read_varint(c, value)) {
                error = "page layout: malformed MiniBlockLayout scalar";
                return false;
            }
            switch (field) {
                case 5U:
                    out.num_dictionary_items = value;
                    break;
                case 7U:
                    out.num_buffers = static_cast<std::uint32_t>(value);
                    break;
                case 9U:
                    out.num_items = value;
                    break;
                default:
                    out.has_large_chunk = value != 0U;
                    break;
            }
        } else if (!skip_field(c, wire)) {
            error = "page layout: malformed MiniBlockLayout";
            return false;
        }
    }
    return true;
}

bool parse_constant(Cursor c, Constant& out, std::string& error) {
    while (!c.done()) {
        std::uint64_t key = 0;
        if (!read_varint(c, key)) {
            error = "page layout: malformed tag in ConstantLayout";
            return false;
        }
        const auto field = static_cast<std::uint32_t>(key >> 3U);
        const auto wire = static_cast<std::uint8_t>(key & 0x07U);
        if (field == 5U && wire == kWireBytes) {
            if (!read_bytes(c, out.layers)) {
                error = "page layout: truncated ConstantLayout layers";
                return false;
            }
        } else if (field == 6U && wire == kWireBytes) {
            std::vector<std::uint8_t> value;
            if (!read_bytes(c, value)) {
                error = "page layout: truncated ConstantLayout inline value";
                return false;
            }
            out.inline_value = std::move(value);
        } else if (wire == kWireBytes && (field == 7U || field == 8U)) {
            // A constant page can be nullable: the same value in every non-null row, with the
            // definition levels in a buffer of their own. Skipping these fields meant a nullable
            // constant column read back with every row valid -- silent corruption, not a refusal.
            Cursor sub;
            if (!read_submessage(c, sub)) {
                error = "page layout: truncated ConstantLayout level encoding";
                return false;
            }
            auto node = std::make_unique<Compressive>();
            if (!parse_compressive(sub, *node, 0, error)) {
                return false;
            }
            (field == 7U ? out.rep_compression : out.def_compression) = std::move(node);
        } else if (wire == kWireVarint && (field == 9U || field == 10U)) {
            std::uint64_t value = 0;
            if (!read_varint(c, value)) {
                error = "page layout: malformed ConstantLayout level count";
                return false;
            }
            (field == 9U ? out.num_rep_values : out.num_def_values) = value;
        } else if (!skip_field(c, wire)) {
            error = "page layout: malformed ConstantLayout";
            return false;
        }
    }
    return true;
}

}  // namespace

namespace {

/// Parse into `out`, which the caller has already reset. Split from decode_page_layout so that
/// function can build into a scratch value and publish it only on success -- see the note there.
bool decode_page_layout_into(const std::vector<std::uint8_t>& encoding, PageLayout& out, std::string& error) {

    // Outer wrapper: f1 type_url string, f2 the PageLayout payload.
    Cursor c{encoding.data(), encoding.size(), 0};
    std::vector<std::uint8_t> type_url;
    Cursor payload{};
    bool have_payload = false;
    while (!c.done()) {
        std::uint64_t key = 0;
        if (!read_varint(c, key)) {
            error = "page layout: malformed tag in encoding wrapper";
            return false;
        }
        const auto field = static_cast<std::uint32_t>(key >> 3U);
        const auto wire = static_cast<std::uint8_t>(key & 0x07U);
        if (field == 1U && wire == kWireBytes) {
            if (!read_bytes(c, type_url)) {
                error = "page layout: truncated encoding type url";
                return false;
            }
        } else if (field == 2U && wire == kWireBytes) {
            if (!read_submessage(c, payload)) {
                error = "page layout: truncated page layout payload";
                return false;
            }
            have_payload = true;
        } else if (!skip_field(c, wire)) {
            error = "page layout: malformed encoding wrapper";
            return false;
        }
    }

    const std::string url(type_url.begin(), type_url.end());
    if (!url.empty() && url.find("PageLayout") == std::string::npos) {
        error = "page layout: unexpected encoding type url '" + url + "'";
        return false;
    }
    if (!have_payload) {
        return true;  // wrapper with no payload; treated as "no descriptor"
    }

    while (!payload.done()) {
        std::uint64_t key = 0;
        if (!read_varint(payload, key)) {
            error = "page layout: malformed tag in PageLayout";
            return false;
        }
        const auto field = static_cast<std::uint32_t>(key >> 3U);
        const auto wire = static_cast<std::uint8_t>(key & 0x07U);
        if (wire != kWireBytes) {
            if (!skip_field(payload, wire)) {
                error = "page layout: malformed PageLayout";
                return false;
            }
            continue;
        }
        Cursor sub;
        if (!read_submessage(payload, sub)) {
            error = "page layout: truncated PageLayout variant";
            return false;
        }
        if (field == 1U) {
            out.kind = LayoutKind::kMiniBlock;
            return parse_mini_block(sub, out.mini_block, error);
        }
        if (field == 2U) {
            out.kind = LayoutKind::kConstant;
            return parse_constant(sub, out.constant, error);
        }
        // Field 3 is FullZipLayout -- that is what a lance.blob.v2 packed column's pages use, and
        // what nanolance's own blob writer emits. Field 4 and beyond (AllNull, and whatever Lance
        // adds later) are likewise parsed but not modeled: record the field so a caller refuses by
        // name rather than misreading the page's buffers.
        out.kind = LayoutKind::kNone;
        out.unknown_layout_field = field;
        return true;
    }
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
    // body is parsed (a PageLayout field 1 means MiniBlock whether or not the MiniBlock parses), so
    // writing straight into `out` left a failed parse reporting kind = kMiniBlock with an empty body.
    // A caller that checks the kind before the return value -- which is exactly how decode dispatch
    // will read this -- would then select a decoder from a descriptor that did not parse. Found by
    // tests/fuzz/fuzz_page_layout.cpp, 25 executions in.
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
        case CompressiveKind::kUnknown:
        default:
            out += "Unknown(field " + std::to_string(node->wire_field) + ")";
            return;
    }
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
            out += ",rows=" + std::to_string(layout.mini_block.num_items);
            out += ",buffers=" + std::to_string(layout.mini_block.num_buffers);
            out += "}";
            return out;
        }
        case LayoutKind::kConstant:
            out = "Constant{";
            out += layout.constant.inline_value ? "inline " + std::to_string(layout.constant.inline_value->size()) + "B"
                   : layers_have_definition_levels(layout.constant.layers) ? "all-null"
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
