#pragma once

// Bitfields packed into a (possibly big-endian) word. Native C++ bitfields can't portably overlay
// wire data, so the robust recipe is: byteswap the whole containing word first (the `be<>` word does
// this), then shift+mask on the host value — bit positions then match the RFC's MSB-first numbering.
// Endianness therefore belongs to the *word* (`bits<be<u16>, ...>`), not to each sub-field.
//
// `bits<Word, field<"a",3>, field<"b",13>>` overlays exactly `sizeof(Word)` wire bytes and expands to
// one Lance column per sub-field (see column_traits / columns_of).

#include "nanotins/endian.hpp"
#include "nanotins/fixed_string.hpp"
#include "nanotins/portability.hpp"

#include <cstdint>
#include <tuple>
#include <type_traits>

namespace nanotins {

template <fixed_string Name, unsigned W>
struct field {
    static constexpr auto name = Name;
    static constexpr unsigned width = W;
};

// Smallest unsigned integer type that holds W bits.
template <unsigned W>
using uint_for_bits = std::conditional_t<
    (W <= 8), std::uint8_t,
    std::conditional_t<(W <= 16), std::uint16_t, std::conditional_t<(W <= 32), std::uint32_t, std::uint64_t>>>;

template <class Word, class... Fields>
struct bits {
    Word word;  // overlay: be<U>, le<U>, or a bare scalar U

    using word_type = Word;
    using fields_tuple = std::tuple<Fields...>;
    static constexpr unsigned field_count = sizeof...(Fields);
    static constexpr unsigned word_bits = static_cast<unsigned>(sizeof(wire_underlying_t<Word>) * 8);
    static constexpr unsigned widths[field_count] = {Fields::width...};

    static_assert(field_count > 0, "bits<> needs at least one field");
    static_assert((Fields::width + ...) == word_bits, "bit widths must sum to the word width");

    static constexpr unsigned width_at(unsigned j) { return widths[j]; }

    // MSB-first: shift_j = word_bits - (w0 + ... + wj).
    static constexpr unsigned shift_at(unsigned j) {
        unsigned acc = 0;
        for (unsigned i = 0; i <= j; ++i) {
            acc += widths[i];
        }
        return word_bits - acc;
    }

    static constexpr std::uint64_t mask_at(unsigned j) { return (std::uint64_t{1} << widths[j]) - 1U; }

    NANOTINS_HD std::uint64_t word_host() const { return static_cast<std::uint64_t>(host_value(word)); }
};

// J-th sub-field's element type (smallest unsigned holding its width).
template <class Bits, unsigned J>
using bits_subfield_elem = uint_for_bits<std::tuple_element_t<J, typename Bits::fields_tuple>::width>;

}  // namespace nanotins
