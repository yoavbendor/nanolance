#pragma once

// Explicit-offset wire spec + a contiguous, device-safe reader.
//
// A StructSpec is a compile-time list of named_field<tag, offset, type, endian> — the single source of
// truth for a wire PDU. Unlike a packed C struct, offsets are explicit (so unaligned multi-byte fields,
// e.g. a uint64 at byte 49, are first-class), and the same spec drives both the HOST contiguous read and
// the GPU firehose: read_field<F>(ptr) is byte-indexed off a raw pointer (no std::ranges, alignment-safe,
// device-safe via NANOTINS_HD), so the compiler coalesces aligned cases while odd offsets stay correct.
//
// This is the wire-IN side. The columnar OUT side (SoA -> Arrow -> Lance) is struct_spec_soa.hpp, which
// derives a fixed-N SoA + scatter from the same spec. A host-only SPARSE reader (over fragmented /
// segmented backings) is a separate, later addition; M1 needs only the contiguous reader.
//
// Vocabulary adapted from the udp_ranges StructSpec/named_field/_fld idea; the reader is reimplemented
// pointer-based for device-safety + no iterator overhead.

#include "soatins/portability.hpp"  // NANOTINS_HD

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

namespace nanotins {

enum class wire_endian { little, big };

// ---- compile-time field-name tag + the _fld user-defined literal -------------------------------------
template <char... Cs>
struct name_tag {
    static constexpr char value[sizeof...(Cs) + 1] = {Cs..., '\0'};
};
template <char... Cs>
constexpr char name_tag<Cs...>::value[sizeof...(Cs) + 1];

namespace literals {
// The string-literal operator template (`"foo"_fld`) is a well-supported GNU/Clang extension; silence the
// pedantic warning rather than lose the ergonomic named-field syntax.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-string-literal-operator-template"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
template <class CharT, CharT... Cs>
consteval name_tag<Cs...> operator""_fld() {
    return {};
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}  // namespace literals

// ---- a named wire field: name tag, byte offset, value type, endianness -------------------------------
template <class Tag, std::size_t Offset, class T, wire_endian E>
struct named_field {
    using tag = Tag;
    using value_type = T;
    static constexpr std::size_t offset = Offset;
    static constexpr wire_endian order = E;
    static constexpr const char* name() { return Tag::value; }
    static constexpr std::size_t end_offset() { return Offset + sizeof(T); }
};

// Smallest unsigned type that holds a Width-bit value (the column element type of a bit-field).
template <std::size_t Width>
using uint_for_width =
    std::conditional_t<(Width <= 8), std::uint8_t,
                       std::conditional_t<(Width <= 16), std::uint16_t,
                                          std::conditional_t<(Width <= 32), std::uint32_t, std::uint64_t>>>;

// A bit-field: Width bits at BitOffset inside a StorageT-wide unit at byte Offset, in the unit's endian.
// BitOffset is MSB-first (BitOffset 0 = the high bit) — network wire order, matching soatins bits<>: a
// 0x45 byte with version<0,4>/ihl<4,4> yields version=4, ihl=5.
template <class Tag, std::size_t Offset, class StorageT, std::size_t BitOffset, std::size_t Width,
          wire_endian E>
struct named_bit_field {
    using tag = Tag;
    using value_type = uint_for_width<Width>;
    static constexpr std::size_t offset = Offset;
    static constexpr std::size_t bit_offset = BitOffset;
    static constexpr std::size_t width = Width;
    static constexpr wire_endian order = E;
    static constexpr std::size_t storage_bits = sizeof(StorageT) * 8;
    static constexpr const char* name() { return Tag::value; }
    static constexpr std::size_t end_offset() { return Offset + sizeof(StorageT); }
};

// A fixed-size byte-array field (IPv6/MAC address, a raw blob): N raw bytes at Offset, no endian swap.
// Its column is a fixed-size-binary, std::array<uint8_t, N> (soatins maps that to Arrow FIXED_SIZE_BINARY).
template <class Tag, std::size_t Offset, std::size_t N>
struct named_bytes_field {
    using tag = Tag;
    using value_type = std::array<std::uint8_t, N>;
    static constexpr std::size_t offset = Offset;
    static constexpr std::size_t byte_count = N;
    static constexpr const char* name() { return Tag::value; }
    static constexpr std::size_t end_offset() { return Offset + N; }
};

// ---- spec composition: embed<Prefix, Offset, SubSpec> splices a sub-spec's fields ---------------------
// Each sub-field is shifted by Offset and its tag prefixed with Prefix (use decltype(""_fld) for none).
// Purely compile-time: it flattens to the same leaf field list, so the readers / SoA / device path are
// unchanged. Lets a header (e.g. the PTP common header) be defined once and reused across message types.
namespace detail_spec {

template <class A, class B>
struct cat_tag;
template <char... A, char... B>
struct cat_tag<name_tag<A...>, name_tag<B...>> {
    using type = name_tag<A..., B...>;
};
template <class A, class B>
using cat_tag_t = typename cat_tag<A, B>::type;

template <class Prefix, std::size_t Base, class F>
struct shift_field;
template <class Prefix, std::size_t Base, class Tag, std::size_t Off, class T, wire_endian E>
struct shift_field<Prefix, Base, named_field<Tag, Off, T, E>> {
    using type = named_field<cat_tag_t<Prefix, Tag>, Base + Off, T, E>;
};
template <class Prefix, std::size_t Base, class Tag, std::size_t Off, class S, std::size_t BO, std::size_t W,
          wire_endian E>
struct shift_field<Prefix, Base, named_bit_field<Tag, Off, S, BO, W, E>> {
    using type = named_bit_field<cat_tag_t<Prefix, Tag>, Base + Off, S, BO, W, E>;
};
template <class Prefix, std::size_t Base, class Tag, std::size_t Off, std::size_t N>
struct shift_field<Prefix, Base, named_bytes_field<Tag, Off, N>> {
    using type = named_bytes_field<cat_tag_t<Prefix, Tag>, Base + Off, N>;
};

template <class Prefix, std::size_t Base, class Tuple>
struct shift_all;
template <class Prefix, std::size_t Base, class... F>
struct shift_all<Prefix, Base, std::tuple<F...>> {
    using type = std::tuple<typename shift_field<Prefix, Base, F>::type...>;
};

template <class A, class B>
struct cat2;
template <class... A, class... B>
struct cat2<std::tuple<A...>, std::tuple<B...>> {
    using type = std::tuple<A..., B...>;
};
template <class... Ts>
struct concat {
    using type = std::tuple<>;
};
template <class T>
struct concat<T> {
    using type = T;
};
template <class A, class B, class... R>
struct concat<A, B, R...> {
    using type = typename concat<typename cat2<A, B>::type, R...>::type;
};

}  // namespace detail_spec

template <class Prefix, std::size_t Offset, class SubSpec>
struct embed {};

namespace detail_spec {
template <class E>
struct expand_one {
    using type = std::tuple<E>;
};
template <class Prefix, std::size_t Offset, class SubSpec>
struct expand_one<embed<Prefix, Offset, SubSpec>> {
    using type = typename shift_all<Prefix, Offset, typename SubSpec::fields>::type;
};
}  // namespace detail_spec

// A StructSpec is a list of leaf fields and/or embed<> groups; it flattens to one leaf field tuple.
template <class... Elems>
struct StructSpec {
    using fields = typename detail_spec::concat<typename detail_spec::expand_one<Elems>::type...>::type;
    static constexpr std::size_t field_count = std::tuple_size_v<fields>;
};

template <class Spec>
using spec_fields_t = typename Spec::fields;

// ---- the reader: byte-indexed, alignment-safe, device-safe -------------------------------------------
// Assemble a sizeof(T)-byte scalar from p[off..] in the field's endianness. A tiny loop with no STL, no
// misaligned word load, so it is correct at any offset and compiles for device under NANOTINS_HD.
template <class T, wire_endian E>
NANOTINS_HD inline T read_scalar_at(const std::uint8_t* p, std::size_t off) {
    constexpr std::size_t n = sizeof(T);
    std::uint64_t v = 0;
    if constexpr (E == wire_endian::big) {
        for (std::size_t i = 0; i < n; ++i) {
            v = (v << 8) | static_cast<std::uint64_t>(p[off + i]);
        }
    } else {
        for (std::size_t i = n; i-- > 0;) {
            v = (v << 8) | static_cast<std::uint64_t>(p[off + i]);
        }
    }
    return static_cast<T>(v);
}

// Read one field — scalar or bit-field — dispatched on whether F carries bit_offset/width. A bit-field
// reads its StorageT-wide unit (endian-aware), then shifts+masks: MSB-first for big-endian units
// (shift = storage_bits - width - bit_offset), LSB-first for little. Device-safe either way.
template <class F>
NANOTINS_HD inline typename F::value_type read_field(const std::uint8_t* p) {
    if constexpr (requires { F::byte_count; }) {
        typename F::value_type a{};
        for (std::size_t i = 0; i < F::byte_count; ++i) {
            a[i] = static_cast<std::uint8_t>(p[F::offset + i]);
        }
        return a;
    } else if constexpr (requires { F::bit_offset; F::width; F::storage_bits; }) {
        using SU = uint_for_width<F::storage_bits>;
        const std::uint64_t unit = static_cast<std::uint64_t>(read_scalar_at<SU, F::order>(p, F::offset));
        const std::uint64_t mask = (std::uint64_t{1} << F::width) - 1;
        std::size_t shift = F::bit_offset;
        if constexpr (F::order == wire_endian::big) {
            shift = F::storage_bits - F::width - F::bit_offset;
        }
        return static_cast<typename F::value_type>((unit >> shift) & mask);
    } else {
        return read_scalar_at<typename F::value_type, F::order>(p, F::offset);
    }
}

// Fixed extent of the spec (max field end) — buffer sizing now; the basis for length-driven fields later.
template <class Spec>
constexpr std::size_t spec_size() {
    std::size_t mx = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((mx = std::tuple_element_t<I, spec_fields_t<Spec>>::end_offset() > mx
                   ? std::tuple_element_t<I, spec_fields_t<Spec>>::end_offset()
                   : mx),
         ...);
    }(std::make_index_sequence<Spec::field_count>{});
    return mx;
}

// ---- a contiguous view with named access: view("field"_fld) ------------------------------------------
template <class Spec>
class struct_view {
public:
    NANOTINS_HD explicit struct_view(const std::uint8_t* p) : p_(p) {}

    template <char... Cs>
    NANOTINS_HD auto operator()(name_tag<Cs...>) const {
        return get_by_tag<name_tag<Cs...>, 0>();
    }

private:
    template <class Tag, std::size_t I>
    NANOTINS_HD auto get_by_tag() const {
        using F = std::tuple_element_t<I, spec_fields_t<Spec>>;
        if constexpr (std::is_same_v<typename F::tag, Tag>) {
            return read_field<F>(p_);
        } else if constexpr (I + 1 < Spec::field_count) {
            return get_by_tag<Tag, I + 1>();
        } else {
            static_assert(sizeof(Tag) == 0, "struct_view: unknown field tag");
        }
    }

    const std::uint8_t* p_;
};

}  // namespace nanotins
