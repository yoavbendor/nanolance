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

bool read_varint(std::span<const std::byte> b, std::size_t& at, std::uint64_t& v) {
    v = 0;
    for (unsigned shift = 0; shift < 64U; shift += 7U) {
        if (at >= b.size()) {
            return false;
        }
        const auto byte = std::to_integer<std::uint8_t>(b[at++]);
        v |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U) {
            return true;
        }
    }
    return false;
}

/// Advances `at` past one protobuf field value of wire type `wt`. False on a malformed or group value.
bool skip_field(std::span<const std::byte> b, std::size_t& at, std::uint8_t wt) {
    std::uint64_t n = 0;
    switch (wt) {
        case 0U:
            return read_varint(b, at, n);
        case 1U:
            if (b.size() - at < 8U) {
                return false;
            }
            at += 8U;
            return true;
        case 2U:
            if (!read_varint(b, at, n) || n > b.size() - at) {
                return false;
            }
            at += static_cast<std::size_t>(n);
            return true;
        case 5U:
            if (b.size() - at < 4U) {
                return false;
            }
            at += 4U;
            return true;
        default:
            return false;
    }
}

/// CompressiveEncoding field 3, Constant{ optional bytes value = 1 }. nanom's CompressiveEncoding has
/// no member for it, so it arrives in `unknown` and is read here. `found` says whether it was there.
bool convert_constant(const nm::unknown_fields& u, Compressive& out, bool& found, std::string& error) {
    const std::span<const std::byte> all(u.bytes);
    std::size_t at = 0;
    while (at < all.size()) {
        std::uint64_t key = 0;
        if (!read_varint(all, at, key)) {
            error = "page layout: malformed CompressiveEncoding";
            return false;
        }
        const auto wt = static_cast<std::uint8_t>(key & 7U);
        if (key >> 3U != 3U || wt != 2U) {
            if (!skip_field(all, at, wt)) {
                error = "page layout: malformed CompressiveEncoding";
                return false;
            }
            continue;
        }
        std::uint64_t n = 0;
        if (!read_varint(all, at, n) || n > all.size() - at) {
            error = "page layout: malformed Constant";
            return false;
        }
        const auto payload = all.subspan(at, static_cast<std::size_t>(n));
        at += static_cast<std::size_t>(n);
        found = true;
        out.kind = CompressiveKind::kConstant;
        out.wire_field = 3U;
        out.has_constant_value = false;
        out.constant_value.clear();
        std::size_t p = 0;
        while (p < payload.size()) {
            std::uint64_t pkey = 0;
            if (!read_varint(payload, p, pkey)) {
                error = "page layout: malformed Constant";
                return false;
            }
            const auto pwt = static_cast<std::uint8_t>(pkey & 7U);
            if (pkey >> 3U == 1U && pwt == 2U) {
                std::uint64_t len = 0;
                if (!read_varint(payload, p, len) || len > payload.size() - p) {
                    error = "page layout: malformed Constant value";
                    return false;
                }
                const auto value = payload.subspan(p, static_cast<std::size_t>(len));
                out.has_constant_value = true;
                out.constant_value.resize(value.size());
                for (std::size_t i = 0; i < value.size(); ++i) {
                    out.constant_value[i] = std::to_integer<std::uint8_t>(value[i]);
                }
                p += value.size();
            } else if (!skip_field(payload, p, pwt)) {
                error = "page layout: malformed Constant";
                return false;
            }
        }
    }
    return true;
}

bool convert(const wire::CompressiveEncoding& w, Compressive& out, int depth, std::string& error);
bool child(const nm::pb_lazy<wire::CompressiveEncoding>& lazy, std::unique_ptr<Compressive>& out, int depth,
           std::string& error);

/// CompressiveEncoding field 13, VariablePackedStruct{ repeated FieldEncoding fields = 1 }, FieldEncoding{
/// CompressiveEncoding value = 1; oneof { uint64 bits_per_value = 2; uint64 bits_per_length = 3; } }. Not a member
/// of nanom's CompressiveEncoding, so it arrives in `unknown`; `found` says whether it was there.
bool convert_variable_packed(const nm::unknown_fields& u, Compressive& out, bool& found, int depth, std::string& error) {
    const std::span<const std::byte> all(u.bytes);
    std::size_t at = 0;
    while (at < all.size()) {
        std::uint64_t key = 0;
        if (!read_varint(all, at, key)) {
            error = "page layout: malformed CompressiveEncoding";
            return false;
        }
        const auto wt = static_cast<std::uint8_t>(key & 7U);
        if (key >> 3U != 13U || wt != 2U) {
            if (!skip_field(all, at, wt)) {
                error = "page layout: malformed CompressiveEncoding";
                return false;
            }
            continue;
        }
        std::uint64_t n = 0;
        if (!read_varint(all, at, n) || n > all.size() - at) {
            error = "page layout: malformed VariablePackedStruct";
            return false;
        }
        const auto payload = all.subspan(at, static_cast<std::size_t>(n));
        at += static_cast<std::size_t>(n);
        found = true;
        out.kind = CompressiveKind::kVariablePackedStruct;
        out.wire_field = 13U;
        out.packed_fields.clear();
        std::size_t p = 0;
        while (p < payload.size()) {
            std::uint64_t fkey = 0;
            if (!read_varint(payload, p, fkey)) {
                error = "page layout: malformed VariablePackedStruct";
                return false;
            }
            const auto fwt = static_cast<std::uint8_t>(fkey & 7U);
            if (fkey >> 3U != 1U || fwt != 2U) {
                if (!skip_field(payload, p, fwt)) {
                    error = "page layout: malformed VariablePackedStruct";
                    return false;
                }
                continue;
            }
            std::uint64_t len = 0;
            if (!read_varint(payload, p, len) || len > payload.size() - p) {
                error = "page layout: malformed VariablePackedStruct field";
                return false;
            }
            const auto field = payload.subspan(p, static_cast<std::size_t>(len));
            p += static_cast<std::size_t>(len);
            PackedField packed;
            std::size_t q = 0;
            while (q < field.size()) {
                std::uint64_t k = 0;
                if (!read_varint(field, q, k)) {
                    error = "page layout: malformed VariablePackedStruct field";
                    return false;
                }
                const auto kwt = static_cast<std::uint8_t>(k & 7U);
                if (k >> 3U == 1U && kwt == 2U) {
                    std::uint64_t vlen = 0;
                    if (!read_varint(field, q, vlen) || vlen > field.size() - q) {
                        error = "page layout: malformed VariablePackedStruct field";
                        return false;
                    }
                    nm::pb_lazy<wire::CompressiveEncoding> lazy;
                    lazy.set_region(field.data() + q, static_cast<std::size_t>(vlen));
                    q += static_cast<std::size_t>(vlen);
                    if (!child(lazy, packed.value, depth, error)) {
                        return false;
                    }
                } else if ((k >> 3U == 2U || k >> 3U == 3U) && kwt == 0U) {
                    std::uint64_t v = 0;
                    if (!read_varint(field, q, v)) {
                        error = "page layout: malformed VariablePackedStruct field";
                        return false;
                    }
                    (k >> 3U == 2U ? packed.bits_per_value : packed.bits_per_length) = v;
                } else if (!skip_field(field, q, kwt)) {
                    error = "page layout: malformed VariablePackedStruct field";
                    return false;
                }
            }
            out.packed_fields.push_back(std::move(packed));
        }
    }
    return true;
}

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
    bool constant = false;
    if (!convert_constant(*w.unknown, out, constant, error)) {
        return false;
    }
    if (constant) {
        return true;
    }
    bool packed = false;
    if (!convert_variable_packed(*w.unknown, out, packed, depth, error)) {
        return false;
    }
    if (packed) {
        return true;
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

/// One field of a protobuf message: its number, its wire type, and its value -- the varint itself, or the
/// bytes of a length-delimited field. Fixed-width fields are skipped and carry neither.
struct WireField {
    std::uint32_t number = 0;
    std::uint8_t wt = 0;
    std::uint64_t varint = 0;
    std::span<const std::byte> bytes;
};

bool next_wire_field(std::span<const std::byte> b, std::size_t& at, WireField& f, std::string& error) {
    std::uint64_t key = 0;
    f = WireField{};
    if (!read_varint(b, at, key)) {
        error = "page layout: malformed sparse layout";
        return false;
    }
    f.number = static_cast<std::uint32_t>(key >> 3U);
    f.wt = static_cast<std::uint8_t>(key & 7U);
    if (f.wt == 0U) {
        if (!read_varint(b, at, f.varint)) {
            error = "page layout: malformed sparse layout";
            return false;
        }
        return true;
    }
    if (f.wt == 2U) {
        std::uint64_t n = 0;
        if (!read_varint(b, at, n) || n > b.size() - at) {
            error = "page layout: malformed sparse layout";
            return false;
        }
        f.bytes = b.subspan(at, static_cast<std::size_t>(n));
        at += static_cast<std::size_t>(n);
        return true;
    }
    if (!skip_field(b, at, f.wt)) {
        error = "page layout: malformed sparse layout";
        return false;
    }
    return true;
}

/// A CompressiveEncoding message (a sparse set's or the value's encoding), parsed as the mini-block
/// descriptors are.
bool parse_compressive(std::span<const std::byte> b, std::unique_ptr<Compressive>& out, std::string& error) {
    nm::pb_lazy<wire::CompressiveEncoding> lazy;
    lazy.set_region(b.data(), b.size());
    if (!child(lazy, out, -1, error)) {
        return false;
    }
    if (out == nullptr) {
        error = "page layout: sparse layout has an empty encoding";
        return false;
    }
    return true;
}

/// SparsePositionSet { oneof { CompressiveEncoding explicit = 1; SparsePositionRange range = 2;
/// SparsePositionAll all = 3; SparsePositionEmpty empty = 4; } uint64 num_positions = 5; }
bool parse_positions(std::span<const std::byte> b, SparsePositions& out, std::string& error) {
    std::size_t at = 0;
    std::uint32_t variants = 0;
    while (at < b.size()) {
        WireField f;
        if (!next_wire_field(b, at, f, error)) {
            return false;
        }
        if (f.number == 1U && f.wt == 2U) {
            out.kind = SparsePositions::Kind::kExplicit;
            if (!parse_compressive(f.bytes, out.encoding, error)) {
                return false;
            }
            ++variants;
        } else if (f.number == 2U && f.wt == 2U) {
            out.kind = SparsePositions::Kind::kRange;
            std::size_t r = 0;
            while (r < f.bytes.size()) {
                WireField rf;
                if (!next_wire_field(f.bytes, r, rf, error)) {
                    return false;
                }
                if (rf.number == 1U && rf.wt == 0U) {
                    out.start = rf.varint;
                } else if (rf.number == 2U && rf.wt == 0U) {
                    out.length = rf.varint;
                }
            }
            ++variants;
        } else if (f.number == 3U && f.wt == 2U) {
            out.kind = SparsePositions::Kind::kAll;
            ++variants;
        } else if (f.number == 4U && f.wt == 2U) {
            out.kind = SparsePositions::Kind::kEmpty;
            ++variants;
        } else if (f.number == 5U && f.wt == 0U) {
            out.num_positions = f.varint;
        }
    }
    if (variants != 1U) {
        error = "page layout: sparse position set must set exactly one variant";
        return false;
    }
    return true;
}

/// SparseValiditySet { Meaning meaning = 1 (1 null positions, 2 valid positions); SparsePositionSet positions = 2 }.
bool parse_validity(std::span<const std::byte> b, SparseValidity& out, std::string& error) {
    std::size_t at = 0;
    std::uint64_t meaning = 0;
    bool have_positions = false;
    while (at < b.size()) {
        WireField f;
        if (!next_wire_field(b, at, f, error)) {
            return false;
        }
        if (f.number == 1U && f.wt == 0U) {
            meaning = f.varint;
        } else if (f.number == 2U && f.wt == 2U) {
            if (!parse_positions(f.bytes, out.positions, error)) {
                return false;
            }
            have_positions = true;
        }
    }
    if (meaning != 1U && meaning != 2U) {
        error = "page layout: sparse validity set has no meaning";
        return false;
    }
    if (!have_positions) {
        error = "page layout: sparse validity set has no positions";
        return false;
    }
    out.null_positions = meaning == 1U;
    return true;
}

/// SparseCountSet { oneof { CompressiveEncoding explicit = 1; SparseCountConstant constant = 2;
/// SparseCountEmpty empty = 3; } }.
bool parse_counts(std::span<const std::byte> b, SparseCounts& out, std::string& error) {
    std::size_t at = 0;
    std::uint32_t variants = 0;
    while (at < b.size()) {
        WireField f;
        if (!next_wire_field(b, at, f, error)) {
            return false;
        }
        if (f.number == 1U && f.wt == 2U) {
            out.kind = SparseCounts::Kind::kExplicit;
            if (!parse_compressive(f.bytes, out.encoding, error)) {
                return false;
            }
            ++variants;
        } else if (f.number == 2U && f.wt == 2U) {
            out.kind = SparseCounts::Kind::kConstant;
            std::size_t c = 0;
            while (c < f.bytes.size()) {
                WireField cf;
                if (!next_wire_field(f.bytes, c, cf, error)) {
                    return false;
                }
                if (cf.number == 1U && cf.wt == 0U) {
                    out.value = cf.varint;
                }
            }
            ++variants;
        } else if (f.number == 3U && f.wt == 2U) {
            out.kind = SparseCounts::Kind::kEmpty;
            ++variants;
        }
    }
    if (variants != 1U) {
        error = "page layout: sparse count set must set exactly one variant";
        return false;
    }
    return true;
}

/// One SparseStructuralLayer: a oneof of SparseValidityLayer / SparseListLayer / SparseFixedSizeListLayer.
bool parse_layer(std::span<const std::byte> b, SparseLayer& out, std::string& error) {
    std::size_t at = 0;
    std::uint32_t variants = 0;
    while (at < b.size()) {
        WireField f;
        if (!next_wire_field(b, at, f, error)) {
            return false;
        }
        if (f.number != 1U && f.number != 2U && f.number != 3U) {
            continue;
        }
        if (f.wt != 2U) {
            error = "page layout: malformed sparse layer";
            return false;
        }
        ++variants;
        std::size_t m = 0;
        bool have_validity = false;
        if (f.number == 1U) {
            out.kind = SparseLayer::Kind::kValidity;
        } else if (f.number == 2U) {
            out.kind = SparseLayer::Kind::kList;
        } else {
            out.kind = SparseLayer::Kind::kFixedSizeList;
        }
        while (m < f.bytes.size()) {
            WireField mf;
            if (!next_wire_field(f.bytes, m, mf, error)) {
                return false;
            }
            if (mf.wt == 0U && mf.number == 1U) {
                out.num_slots = mf.varint;
            } else if (mf.wt == 0U && mf.number == 2U && out.kind == SparseLayer::Kind::kList) {
                out.num_child_slots = mf.varint;
            } else if (mf.wt == 0U && mf.number == 2U && out.kind == SparseLayer::Kind::kFixedSizeList) {
                out.dimension = mf.varint;
            } else if (mf.wt == 2U && mf.number == 3U && out.kind == SparseLayer::Kind::kList) {
                if (!parse_positions(mf.bytes, out.non_empty, error)) {
                    return false;
                }
            } else if (mf.wt == 2U && mf.number == 4U && out.kind == SparseLayer::Kind::kList) {
                if (!parse_counts(mf.bytes, out.counts, error)) {
                    return false;
                }
            } else if (mf.wt == 2U && ((mf.number == 2U && out.kind == SparseLayer::Kind::kValidity) ||
                                       (mf.number == 5U && out.kind == SparseLayer::Kind::kList) ||
                                       (mf.number == 3U && out.kind == SparseLayer::Kind::kFixedSizeList))) {
                if (!parse_validity(mf.bytes, out.validity, error)) {
                    return false;
                }
                have_validity = true;
            }
        }
        if (!have_validity && out.kind != SparseLayer::Kind::kList) {
            error = "page layout: sparse layer has no validity set";
            return false;
        }
    }
    if (variants != 1U) {
        error = "page layout: sparse layer must set exactly one layer kind";
        return false;
    }
    return true;
}

/// SparseLayout (PageLayout field 5). Its fields: 1 value_compression, 2 num_buffers, 3 num_items,
/// 4 num_visible_items, 5 has_large_chunk, 6 structural_layers (repeated).
bool parse_sparse_layout(std::span<const std::byte> b, SparseLayout& out, std::string& error) {
    std::size_t at = 0;
    bool have_value = false;
    while (at < b.size()) {
        WireField f;
        if (!next_wire_field(b, at, f, error)) {
            return false;
        }
        if (f.number == 1U && f.wt == 2U) {
            out.value_compression_bytes.assign(reinterpret_cast<const std::uint8_t*>(f.bytes.data()),
                                               reinterpret_cast<const std::uint8_t*>(f.bytes.data()) + f.bytes.size());
            if (!parse_compressive(f.bytes, out.value_compression, error)) {
                return false;
            }
            have_value = true;
        } else if (f.number == 2U && f.wt == 0U) {
            out.num_buffers = f.varint;
        } else if (f.number == 3U && f.wt == 0U) {
            out.num_items = f.varint;
        } else if (f.number == 4U && f.wt == 0U) {
            out.num_visible_items = f.varint;
        } else if (f.number == 5U && f.wt == 0U) {
            out.has_large_chunk = f.varint != 0U;
        } else if (f.number == 6U && f.wt == 2U) {
            SparseLayer layer;
            if (!parse_layer(f.bytes, layer, error)) {
                return false;
            }
            out.layers.push_back(std::move(layer));
        }
    }
    if (!have_value) {
        error = "page layout: sparse layout has no value compression";
        return false;
    }
    // Lance 2.3 reads sparse pages only with the u32 chunk grammar (has_large_chunk); its validator
    // refuses the u16 one, and nanolance does the same rather than guess the chunk sizes.
    if (!out.has_large_chunk) {
        error = "page layout: sparse pages require the u32 chunk grammar";
        return false;
    }
    return true;
}

/// Finds PageLayout field 5 among the unknown fields. The payload stays a view into the descriptor.
bool find_sparse_payload(const nm::unknown_fields& u, std::span<const std::byte>& payload, bool& found,
                         std::string& error) {
    const std::span<const std::byte> all(u.bytes);
    std::size_t at = 0;
    while (at < all.size()) {
        WireField f;
        if (!next_wire_field(all, at, f, error)) {
            return false;
        }
        if (f.number == 5U && f.wt == 2U) {
            if (found) {
                error = "page layout: sparse layout appears twice";
                return false;
            }
            payload = f.bytes;
            found = true;
        }
    }
    return true;
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
    // Field 5, SparseLayout (file version 2.3+): read through the unknown fields, as the constant
    // and packed-struct variants of CompressiveEncoding are. Its buffers and decoding are in
    // nanolance/sparse.hpp.
    std::span<const std::byte> sparse_bytes;
    bool has_sparse = false;
    if (!find_sparse_payload(*layout.unknown, sparse_bytes, has_sparse, error)) {
        return false;
    }
    if (has_sparse) {
        out.kind = LayoutKind::kSparse;
        return parse_sparse_layout(sparse_bytes, out.sparse, error);
    }
    // Field 4 (BlobLayout) and whatever Lance adds later are parsed but not modeled: record the field
    // so a caller refuses by name rather than misreading the page's buffers.
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
        case CompressiveKind::kConstant:
            out += node->has_constant_value ? "Constant{" + std::to_string(node->constant_value.size()) + "B}"
                                            : "Constant{null}";
            return;
        case CompressiveKind::kVariablePackedStruct:
            out += "VariablePackedStruct{";
            for (std::size_t i = 0; i < node->packed_fields.size(); ++i) {
                const auto& f = node->packed_fields[i];
                out += i == 0U ? "" : ",";
                describe_compressive(f.value.get(), out);
                if (f.bits_per_length != 0U) {
                    out += "/len" + std::to_string(f.bits_per_length);
                }
            }
            out += "}";
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
        case LayoutKind::kSparse: {
            const auto& sp = layout.sparse;
            out = "Sparse{values=";
            describe_compressive(sp.value_compression.get(), out);
            for (const auto& layer : sp.layers) {
                switch (layer.kind) {
                    case SparseLayer::Kind::kValidity:
                        out += ",validity";
                        break;
                    case SparseLayer::Kind::kList:
                        out += ",list";
                        break;
                    case SparseLayer::Kind::kFixedSizeList:
                        out += ",fixed_size_list(" + std::to_string(layer.dimension) + ")";
                        break;
                }
            }
            out += ",visible=" + std::to_string(sp.num_visible_items) + "}";
            return out;
        }
        case LayoutKind::kNone:
        default:
            return layout.unknown_layout_field != 0U
                       ? "Unsupported layout (PageLayout field " + std::to_string(layout.unknown_layout_field) + ")"
                       : "none";
    }
}

}  // namespace nano_lance::page_layout
