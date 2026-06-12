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
template <class CharT, CharT... Cs>
consteval name_tag<Cs...> operator""_fld() {
    return {};
}
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

template <class... Fields>
struct StructSpec {
    static constexpr std::size_t field_count = sizeof...(Fields);
    using fields = std::tuple<Fields...>;
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

template <class F>
NANOTINS_HD inline typename F::value_type read_field(const std::uint8_t* p) {
    return read_scalar_at<typename F::value_type, F::order>(p, F::offset);
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
