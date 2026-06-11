#pragma once

// Wire byte order attached to the field *type*. `be<T>`/`le<T>` store raw wire bytes in an
// `unsigned char[N]` so the aggregate is 1-aligned, standard-layout, and free of padding — safe to
// overlay directly on packet memory. `host()` assembles a host-order value (the only place a swap
// happens). Rolled by hand (not boost::endian) so the conversion is `__host__ __device__`.

#include "soatins/portability.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace soatins {

template <class T>
struct be {
    static_assert(std::is_integral_v<T>, "be<T> requires an integral T");
    unsigned char raw[sizeof(T)];

    NANOTINS_HD T host() const noexcept {
        std::make_unsigned_t<T> v{};
        for (std::size_t k = 0; k < sizeof(T); ++k) {
            v = static_cast<std::make_unsigned_t<T>>(v << 8) | raw[k];
        }
        return static_cast<T>(v);
    }
    NANOTINS_HD operator T() const noexcept { return host(); }
};

template <class T>
struct le {
    static_assert(std::is_integral_v<T>, "le<T> requires an integral T");
    unsigned char raw[sizeof(T)];

    NANOTINS_HD T host() const noexcept {
        std::make_unsigned_t<T> v{};
        for (std::size_t k = sizeof(T); k-- > 0;) {
            v = static_cast<std::make_unsigned_t<T>>(v << 8) | raw[k];
        }
        return static_cast<T>(v);
    }
    NANOTINS_HD operator T() const noexcept { return host(); }
};

static_assert(sizeof(be<std::uint16_t>) == 2 && alignof(be<std::uint16_t>) == 1);
static_assert(std::is_standard_layout_v<be<std::uint32_t>>);
static_assert(std::is_standard_layout_v<le<std::uint64_t>>);

// Trait: pull the underlying scalar out of a (possibly wrapped) wire field type.
template <class F>
struct wire_underlying {
    using type = F;
};
template <class T>
struct wire_underlying<be<T>> {
    using type = T;
};
template <class T>
struct wire_underlying<le<T>> {
    using type = T;
};
template <class F>
using wire_underlying_t = typename wire_underlying<F>::type;

// Read a (possibly wrapped) wire field as a host-order value.
template <class F>
NANOTINS_HD constexpr auto host_value(const F& f) {
    if constexpr (requires { f.host(); }) {
        return f.host();
    } else {
        return f;
    }
}

}  // namespace soatins
