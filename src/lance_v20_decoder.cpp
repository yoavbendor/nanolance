// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Lance file format 2.0 (see lance_v20_decoder.hpp). Byte layouts follow lance-encoding's
// `array_encoding` module (the 2.0 decoders), which Lance keeps for reading these files.

#include "nanolance/lance_v20_decoder.hpp"

#include "nanolance/column_slice.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/fastlanes_bitpack.hpp"
#include "nanolance/fsst.hpp"
#include "nanolance/lance_column_decoder.hpp"
#include "nanolance/read_safety.hpp"
#include "nanolance/schema_mapper.hpp"

#include <nanom/formats/lance_encodings.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>
#include <memory>
#include <string>
#include <vector>

namespace nano_lance::v20 {
namespace {

// ── The ArrayEncoding tree ───────────────────────────────────────────────────────────────────────

// The wire format of these trees -- lance.encodings' ArrayEncoding and its variants -- is declared in
// nanom/formats/lance_encodings.hpp and read by nanom's protobuf codec, which checks every length
// and varint. Children are pb_lazy there: each is decoded (and checked) here, one level deeper.
namespace nm = ::nanom;
namespace wire = ::nanom_formats::lance;

constexpr int kMaxDepth = 16;  // a hostile descriptor cannot recurse without bound

struct BufferRef {
    std::uint32_t index = 0;
    std::uint32_t type = 0;  // 0 page, 1 column, 2 file
    bool present = false;
};

enum class Kind {
    kNone,
    kFlat,
    kNullable,
    kFixedSizeList,
    kList,
    kStruct,
    kBinary,
    kDictionary,
    kFsst,
    kPackedStruct,
    kBitpacked,
    kFixedSizeBinary,
    kBitpackedForNonNeg,
    kConstant,
    kOther,
};

/// One node of a 2.0 ArrayEncoding: the fields of whichever variant it is.
struct Enc {
    Kind kind = Kind::kNone;
    std::uint32_t variant = 0;  // the oneof field number, for refusals by name
    // Flat
    std::uint64_t bits = 0;
    BufferRef buffer;
    std::string compression;
    // Nullable: 1 no nulls (a = values), 2 some (a = validity, b = values), 3 all
    int nulls = 0;
    std::unique_ptr<Enc> a;
    std::unique_ptr<Enc> b;
    // FixedSizeList (a = items)
    std::uint32_t dimension = 0;
    // List (a = offsets)
    std::uint64_t null_offset_adjustment = 0;
    std::uint64_t num_items = 0;
    // Binary (a = indices, b = bytes)
    std::uint64_t null_adjustment = 0;
    // Dictionary (a = indices, b = items)
    std::uint32_t num_dictionary_items = 0;
    // Fsst (a = binary)
    std::vector<std::uint8_t> symbol_table;
    // Bitpacked / BitpackedForNonNeg
    std::uint64_t compressed_bits = 0;
    std::uint64_t uncompressed_bits = 0;
    bool is_signed = false;
    // FixedSizeBinary (a = bytes)
    std::uint32_t byte_width = 0;
    // PackedStruct: each child's width in bytes (Flat children only), then `buffer`
    std::vector<std::uint32_t> packed_widths;
};

bool parse_enc(const wire::ArrayEncoding& w, Enc& out, int depth, std::string& error);

bool parse_child(const nm::pb_lazy<wire::ArrayEncoding>& lazy, std::unique_ptr<Enc>& out, int depth,
                 std::string& error) {
    if (!lazy) {
        return true;
    }
    wire::ArrayEncoding w;
    if (!lazy.decode_into(w)) {
        error = "malformed format 2.0 encoding";
        return false;
    }
    out = std::make_unique<Enc>();
    return parse_enc(w, *out, depth + 1, error);
}

BufferRef buffer_of(const std::optional<wire::Buffer>& b) {
    BufferRef out;
    if (b) {
        out.present = true;
        out.index = static_cast<std::uint32_t>(*b->buffer_index);
        out.type = static_cast<std::uint32_t>(*b->buffer_type);
    }
    return out;
}

/// The variant a oneof message sets that this build does not model: the field number of the first
/// length-delimited record kept in its pb_unknown.
std::uint32_t unknown_variant(const nm::unknown_fields& u) {
    std::uint32_t field = 0;
    nm::for_each_unknown(u, [&](std::uint64_t f, std::uint8_t wire_type) {
        if (field == 0U && wire_type == 2U) {
            field = static_cast<std::uint32_t>(f);
        }
    });
    return field;
}

const char* variant_name(std::uint32_t field) {
    static const char* const kNames[] = {"none",          "Flat",          "Nullable",
                                         "FixedSizeList", "List",          "SimpleStruct",
                                         "Binary",        "Dictionary",    "Fsst",
                                         "PackedStruct",  "Bitpacked",     "FixedSizeBinary",
                                         "BitpackedForNonNeg", "Constant", "InlineBitpacking",
                                         "OutOfLineBitpacking", "Variable", "PackedStructFixedWidthMiniBlock",
                                         "Block",         "Rle",           "GeneralMiniBlock",
                                         "ByteStreamSplit"};
    return field < sizeof(kNames) / sizeof(kNames[0]) ? kNames[field] : "unknown";
}

bool parse_enc(const wire::ArrayEncoding& w, Enc& out, int depth, std::string& error) {
    if (depth > kMaxDepth) {
        error = "format 2.0 encoding nests too deeply";
        return false;
    }
    const auto members = wire::oneof_members(w);
    if (members > 1U) {
        error = "malformed format 2.0 encoding (more than one variant)";
        return false;
    }
    bool ok = true;
    if (members == 0U) {
        out.kind = Kind::kNone;
    } else if (const auto& v = *w.flat) {
        out.variant = 1U;
        out.kind = Kind::kFlat;
        out.bits = *v->bits_per_value;
        out.buffer = buffer_of(*v->buffer);
        if (const auto& c = *v->compression) {
            out.compression = std::string(*c->scheme);
        }
    } else if (const auto& v = *w.nullable) {  // oneof { no_nulls = 1, some_nulls = 2, all_nulls = 3 }
        out.variant = 2U;
        out.kind = Kind::kNullable;
        const auto nulls = wire::oneof_members(*v);
        if (nulls > 1U) {
            ok = false;
        } else if (const auto& n = *v->no_nulls) {
            out.nulls = 1;
            ok = parse_child(*n->values, out.a, depth, error);
        } else if (const auto& n = *v->some_nulls) {
            out.nulls = 2;
            ok = parse_child(*n->validity, out.a, depth, error) && parse_child(*n->values, out.b, depth, error);
        } else if (*v->all_nulls) {
            out.nulls = 3;
        } else {
            out.nulls = static_cast<int>(unknown_variant(*v->unknown));  // refused downstream, by number
        }
    } else if (const auto& v = *w.fixed_size_list) {
        out.variant = 3U;
        out.kind = Kind::kFixedSizeList;
        out.dimension = static_cast<std::uint32_t>(*v->dimension);
        ok = parse_child(*v->items, out.a, depth, error);
    } else if (const auto& v = *w.list) {
        out.variant = 4U;
        out.kind = Kind::kList;
        out.null_offset_adjustment = *v->null_offset_adjustment;
        out.num_items = *v->num_items;
        ok = parse_child(*v->offsets, out.a, depth, error);
    } else if (*w.struct_) {
        out.variant = 5U;
        out.kind = Kind::kStruct;
    } else if (const auto& v = *w.binary) {
        out.variant = 6U;
        out.kind = Kind::kBinary;
        out.null_adjustment = *v->null_adjustment;
        ok = parse_child(*v->indices, out.a, depth, error) && parse_child(*v->bytes, out.b, depth, error);
    } else if (const auto& v = *w.dictionary) {
        out.variant = 7U;
        out.kind = Kind::kDictionary;
        out.num_dictionary_items = static_cast<std::uint32_t>(*v->num_dictionary_items);
        ok = parse_child(*v->indices, out.a, depth, error) && parse_child(*v->items, out.b, depth, error);
    } else if (const auto& v = *w.fsst) {
        out.variant = 8U;
        out.kind = Kind::kFsst;
        const auto* table = reinterpret_cast<const std::uint8_t*>(v->symbol_table->data());
        out.symbol_table.assign(table, table + v->symbol_table->size());
        ok = parse_child(*v->binary, out.a, depth, error);
    } else if (const auto& v = *w.packed_struct) {
        out.variant = 9U;
        out.kind = Kind::kPackedStruct;
        out.buffer = buffer_of(*v->buffer);
        for (const auto& inner : *v->inner) {
            Enc child;
            if (!parse_enc(inner, child, depth + 1, error)) {
                return false;
            }
            // Lance packs fixed-width children only -- a number or a fixed-size list of them, never
            // null, so a nullable wrapper adds nothing here.
            const Enc* flat = &child;
            while (flat->kind == Kind::kNullable && flat->nulls == 1 && flat->a) flat = flat->a.get();
            std::uint64_t items = 1;
            if (flat->kind == Kind::kFixedSizeList && flat->a) {
                items = flat->dimension;
                flat = flat->a.get();
                while (flat->kind == Kind::kNullable && flat->nulls == 1 && flat->a) flat = flat->a.get();
            }
            if (flat->kind != Kind::kFlat || flat->bits == 0U || flat->bits % 8U != 0U || items == 0U ||
                items > 65536U || flat->bits > 1024U) {
                error = "format 2.0 packed struct with a child that is not fixed-width";
                return false;
            }
            out.packed_widths.push_back(static_cast<std::uint32_t>(items * (flat->bits / 8U)));
        }
    } else if (const auto& v = *w.bitpacked) {
        out.variant = 10U;
        out.kind = Kind::kBitpacked;
        out.compressed_bits = *v->compressed_bits_per_value;
        out.uncompressed_bits = *v->uncompressed_bits_per_value;
        out.buffer = buffer_of(*v->buffer);
        out.is_signed = *v->signed_ != 0U;
    } else if (const auto& v = *w.fixed_size_binary) {
        out.variant = 11U;
        out.kind = Kind::kFixedSizeBinary;
        out.byte_width = static_cast<std::uint32_t>(*v->byte_width);
        ok = parse_child(*v->bytes, out.a, depth, error);
    } else if (const auto& v = *w.bitpacked_for_non_neg) {
        out.variant = 12U;
        out.kind = Kind::kBitpackedForNonNeg;
        out.compressed_bits = *v->compressed_bits_per_value;
        out.uncompressed_bits = *v->uncompressed_bits_per_value;
        out.buffer = buffer_of(*v->buffer);
    } else if (*w.constant) {
        out.variant = 13U;
        out.kind = Kind::kConstant;
    } else {
        out.variant = unknown_variant(*w.unknown);
        out.kind = Kind::kOther;
    }
    if (!ok) {
        if (error.empty()) {
            error = std::string("malformed format 2.0 ") + variant_name(out.variant) + " encoding";
        }
        return false;
    }
    return true;
}

/// A page's (or column's) encoding: Any { type_url = 1, value = 2 } around the ArrayEncoding.
bool parse_page_encoding(const std::vector<std::uint8_t>& any_bytes, Enc& out, std::string& error) {
    wire::EncodingAny any;
    const auto in = nm::from(std::span<const std::byte>(reinterpret_cast<const std::byte*>(any_bytes.data()),
                                                        any_bytes.size()));
    if (!nm::protobuf_decode(in, any)) {
        error = "malformed page encoding";
        return false;
    }
    if (*any.type_url != "/lance.encodings.ArrayEncoding") {
        error = "not a format 2.0 page (encoding '" + std::string(*any.type_url) + "')";
        return false;
    }
    if (!*any.value) {
        out.kind = Kind::kNone;
        return true;
    }
    wire::ArrayEncoding w;
    if (!nm::protobuf_decode(nm::from(**any.value), w)) {
        error = "malformed format 2.0 encoding";
        return false;
    }
    return parse_enc(w, out, 0, error);
}

// ── Decoded arrays ───────────────────────────────────────────────────────────────────────────────

/// What a node decodes to, before it becomes a column's values.
struct Arr {
    enum class Shape { kFixed, kBits, kVariable, kAllNull } shape = Shape::kFixed;
    std::uint64_t n = 0;
    std::uint32_t width = 0;              // kFixed: bytes per value (a fixed_size_list row: all of it)
    std::vector<std::uint8_t> bytes;      // kFixed values, kBits bitmap (LSB first), kVariable data
    std::vector<std::uint64_t> offsets;   // kVariable: n + 1, from 0
    std::vector<std::uint8_t> validity;   // per value, LSB first; empty when every value is valid
    std::uint64_t nulls = 0;
    std::uint32_t dimension = 0;          // a fixed_size_list: its items per row
    std::vector<std::uint8_t> item_validity;
    std::uint64_t item_nulls = 0;
    std::vector<std::uint32_t> packed;    // a packed struct: its children's widths
};

bool bit_at(const std::vector<std::uint8_t>& bits, std::uint64_t i) {
    return ((bits[static_cast<std::size_t>(i >> 3U)] >> (i & 7U)) & 1U) != 0U;
}

std::uint64_t count_clear(const std::vector<std::uint8_t>& bits, std::uint64_t n) {
    std::uint64_t clear = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        clear += bit_at(bits, i) ? 0U : 1U;
    }
    return clear;
}

/// A page's buffers (and its column's). A compressed buffer, or one read whole, is read once and
/// kept; a part of an uncompressed one is read on its own, so a range or a take reads only the bytes
/// its rows need.
struct PageReader {
    const std::filesystem::path& path;
    const pb::ColumnPage& page;
    const pb::ColumnMetadata& column;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::string>, std::vector<std::uint8_t>> whole;

    PageReader(const std::filesystem::path& p, const pb::ColumnPage& pg, const pb::ColumnMetadata& c)
        : path(p), page(pg), column(c) {}

    bool locate(const BufferRef& ref, std::uint64_t& offset, std::uint64_t& size, std::string& error) const {
        if (!ref.present) {
            error = "format 2.0 value encoding names no buffer";
            return false;
        }
        const auto* offsets = ref.type == 0U ? &page.buffer_offsets : ref.type == 1U ? &column.buffer_offsets : nullptr;
        const auto* sizes = ref.type == 0U ? &page.buffer_sizes : ref.type == 1U ? &column.buffer_sizes : nullptr;
        if (offsets == nullptr) {
            error = "format 2.0 file-level buffers are not read";
            return false;
        }
        if (ref.index >= offsets->size() || ref.index >= sizes->size()) {
            error = std::string("format 2.0 encoding names ") + (ref.type == 0U ? "page" : "column") + " buffer " +
                    std::to_string(ref.index) + " of " + std::to_string(offsets->size());
            return false;
        }
        offset = (*offsets)[ref.index];
        size = (*sizes)[ref.index];
        return true;
    }

    /// The whole buffer, decompressed when `compression` names a scheme.
    bool get(const BufferRef& ref, const std::string& compression, const std::vector<std::uint8_t>*& out,
             std::string& error) {
        const auto key = std::make_tuple(ref.type, ref.index, compression);
        if (const auto it = whole.find(key); it != whole.end()) {
            out = &it->second;
            return true;
        }
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::vector<std::uint8_t> raw;
        if (!locate(ref, offset, size, error) || !read_lance_data_file_bytes(path, offset, size, raw, error)) {
            return false;
        }
        auto& slot = whole[key];
        if (compression.empty() || compression == "none") {
            slot = std::move(raw);
        } else if (!decompress_general_buffer(compression, raw.data(), raw.size(), slot, error)) {
            whole.erase(key);
            return false;
        }
        out = &slot;
        return true;
    }

    /// Bytes [at, at + len) of an uncompressed buffer.
    bool get_range(const BufferRef& ref, std::uint64_t at, std::uint64_t len, std::vector<std::uint8_t>& out,
                   std::string& error) {
        if (const auto it = whole.find(std::make_tuple(ref.type, ref.index, std::string())); it != whole.end()) {
            if (at > it->second.size() || len > it->second.size() - at) {
                error = "format 2.0 buffer is shorter than its values";
                return false;
            }
            out.assign(it->second.begin() + static_cast<std::ptrdiff_t>(at),
                       it->second.begin() + static_cast<std::ptrdiff_t>(at + len));
            return true;
        }
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        if (!locate(ref, offset, size, error)) {
            return false;
        }
        if (at > size || len > size - at) {
            error = "format 2.0 buffer of " + std::to_string(size) + " bytes is shorter than its values (" +
                    std::to_string(at) + "+" + std::to_string(len) + ")";
            return false;
        }
        if (len == 0U) {
            out.clear();
            return true;
        }
        return read_lance_data_file_bytes(path, offset + at, len, out, error);
    }
};

/// Values [start, start + count) of a node.
bool decode(const Enc& enc, std::uint64_t start, std::uint64_t count, PageReader& io, Arr& out, std::string& error);

/// Values of an index or offset array, as u64 (fixed-width unsigned, 1 to 8 bytes).
bool as_u64(const Arr& a, std::vector<std::uint64_t>& out, std::string& error) {
    if (a.shape == Arr::Shape::kAllNull) {
        out.assign(static_cast<std::size_t>(a.n), 0U);
        return true;
    }
    if (a.shape != Arr::Shape::kFixed || a.width == 0U || a.width > 8U || a.dimension != 0U ||
        a.bytes.size() < a.n * a.width) {
        error = "format 2.0 offsets or indices are not fixed-width integers";
        return false;
    }
    out.resize(static_cast<std::size_t>(a.n));
    for (std::uint64_t i = 0; i < a.n; ++i) {
        std::uint64_t v = 0;
        std::memcpy(&v, a.bytes.data() + i * a.width, a.width);  // little-endian
        out[static_cast<std::size_t>(i)] = v;
    }
    return true;
}

/// `count` bits of `src` from bit `from`, as a bitmap starting at bit 0.
std::vector<std::uint8_t> shift_bits(const std::uint8_t* src, std::uint64_t from, std::uint64_t count) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>((count + 7U) / 8U), 0U);
    if (from % 8U == 0U) {
        std::memcpy(out.data(), src + from / 8U, out.size());
        if (count % 8U != 0U) {
            out.back() &= static_cast<std::uint8_t>((1U << (count % 8U)) - 1U);
        }
        return out;
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto b = from + i;
        if (((src[static_cast<std::size_t>(b >> 3U)] >> (b & 7U)) & 1U) != 0U) {
            out[static_cast<std::size_t>(i >> 3U)] |= static_cast<std::uint8_t>(1U << (i & 7U));
        }
    }
    return out;
}

bool decode_flat(const Enc& enc, std::uint64_t start, std::uint64_t count, PageReader& io, Arr& out,
                 std::string& error) {
    out.n = count;
    const bool compressed = !enc.compression.empty() && enc.compression != "none";
    if (enc.bits == 1U) {
        out.shape = Arr::Shape::kBits;
        const auto first_byte = start / 8U;
        const auto end_byte = (start + count + 7U) / 8U;
        std::vector<std::uint8_t> part;
        const std::vector<std::uint8_t>* raw = nullptr;
        if (compressed) {
            if (!io.get(enc.buffer, enc.compression, raw, error)) return false;
            if (raw->size() < end_byte) {
                error = "format 2.0 bitmap is shorter than its values";
                return false;
            }
            out.bytes = shift_bits(raw->data(), start, count);
            return true;
        }
        if (!io.get_range(enc.buffer, first_byte, end_byte - first_byte, part, error)) return false;
        out.bytes = shift_bits(part.data(), start - first_byte * 8U, count);
        return true;
    }
    if (enc.bits == 0U || enc.bits % 8U != 0U || enc.bits > 8U * 4096U) {
        error = "format 2.0 Flat values of " + std::to_string(enc.bits) + " bits are not read";
        return false;
    }
    out.shape = Arr::Shape::kFixed;
    out.width = static_cast<std::uint32_t>(enc.bits / 8U);
    std::uint64_t at = 0;
    std::uint64_t len = 0;
    if (!checked_mul(start, static_cast<std::uint64_t>(out.width), at) ||
        !checked_mul(count, static_cast<std::uint64_t>(out.width), len) ||
        len > default_read_limits().max_uncompressed_bytes) {
        error = "format 2.0 Flat values are too large";
        return false;
    }
    if (!compressed) {
        return io.get_range(enc.buffer, at, len, out.bytes, error);
    }
    const std::vector<std::uint8_t>* raw = nullptr;
    if (!io.get(enc.buffer, enc.compression, raw, error)) return false;
    if (at > raw->size() || len > raw->size() - at) {
        error = "format 2.0 Flat buffer holds " + std::to_string(raw->size()) + " bytes, too few for its values";
        return false;
    }
    out.bytes.assign(raw->begin() + static_cast<std::ptrdiff_t>(at), raw->begin() + static_cast<std::ptrdiff_t>(at + len));
    return true;
}

/// Lance's first bit-packing: every value's low `compressed_bits`, back to back, LSB first; a signed
/// value's top packed bit is its sign, extended over the uncompressed width.
bool decode_bitpacked(const Enc& enc, std::uint64_t start, std::uint64_t count, PageReader& io, Arr& out,
                      std::string& error) {
    const std::uint64_t bits = enc.compressed_bits;
    const std::uint64_t width = enc.uncompressed_bits / 8U;
    if (bits == 0U || bits > 64U || enc.uncompressed_bits % 8U != 0U || width == 0U || width > 8U ||
        bits > enc.uncompressed_bits) {
        error = "format 2.0 Bitpacked of " + std::to_string(bits) + " in " + std::to_string(enc.uncompressed_bits) +
                " bits is not read";
        return false;
    }
    std::uint64_t first_bit = 0;
    std::uint64_t end_bit = 0;
    if (!checked_mul(start, bits, first_bit) || !checked_mul(start + count, bits, end_bit)) {
        error = "format 2.0 Bitpacked page is too large";
        return false;
    }
    const auto first_byte = first_bit / 8U;
    std::vector<std::uint8_t> raw;
    if (!io.get_range(enc.buffer, first_byte, (end_bit + 7U) / 8U - first_byte, raw, error)) {
        return false;
    }
    out.shape = Arr::Shape::kFixed;
    out.n = count;
    out.width = static_cast<std::uint32_t>(width);
    out.bytes.assign(static_cast<std::size_t>(count * width), 0U);
    const std::uint64_t mask = bits == 64U ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1U);
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t at = first_bit - first_byte * 8U + i * bits;
        std::uint64_t value = 0;
        std::uint64_t got = 0;
        while (got < bits) {
            const std::uint64_t pos = at + got;
            const std::uint64_t in_byte = pos & 7U;
            const std::uint64_t take = std::min<std::uint64_t>(8U - in_byte, bits - got);
            const std::uint64_t chunk = (static_cast<std::uint64_t>(raw[static_cast<std::size_t>(pos >> 3U)]) >> in_byte) &
                                        ((std::uint64_t{1} << take) - 1U);
            value |= chunk << got;
            got += take;
        }
        value &= mask;
        if (enc.is_signed && bits < 64U && ((value >> (bits - 1U)) & 1U) != 0U) {
            value |= ~mask;
        }
        std::memcpy(out.bytes.data() + i * width, &value, static_cast<std::size_t>(width));
    }
    return true;
}

template <class T>
void unpack_chunks(const std::uint8_t* raw, unsigned bits, std::uint64_t skip, std::uint64_t count, std::uint8_t* dst) {
    const std::size_t words = fastlanes::packed_words_1024<T>(bits);
    std::vector<T> in(words == 0U ? 1U : words);
    std::vector<T> chunk(1024U);
    const std::size_t chunk_bytes = words * sizeof(T);
    std::uint64_t done = 0;
    for (std::uint64_t c = 0; done < count; ++c) {
        if (chunk_bytes != 0U) {
            std::memcpy(in.data(), raw + c * chunk_bytes, chunk_bytes);
        }
        fastlanes::unpack_1024<T>(bits, in.data(), chunk.data());
        const auto from = c == 0U ? skip : 0U;
        const auto take = std::min<std::uint64_t>(1024U - from, count - done);
        std::memcpy(dst + done * sizeof(T), chunk.data() + from, static_cast<std::size_t>(take) * sizeof(T));
        done += take;
    }
}

/// FastLanes chunks of 1024 values at a fixed width (what 2.1's InlineBitpacking packs, without the
/// per-chunk width); the last chunk is padded to 1024.
bool decode_bitpacked_non_neg(const Enc& enc, std::uint64_t start, std::uint64_t count, PageReader& io, Arr& out,
                              std::string& error) {
    const auto ubits = enc.uncompressed_bits;
    const auto bits = enc.compressed_bits;
    if ((ubits != 8U && ubits != 16U && ubits != 32U && ubits != 64U) || bits > ubits) {
        error = "format 2.0 BitpackedForNonNeg of " + std::to_string(bits) + " in " + std::to_string(ubits) +
                " bits is not read";
        return false;
    }
    out.shape = Arr::Shape::kFixed;
    out.n = count;
    out.width = static_cast<std::uint32_t>(ubits / 8U);
    if (count == 0U) {
        return true;
    }
    const std::uint64_t chunk_bytes = 1024U * bits / 8U;
    const std::uint64_t c0 = start / 1024U;
    const std::uint64_t c1 = (start + count - 1U) / 1024U;
    std::vector<std::uint8_t> raw;
    if (!io.get_range(enc.buffer, c0 * chunk_bytes, (c1 - c0 + 1U) * chunk_bytes, raw, error)) {
        return false;
    }
    out.bytes.assign(static_cast<std::size_t>(count * out.width), 0U);
    const auto b = static_cast<unsigned>(bits);
    const auto skip = start % 1024U;
    switch (ubits) {
        case 8: unpack_chunks<std::uint8_t>(raw.data(), b, skip, count, out.bytes.data()); break;
        case 16: unpack_chunks<std::uint16_t>(raw.data(), b, skip, count, out.bytes.data()); break;
        case 32: unpack_chunks<std::uint32_t>(raw.data(), b, skip, count, out.bytes.data()); break;
        default: unpack_chunks<std::uint64_t>(raw.data(), b, skip, count, out.bytes.data()); break;
    }
    return true;
}

void mark_null(Arr& out, std::uint64_t i) {
    if (out.validity.empty()) {
        out.validity.assign(static_cast<std::size_t>((out.n + 7U) / 8U), 0xFFU);
    }
    out.validity[static_cast<std::size_t>(i >> 3U)] &= static_cast<std::uint8_t>(~(1U << (i & 7U)));
    ++out.nulls;
}

/// Binary: end offsets (a null's raised by `null_adjustment`) and the bytes. Rows from `start` need
/// the end offset before them too: where their bytes begin.
bool decode_binary(const Enc& enc, std::uint64_t start, std::uint64_t count, PageReader& io, Arr& out,
                   std::string& error) {
    if (!enc.a || !enc.b) {
        error = "format 2.0 Binary encoding is missing its indices or bytes";
        return false;
    }
    out.shape = Arr::Shape::kVariable;
    out.n = count;
    out.offsets.assign(1, 0U);
    if (count == 0U) {
        return true;
    }
    const bool lead = start != 0U;
    Arr indices;
    std::vector<std::uint64_t> raw;
    if (!decode(*enc.a, lead ? start - 1U : 0U, count + (lead ? 1U : 0U), io, indices, error) ||
        !as_u64(indices, raw, error)) {
        return false;
    }
    const auto adjustment = enc.null_adjustment;
    const auto norm = [&](std::uint64_t v) { return v >= adjustment ? v - adjustment : v; };
    const std::uint64_t base = lead ? norm(raw[0]) : 0U;
    out.offsets.reserve(static_cast<std::size_t>(count) + 1U);
    std::uint64_t prev = base;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto v = raw[static_cast<std::size_t>(i + (lead ? 1U : 0U))];
        const auto end = norm(v);
        if (end < prev) {
            error = "corrupt format 2.0 Binary page: offset " + std::to_string(end) + " after " + std::to_string(prev);
            return false;
        }
        if (v >= adjustment) {
            mark_null(out, i);
        }
        out.offsets.push_back(end - base);
        prev = end;
    }
    if (prev - base > default_read_limits().max_uncompressed_bytes) {
        error = "format 2.0 Binary page exceeds the decoded-size limit";
        return false;
    }
    Arr bytes;
    if (!decode(*enc.b, base, prev - base, io, bytes, error)) {
        return false;
    }
    if (bytes.shape != Arr::Shape::kFixed || bytes.width != 1U || bytes.bytes.size() != prev - base) {
        error = "format 2.0 Binary bytes are not a byte buffer";
        return false;
    }
    out.bytes = std::move(bytes.bytes);
    return true;
}

bool decode_fsst(const Enc& enc, std::uint64_t start, std::uint64_t count, PageReader& io, Arr& out,
                 std::string& error) {
    if (!enc.a) {
        error = "format 2.0 Fsst encoding has no inner encoding";
        return false;
    }
    fsst::SymbolTable table;
    if (!fsst::parse_symbol_table(enc.symbol_table, table, error)) {
        return false;
    }
    Arr compressed;
    if (!decode(*enc.a, start, count, io, compressed, error)) {
        return false;
    }
    if (compressed.shape != Arr::Shape::kVariable) {
        error = "format 2.0 Fsst values are not variable-width";
        return false;
    }
    out = Arr{};
    out.shape = Arr::Shape::kVariable;
    out.n = count;
    out.validity = std::move(compressed.validity);
    out.nulls = compressed.nulls;
    out.offsets.assign(1, 0U);
    out.offsets.reserve(static_cast<std::size_t>(count) + 1U);
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto a = compressed.offsets[static_cast<std::size_t>(i)];
        const auto b = compressed.offsets[static_cast<std::size_t>(i) + 1U];
        if (!fsst::decompress_value(table, compressed.bytes.data() + a, static_cast<std::size_t>(b - a), out.bytes,
                                    error)) {
            return false;
        }
        if (out.bytes.size() > default_read_limits().max_uncompressed_bytes) {
            error = "format 2.0 Fsst page exceeds the decoded-size limit";
            return false;
        }
        out.offsets.push_back(out.bytes.size());
    }
    return true;
}

/// Dictionary: item i + 1 per row, 0 for a null (lance-encoding's DictionaryPageDecoder).
bool decode_dictionary(const Enc& enc, std::uint64_t start, std::uint64_t count, PageReader& io, Arr& out,
                       std::string& error) {
    if (!enc.a || !enc.b) {
        error = "format 2.0 Dictionary encoding is missing its indices or items";
        return false;
    }
    Arr items;
    if (!decode(*enc.b, 0, enc.num_dictionary_items, io, items, error)) {
        return false;
    }
    Arr indices_arr;
    std::vector<std::uint64_t> indices;
    if (!decode(*enc.a, start, count, io, indices_arr, error) || !as_u64(indices_arr, indices, error)) {
        return false;
    }
    out = Arr{};
    out.n = count;
    if (items.shape == Arr::Shape::kVariable) {
        out.shape = Arr::Shape::kVariable;
        out.offsets.assign(1, 0U);
        out.offsets.reserve(static_cast<std::size_t>(count) + 1U);
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto k = indices[static_cast<std::size_t>(i)];
            if (k == 0U) {
                mark_null(out, i);
            } else if (k > items.n) {
                error = "format 2.0 dictionary index " + std::to_string(k - 1U) + " past its " +
                        std::to_string(items.n) + " items";
                return false;
            } else {
                const auto a = items.offsets[static_cast<std::size_t>(k - 1U)];
                const auto b = items.offsets[static_cast<std::size_t>(k)];
                out.bytes.insert(out.bytes.end(), items.bytes.begin() + static_cast<std::ptrdiff_t>(a),
                                 items.bytes.begin() + static_cast<std::ptrdiff_t>(b));
                if (out.bytes.size() > default_read_limits().max_uncompressed_bytes) {
                    error = "format 2.0 dictionary page exceeds the decoded-size limit";
                    return false;
                }
            }
            out.offsets.push_back(out.bytes.size());
        }
        return true;
    }
    if (items.shape == Arr::Shape::kFixed && items.dimension == 0U) {
        out.shape = Arr::Shape::kFixed;
        out.width = items.width;
        out.bytes.assign(static_cast<std::size_t>(count * items.width), 0U);
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto k = indices[static_cast<std::size_t>(i)];
            if (k == 0U) {
                mark_null(out, i);
            } else if (k > items.n) {
                error = "format 2.0 dictionary index past its items";
                return false;
            } else {
                std::memcpy(out.bytes.data() + i * items.width, items.bytes.data() + (k - 1U) * items.width,
                            items.width);
            }
        }
        return true;
    }
    error = "format 2.0 dictionary of this item type is not read";
    return false;
}

bool decode(const Enc& enc, std::uint64_t start, std::uint64_t count, PageReader& io, Arr& out, std::string& error) {
    out = Arr{};
    switch (enc.kind) {
        case Kind::kFlat:
            return decode_flat(enc, start, count, io, out, error);
        case Kind::kNullable: {
            if (enc.nulls == 3) {
                out.shape = Arr::Shape::kAllNull;
                out.n = count;
                out.nulls = count;
                return true;
            }
            if (enc.nulls == 1 && enc.a) {
                return decode(*enc.a, start, count, io, out, error);
            }
            if (enc.nulls == 2 && enc.a && enc.b) {
                Arr validity;
                if (!decode(*enc.a, start, count, io, validity, error)) {
                    return false;
                }
                if (validity.shape != Arr::Shape::kBits) {
                    error = "format 2.0 validity is not a bitmap";
                    return false;
                }
                if (!decode(*enc.b, start, count, io, out, error)) {
                    return false;
                }
                if (out.shape == Arr::Shape::kAllNull) {
                    return true;
                }
                if (out.validity.empty()) {
                    out.validity = std::move(validity.bytes);
                } else {
                    for (std::size_t i = 0; i < out.validity.size(); ++i) {
                        out.validity[i] &= validity.bytes[i];
                    }
                }
                out.nulls = count_clear(out.validity, count);
                if (out.nulls == 0U) {
                    out.validity.clear();
                }
                return true;
            }
            error = "malformed format 2.0 Nullable encoding";
            return false;
        }
        case Kind::kBitpacked:
            return decode_bitpacked(enc, start, count, io, out, error);
        case Kind::kBitpackedForNonNeg:
            return decode_bitpacked_non_neg(enc, start, count, io, out, error);
        case Kind::kFixedSizeList: {
            if (!enc.a || enc.dimension == 0U) {
                error = "malformed format 2.0 FixedSizeList encoding";
                return false;
            }
            std::uint64_t first_item = 0;
            std::uint64_t items = 0;
            if (!checked_mul(start, static_cast<std::uint64_t>(enc.dimension), first_item) ||
                !checked_mul(count, static_cast<std::uint64_t>(enc.dimension), items)) {
                error = "format 2.0 FixedSizeList is too large";
                return false;
            }
            Arr child;
            if (!decode(*enc.a, first_item, items, io, child, error)) {
                return false;
            }
            if (child.shape == Arr::Shape::kAllNull) {
                error = "format 2.0 fixed_size_list whose every item is null is not read";
                return false;
            }
            if (child.shape != Arr::Shape::kFixed || child.dimension != 0U) {
                error = "format 2.0 fixed_size_list of this item type is not read";
                return false;
            }
            out.shape = Arr::Shape::kFixed;
            out.n = count;
            out.width = child.width * enc.dimension;
            out.dimension = enc.dimension;
            out.bytes = std::move(child.bytes);
            out.item_validity = std::move(child.validity);
            out.item_nulls = child.nulls;
            return true;
        }
        case Kind::kBinary:
            return decode_binary(enc, start, count, io, out, error);
        case Kind::kFsst:
            return decode_fsst(enc, start, count, io, out, error);
        case Kind::kDictionary:
            return decode_dictionary(enc, start, count, io, out, error);
        case Kind::kFixedSizeBinary: {
            if (!enc.a || enc.byte_width == 0U) {
                error = "malformed format 2.0 FixedSizeBinary encoding";
                return false;
            }
            Arr bytes;
            std::uint64_t first = 0;
            std::uint64_t total = 0;
            if (!checked_mul(start, static_cast<std::uint64_t>(enc.byte_width), first) ||
                !checked_mul(count, static_cast<std::uint64_t>(enc.byte_width), total)) {
                error = "format 2.0 FixedSizeBinary is too large";
                return false;
            }
            if (!decode(*enc.a, first, total, io, bytes, error)) {
                return false;
            }
            if (bytes.shape != Arr::Shape::kFixed || bytes.width != 1U) {
                error = "format 2.0 FixedSizeBinary bytes are not a byte buffer";
                return false;
            }
            out.shape = Arr::Shape::kVariable;
            out.n = count;
            out.bytes = std::move(bytes.bytes);
            out.offsets.resize(static_cast<std::size_t>(count) + 1U);
            for (std::uint64_t i = 0; i <= count; ++i) {
                out.offsets[static_cast<std::size_t>(i)] = i * enc.byte_width;
            }
            return true;
        }
        case Kind::kPackedStruct: {
            // Rows of every child's bytes back to back; returned as one fixed-width value per row
            // (only a blob column's (position, size) descriptions are read from it).
            std::uint64_t width = 0;
            for (const auto w : enc.packed_widths) width += w;
            std::uint64_t at = 0;
            std::uint64_t len = 0;
            if (width == 0U || !checked_mul(start, width, at) || !checked_mul(count, width, len)) {
                error = "format 2.0 packed struct with no children";
                return false;
            }
            out.shape = Arr::Shape::kFixed;
            out.n = count;
            out.width = static_cast<std::uint32_t>(width);
            out.packed = enc.packed_widths;
            return io.get_range(enc.buffer, at, len, out.bytes, error);
        }
        case Kind::kStruct:
        case Kind::kList:
            error = std::string("format 2.0 ") + variant_name(enc.variant) + " page where values were expected";
            return false;
        case Kind::kConstant:
            error = "format 2.0 Constant pages are not read (Lance does not read them either)";
            return false;
        case Kind::kNone:
        case Kind::kOther:
            break;
    }
    error = std::string("format 2.0 ") + variant_name(enc.variant) + " encoding is not read";
    return false;
}

// ── Into ColumnValues ────────────────────────────────────────────────────────────────────────────

/// Append `n` validity bits (empty `bits`: all valid) at row `at` of `out`'s bitmap, which stays
/// empty until the first null.
void append_bits(std::vector<std::uint8_t>& bitmap, std::uint64_t& null_count, std::uint64_t at,
                 const std::vector<std::uint8_t>* bits, std::uint64_t n, bool all_null) {
    const bool any_null = all_null || (bits != nullptr && !bits->empty() && count_clear(*bits, n) != 0U);
    if (bitmap.empty() && !any_null) {
        return;
    }
    if (bitmap.empty()) {
        bitmap.assign(static_cast<std::size_t>((at + 7U) / 8U), 0U);
        for (std::uint64_t i = 0; i < at; ++i) {
            bitmap[static_cast<std::size_t>(i >> 3U)] |= static_cast<std::uint8_t>(1U << (i & 7U));
        }
    }
    bitmap.resize(static_cast<std::size_t>((at + n + 7U) / 8U), 0U);
    for (std::uint64_t i = 0; i < n; ++i) {
        const bool valid = !all_null && (bits == nullptr || bits->empty() || bit_at(*bits, i));
        const auto r = at + i;
        if (valid) {
            bitmap[static_cast<std::size_t>(r >> 3U)] |= static_cast<std::uint8_t>(1U << (r & 7U));
        } else {
            bitmap[static_cast<std::size_t>(r >> 3U)] &= static_cast<std::uint8_t>(~(1U << (r & 7U)));
            ++null_count;
        }
    }
}

struct LeafType {
    bool variable = false;
    bool large = false;
    bool boolean = false;
    bool null_type = false;   // Arrow's null type: every page is all-null
    std::uint64_t items = 0;  // fixed_size_list
    std::size_t value_bytes = 0;
};

bool leaf_type(const pb::Field& field, LeafType& t, std::string& error) {
    const auto& type = field.logical_type;
    if (lance_field_is_variable_width(type)) {
        t.variable = true;
        t.large = lance_logical_type_has_large_offsets(type);
        return true;
    }
    std::string element;
    if (lance_fixed_size_list_parts(type, element, t.items)) {
        if (element == "bool" || lance_field_is_variable_width(element)) {
            error = "column '" + field.name + "': a fixed_size_list of " + element + " is not read";
            return false;
        }
    }
    t.boolean = type == "bool";
    t.null_type = type == "null";
    t.value_bytes = lance_logical_type_value_bytes(type);
    if (t.value_bytes == 0U || type == "struct" || type.rfind("list", 0) == 0 ||
        type.rfind("large_list", 0) == 0 || type.rfind("dict:", 0) == 0) {
        error = "column '" + field.name + "': format 2.0 values of type " + type + " are not read";
        return false;
    }
    return true;
}

/// One page's values, appended to the column.
bool append_page(ColumnValues& out, const LeafType& t, Arr& a, const std::string& name, std::string& error) {
    const auto at = out.rows;
    const bool all_null = a.shape == Arr::Shape::kAllNull;
    if (t.variable) {
        if (!all_null && a.shape != Arr::Shape::kVariable) {
            error = "column '" + name + "': format 2.0 page of strings decoded to fixed-width values";
            return false;
        }
        auto& v = out.variable;
        const std::size_t ob = v.large ? 8U : 4U;
        if (v.offsets.empty()) {
            v.offsets.assign(ob, 0U);
        }
        const std::uint64_t base = v.data.size();
        if (!all_null) {
            v.data.insert(v.data.end(), a.bytes.begin(), a.bytes.end());
        }
        if (v.data.size() > default_read_limits().max_uncompressed_bytes ||
            (!v.large && v.data.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))) {
            error = "column '" + name + "' exceeds the decoded-size limit";
            return false;
        }
        const auto old = v.offsets.size();
        v.offsets.resize(old + static_cast<std::size_t>(a.n) * ob);
        for (std::uint64_t i = 0; i < a.n; ++i) {
            const std::uint64_t end = base + (all_null ? 0U : a.offsets[static_cast<std::size_t>(i) + 1U]);
            if (v.large) {
                const auto e = static_cast<std::int64_t>(end);
                std::memcpy(v.offsets.data() + old + i * 8U, &e, 8U);
            } else {
                const auto e = static_cast<std::int32_t>(end);
                std::memcpy(v.offsets.data() + old + i * 4U, &e, 4U);
            }
        }
    } else if (t.null_type) {
        if (!all_null) {
            error = "column '" + name + "': a null-typed format 2.0 page holds values";
            return false;
        }
        out.fixed.resize(out.fixed.size() + static_cast<std::size_t>(a.n) * t.value_bytes, 0U);
    } else if (t.boolean) {
        const auto base = out.fixed.size();
        out.fixed.resize(base + static_cast<std::size_t>(a.n), 0U);
        if (a.shape == Arr::Shape::kBits) {
            for (std::uint64_t i = 0; i < a.n; ++i) {
                out.fixed[base + static_cast<std::size_t>(i)] = bit_at(a.bytes, i) ? 1U : 0U;
            }
        } else if (a.shape == Arr::Shape::kFixed && a.width == 1U) {
            for (std::uint64_t i = 0; i < a.n; ++i) {
                out.fixed[base + static_cast<std::size_t>(i)] = a.bytes[static_cast<std::size_t>(i)] != 0U ? 1U : 0U;
            }
        } else if (!all_null) {
            error = "column '" + name + "': format 2.0 booleans decoded to another shape";
            return false;
        }
    } else {
        if (!all_null && (a.shape != Arr::Shape::kFixed || a.width != t.value_bytes ||
                          (t.items != 0U && a.dimension != t.items) || (t.items == 0U && a.dimension != 0U))) {
            error = "column '" + name + "': format 2.0 values of " + std::to_string(a.width) +
                    " bytes for a type of " + std::to_string(t.value_bytes);
            return false;
        }
        const auto base = out.fixed.size();
        if (all_null) {
            out.fixed.resize(base + static_cast<std::size_t>(a.n) * t.value_bytes, 0U);
        } else {
            out.fixed.insert(out.fixed.end(), a.bytes.begin(), a.bytes.end());
        }
        if (out.fixed.size() > default_read_limits().max_uncompressed_bytes) {
            error = "column '" + name + "' exceeds the decoded-size limit";
            return false;
        }
        if (t.items != 0U) {
            out.items_per_row = t.items;
            const auto item_at = at * t.items;
            const auto items = a.n * t.items;
            if (all_null) {
                append_bits(out.item_validity, out.item_null_count, item_at, nullptr, items, true);
            } else {
                append_bits(out.item_validity, out.item_null_count, item_at, &a.item_validity, items, false);
            }
        }
    }
    append_bits(out.validity, out.null_count, at, &a.validity, a.n, all_null);
    out.rows += a.n;
    return true;
}

/// Rows [start, start + count) of one page.
bool decode_page(const std::filesystem::path& path, const pb::ColumnMetadata& column, const pb::ColumnPage& page,
                 std::uint64_t start, std::uint64_t count, Arr& out, std::string& error) {
    if (start > page.length || count > page.length - start) {
        error = "rows " + std::to_string(start) + "+" + std::to_string(count) + " past the page's " +
                std::to_string(page.length);
        return false;
    }
    Enc enc;
    if (!parse_page_encoding(page.encoding, enc, error)) {
        return false;
    }
    PageReader io(path, page, column);
    return decode(enc, start, count, io, out, error);
}

/// Is this a 2.0 blob column? Its own encoding: Any { /lance.encodings.ColumnEncoding, { blob = 3 } }.
bool is_blob_column(const pb::ColumnMetadata& column) {
    if (column.encoding.empty()) {
        return false;
    }
    wire::EncodingAny any;
    const auto in = nm::from(std::span<const std::byte>(reinterpret_cast<const std::byte*>(column.encoding.data()),
                                                        column.encoding.size()));
    if (!nm::protobuf_decode(in, any) || *any.type_url != "/lance.encodings.ColumnEncoding" || !*any.value) {
        return false;
    }
    wire::ColumnEncoding20 encoding;
    return nm::protobuf_decode(nm::from(**any.value), encoding) && encoding.blob->has_value();
}

/// A 2.0 blob column's page: (position, size) per row, the bytes elsewhere in the same data file. A
/// null is (1, 0) and an empty value (0, 0), as Lance writes them.
bool read_blob_values(const std::filesystem::path& path, Arr& a, const std::string& name, std::string& error) {
    if (a.shape != Arr::Shape::kFixed || a.packed != std::vector<std::uint32_t>{8U, 8U}) {
        error = "column '" + name + "': a format 2.0 blob page that is not (position, size) descriptions";
        return false;
    }
    Arr out;
    out.shape = Arr::Shape::kVariable;
    out.n = a.n;
    out.offsets.assign(1, 0U);
    out.offsets.reserve(static_cast<std::size_t>(a.n) + 1U);
    std::vector<std::uint8_t> value;
    for (std::uint64_t i = 0; i < a.n; ++i) {
        std::uint64_t position = 0;
        std::uint64_t size = 0;
        std::memcpy(&position, a.bytes.data() + i * 16U, 8U);
        std::memcpy(&size, a.bytes.data() + i * 16U + 8U, 8U);
        if (size == 0U && position == 1U) {
            if (out.validity.empty()) {
                out.validity.assign(static_cast<std::size_t>((a.n + 7U) / 8U), 0xFFU);
            }
            out.validity[static_cast<std::size_t>(i >> 3U)] &= static_cast<std::uint8_t>(~(1U << (i & 7U)));
            ++out.nulls;
        } else if (size != 0U) {
            if (!read_lance_data_file_bytes(path, position, size, value, error)) {
                error = "column '" + name + "' blob " + std::to_string(i) + ": " + error;
                return false;
            }
            out.bytes.insert(out.bytes.end(), value.begin(), value.end());
            if (out.bytes.size() > default_read_limits().max_uncompressed_bytes) {
                error = "column '" + name + "' exceeds the decoded-size limit";
                return false;
            }
        }
        out.offsets.push_back(out.bytes.size());
    }
    a = std::move(out);
    return true;
}

/// A field of a packed struct: its bytes out of each row of the struct's packed page.
bool select_packed_child(Arr& a, const LeafContext* context, const LeafType& t, const std::string& name,
                         std::string& error) {
    if (context == nullptr || context->packed_child < 0) {
        return true;
    }
    const auto k = static_cast<std::size_t>(context->packed_child);
    if (a.shape != Arr::Shape::kFixed || k >= a.packed.size()) {
        error = "column '" + name + "': its packed struct page has no field " + std::to_string(k);
        return false;
    }
    const std::uint64_t width = a.packed[k];
    if (t.variable || t.boolean || width != t.value_bytes) {
        error = "column '" + name + "': its packed struct page holds " + std::to_string(width) +
                "-byte values where " + std::to_string(t.value_bytes) + " were expected";
        return false;
    }
    std::uint64_t skip = 0;
    for (std::size_t i = 0; i < k; ++i) skip += a.packed[i];
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(a.n * width));
    for (std::uint64_t i = 0; i < a.n; ++i) {
        std::memcpy(bytes.data() + i * width, a.bytes.data() + i * a.width + skip, static_cast<std::size_t>(width));
    }
    a.bytes = std::move(bytes);
    a.width = static_cast<std::uint32_t>(width);
    a.dimension = static_cast<std::uint32_t>(t.items);  // packed fields are never null, nor their items
    a.packed.clear();
    return true;
}

void start_column(ColumnValues& out, const LeafType& t) {
    out = ColumnValues{};
    if (t.variable) {
        out.kind = ColumnValues::Kind::VariableWidth;
        out.variable.large = t.large;
        out.variable.offsets.assign(t.large ? 8U : 4U, 0U);
    } else {
        out.kind = ColumnValues::Kind::FixedWidth;
    }
}

/// The values of pages [p0, p1) of a column.
bool decode_pages(const std::filesystem::path& path, const pb::Field& field, const pb::ColumnMetadata& column,
                  const LeafContext* context, std::size_t p0, std::size_t p1, ColumnValues& out,
                  std::string& error) {
    LeafType t;
    if (!leaf_type(field, t, error)) {
        return false;
    }
    start_column(out, t);
    const bool blob = t.variable && is_blob_column(column);
    for (std::size_t p = p0; p < p1; ++p) {
        const auto& page = column.pages[p];
        if (page.length == 0U) {
            continue;
        }
        Arr a;
        if (!decode_page(path, column, page, 0, page.length, a, error)) {
            error = "column '" + field.name + "' page " + std::to_string(p) + ": " + error;
            return false;
        }
        if (blob && !read_blob_values(path, a, field.name, error)) {
            return false;
        }
        if (!select_packed_child(a, context, t, field.name, error) || !append_page(out, t, a, field.name, error)) {
            return false;
        }
    }
    return true;
}

/// A list column: its offsets and validity, one entry per list. Each page's offsets count from that
/// page's first item; a null list's is raised by the page's `null_offset_adjustment`.
bool decode_list_layer(const std::filesystem::path& path, const Ancestor& list, ColumnValues::NestedLayer& layer,
                       std::string& error) {
    layer = ColumnValues::NestedLayer{};
    layer.is_list = true;
    layer.offsets.assign(1, 0);
    std::uint64_t base = 0;
    for (std::size_t p = 0; p < list.column->pages.size(); ++p) {
        const auto& page = list.column->pages[p];
        if (page.length == 0U) {
            continue;
        }
        Enc enc;
        if (!parse_page_encoding(page.encoding, enc, error)) {
            return false;
        }
        if (enc.kind != Kind::kList || !enc.a) {
            error = "list '" + list.name + "' page " + std::to_string(p) + " is not a format 2.0 List page";
            return false;
        }
        PageReader io(path, page, *list.column);
        Arr offsets_arr;
        std::vector<std::uint64_t> offsets;
        if (!decode(*enc.a, 0, page.length, io, offsets_arr, error) || !as_u64(offsets_arr, offsets, error)) {
            error = "list '" + list.name + "' page " + std::to_string(p) + ": " + error;
            return false;
        }
        const auto adjustment = enc.null_offset_adjustment;
        std::uint64_t prev = 0;
        for (std::uint64_t i = 0; i < page.length; ++i) {
            const auto raw = offsets[static_cast<std::size_t>(i)];
            const bool valid = raw < adjustment;
            const auto end = valid ? raw : raw - adjustment;
            if (end < prev || end > enc.num_items) {
                error = "corrupt format 2.0 list '" + list.name + "': offset " + std::to_string(end) + " after " +
                        std::to_string(prev) + " of " + std::to_string(enc.num_items) + " items";
                return false;
            }
            const auto r = layer.length + i;
            if (!valid) {
                if (layer.validity.empty()) {
                    layer.validity.assign(static_cast<std::size_t>((r + 8U) / 8U), 0U);
                    for (std::uint64_t k = 0; k < r; ++k) {
                        layer.validity[static_cast<std::size_t>(k >> 3U)] |= static_cast<std::uint8_t>(1U << (k & 7U));
                    }
                }
                ++layer.null_count;
            }
            if (!layer.validity.empty()) {
                layer.validity.resize(static_cast<std::size_t>((r + 8U) / 8U), 0U);
                if (valid) {
                    layer.validity[static_cast<std::size_t>(r >> 3U)] |= static_cast<std::uint8_t>(1U << (r & 7U));
                }
            }
            layer.offsets.push_back(static_cast<std::int64_t>(base + end));
            prev = end;
        }
        if (prev != enc.num_items) {
            error = "format 2.0 list '" + list.name + "' page " + std::to_string(p) + " references " +
                    std::to_string(enc.num_items) + " items but its offsets end at " + std::to_string(prev);
            return false;
        }
        base += enc.num_items;
        layer.length += page.length;
    }
    return true;
}

/// A string an older writer stored as list<uint8>: the list column's offsets, the next column's bytes.
bool decode_binary_as_list(const std::filesystem::path& path, const pb::Field& field, const pb::ColumnMetadata& column,
                           const pb::ColumnMetadata& items, ColumnValues& out, std::string& error) {
    LeafType t;
    if (!leaf_type(field, t, error)) {
        return false;
    }
    Ancestor as_list;
    as_list.is_list = true;
    as_list.name = field.name;
    as_list.column = &column;
    ColumnValues::NestedLayer layer;
    if (!decode_list_layer(path, as_list, layer, error)) {
        return false;
    }
    pb::Field bytes_field;
    bytes_field.name = field.name;
    bytes_field.logical_type = "uint8";
    ColumnValues bytes;
    if (!decode_pages(path, bytes_field, items, nullptr, 0, items.pages.size(), bytes, error)) {
        return false;
    }
    if (bytes.fixed.size() != static_cast<std::size_t>(layer.offsets.back())) {
        error = "column '" + field.name + "': its bytes column holds " + std::to_string(bytes.fixed.size()) +
                " bytes for offsets ending at " + std::to_string(layer.offsets.back());
        return false;
    }
    start_column(out, t);
    out.variable.data = std::move(bytes.fixed);
    out.variable.offsets.clear();
    for (const auto o : layer.offsets) {
        if (t.large) {
            const auto e = static_cast<std::int64_t>(o);
            out.variable.offsets.insert(out.variable.offsets.end(), reinterpret_cast<const std::uint8_t*>(&e),
                                        reinterpret_cast<const std::uint8_t*>(&e) + 8);
        } else {
            if (o > std::numeric_limits<std::int32_t>::max()) {
                error = "column '" + field.name + "' exceeds 2 GiB of string data";
                return false;
            }
            const auto e = static_cast<std::int32_t>(o);
            out.variable.offsets.insert(out.variable.offsets.end(), reinterpret_cast<const std::uint8_t*>(&e),
                                        reinterpret_cast<const std::uint8_t*>(&e) + 4);
        }
    }
    out.validity = std::move(layer.validity);
    out.null_count = layer.null_count;
    out.rows = layer.length;
    return true;
}

/// Rows [start, start + count) of page `p`, appended to `out`.
bool append_rows(const std::filesystem::path& path, const pb::Field& field, const pb::ColumnMetadata& column,
                 const LeafContext* context, std::size_t p, std::uint64_t start, std::uint64_t count,
                 const LeafType& t, ColumnValues& out, std::string& error) {
    Arr a;
    if (!decode_page(path, column, column.pages[p], start, count, a, error)) {
        error = "column '" + field.name + "' page " + std::to_string(p) + ": " + error;
        return false;
    }
    if (t.variable && is_blob_column(column) && !read_blob_values(path, a, field.name, error)) {
        return false;
    }
    return select_packed_child(a, context, t, field.name, error) && append_page(out, t, a, field.name, error);
}

/// The column's top-level row count, and the first top-level row of each of its pages (only for a
/// leaf with no list above it: a leaf under a list counts items, not rows).
std::vector<std::uint64_t> page_starts(const pb::ColumnMetadata& column) {
    std::vector<std::uint64_t> starts(column.pages.size() + 1U, 0U);
    for (std::size_t p = 0; p < column.pages.size(); ++p) {
        starts[p + 1U] = starts[p] + column.pages[p].length;
    }
    return starts;
}

bool needs_whole_column(const LeafContext* context) {
    return context != nullptr && (context->has_lists() || context->binary_items != nullptr);
}

}  // namespace

bool LeafContext::has_lists() const {
    return std::any_of(ancestors.begin(), ancestors.end(), [](const Ancestor& a) { return a.is_list; });
}

bool is_v20_column(const pb::ColumnMetadata& column) {
    static const std::string kUrl = "/lance.encodings.ArrayEncoding";
    for (const auto& page : column.pages) {
        if (page.encoding.empty()) {
            continue;
        }
        // Any { type_url = 1 }: the url sits right after its tag and one-byte length.
        const auto& e = page.encoding;
        return e.size() >= 2U + kUrl.size() && e[0] == 0x0AU && e[1] == kUrl.size() &&
               std::memcmp(e.data() + 2, kUrl.data(), kUrl.size()) == 0;
    }
    return false;
}

bool column_is_packed_struct(const pb::ColumnMetadata& column) {
    for (const auto& page : column.pages) {
        if (page.encoding.empty() || page.length == 0U) {
            continue;
        }
        Enc enc;
        std::string ignored;
        return parse_page_encoding(page.encoding, enc, ignored) && enc.kind == Kind::kPackedStruct;
    }
    return false;
}

bool column_is_list_encoded(const pb::ColumnMetadata& column) {
    for (const auto& page : column.pages) {
        if (page.encoding.empty() || page.length == 0U) {
            continue;
        }
        Enc enc;
        std::string ignored;
        return parse_page_encoding(page.encoding, enc, ignored) && enc.kind == Kind::kList;
    }
    return false;
}

bool decode_column(const std::filesystem::path& path, const pb::Field& field, const pb::ColumnMetadata& column,
                   const LeafContext* context, ColumnValues& out, std::string& error) {
    error.clear();
    if (context != nullptr && context->binary_items != nullptr) {
        if (context->has_lists()) {
            error = "column '" + field.name + "': a list of strings stored as lists of bytes is not read";
            return false;
        }
        return decode_binary_as_list(path, field, column, *context->binary_items, out, error);
    }
    if (!decode_pages(path, field, column, context, 0, column.pages.size(), out, error)) {
        return false;
    }
    if (context == nullptr || !context->has_lists()) {
        return true;
    }
    // Layers for every list and struct above the leaf, outermost first. A struct's length is that of
    // the level it sits at: the rows, or the items of the list above it.
    std::vector<ColumnValues::NestedLayer> layers(context->ancestors.size());
    for (std::size_t k = 0; k < context->ancestors.size(); ++k) {
        const auto& a = context->ancestors[k];
        if (a.is_list) {
            if (a.column == nullptr) {
                error = "list '" + a.name + "' has no offsets column";
                return false;
            }
            if (!decode_list_layer(path, a, layers[k], error)) {
                return false;
            }
        } else {
            layers[k].is_list = false;
        }
    }
    // The level of the outermost layer: rows. Structs above the first list share its length.
    std::uint64_t level = 0;
    for (const auto& layer : layers) {
        if (layer.is_list) {
            level = layer.length;
            break;
        }
    }
    for (auto& layer : layers) {
        if (!layer.is_list) {
            layer.length = level;
            continue;
        }
        if (layer.length != level) {
            error = "column '" + field.name + "': a list holds " + std::to_string(layer.length) + " entries where " +
                    std::to_string(level) + " were expected";
            return false;
        }
        level = static_cast<std::uint64_t>(layer.offsets.back());
    }
    if (out.rows != level) {
        error = "column '" + field.name + "' holds " + std::to_string(out.rows) + " items where its lists reference " +
                std::to_string(level);
        return false;
    }
    out.layers = std::move(layers);
    return true;
}

bool decode_column_range(const std::filesystem::path& path, const pb::Field& field, const pb::ColumnMetadata& column,
                         const LeafContext* context, std::uint64_t first, std::uint64_t count,
                         std::size_t value_bytes, ColumnValues& out, std::string& error) {
    error.clear();
    if (needs_whole_column(context)) {
        if (!decode_column(path, field, column, context, out, error)) {
            return false;
        }
        const auto total = out.layers.empty() ? out.rows : out.layers.front().length;
        return slice_column_values(out, first, count, total, value_bytes, error);
    }
    const auto starts = page_starts(column);
    if (first > starts.back() || count > starts.back() - first) {
        error = "rows " + std::to_string(first) + ".." + std::to_string(first + count) + " past the column's " +
                std::to_string(starts.back());
        return false;
    }
    LeafType t;
    if (!leaf_type(field, t, error)) {
        return false;
    }
    start_column(out, t);
    // Just the rows asked for, from each page they span.
    for (std::size_t p = 0; p < column.pages.size() && count != 0U; ++p) {
        const auto lo = std::max(first, starts[p]);
        const auto hi = std::min(first + count, starts[p + 1U]);
        if (lo >= hi) {
            continue;
        }
        if (!append_rows(path, field, column, context, p, lo - starts[p], hi - lo, t, out, error)) {
            return false;
        }
    }
    return true;
}

bool decode_column_rows(const std::filesystem::path& path, const pb::Field& field, const pb::ColumnMetadata& column,
                        const LeafContext* context, const std::vector<std::uint64_t>& rows,
                        std::size_t value_bytes, ColumnValues& out, std::string& error) {
    error.clear();
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (rows[i] <= rows[i - 1U]) {
            error = "rows to take must be strictly ascending";
            return false;
        }
    }
    if (needs_whole_column(context)) {
        if (!decode_column(path, field, column, context, out, error)) {
            return false;
        }
        const auto total = out.layers.empty() ? out.rows : out.layers.front().length;
        std::vector<std::uint8_t> keep(static_cast<std::size_t>(total), 0U);
        for (const auto r : rows) {
            if (r >= total) {
                error = "row " + std::to_string(r) + " is past the column's " + std::to_string(total) + " rows";
                return false;
            }
            keep[static_cast<std::size_t>(r)] = 1U;
        }
        return compact_column_values(out, keep, total, value_bytes, error);
    }
    const auto starts = page_starts(column);
    if (!rows.empty() && rows.back() >= starts.back()) {
        error = "row " + std::to_string(rows.back()) + " is past the column's " + std::to_string(starts.back()) +
                " rows";
        return false;
    }
    LeafType t;
    if (!leaf_type(field, t, error)) {
        return false;
    }
    start_column(out, t);
    // Each run of consecutive rows is one ranged decode of the page holding it.
    std::size_t p = 0;
    for (std::size_t i = 0; i < rows.size();) {
        while (rows[i] >= starts[p + 1U]) {
            ++p;
        }
        std::size_t j = i + 1U;
        while (j < rows.size() && rows[j] == rows[j - 1U] + 1U && rows[j] < starts[p + 1U]) {
            ++j;
        }
        if (!append_rows(path, field, column, context, p, rows[i] - starts[p], static_cast<std::uint64_t>(j - i), t, out,
                         error)) {
            return false;
        }
        i = j;
    }
    return true;
}

}  // namespace nano_lance::v20
