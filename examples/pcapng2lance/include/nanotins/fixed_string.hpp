#pragma once

#include <algorithm>
#include <cstddef>

namespace nanotins {

// C++20 class-type NTTP carrying a compile-time field name (the `field<"vid", 12>` parameter).
// `N` includes the trailing NUL of the source literal; `size()` is the visible length.
template <std::size_t N>
struct fixed_string {
    char value[N]{};

    constexpr fixed_string(const char (&str)[N]) { std::copy_n(str, N, value); }

    constexpr const char* c_str() const { return value; }
    static constexpr std::size_t size() { return N - 1; }
};

template <std::size_t N>
fixed_string(const char (&)[N]) -> fixed_string<N>;

}  // namespace nanotins
