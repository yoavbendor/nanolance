#pragma once

// Flattens a described struct's members into a compile-time list of *columns* (a plain field -> one
// column; a bits<> field -> one column per sub-field), then builds the owning SoA over that list.
// store(), arrow_schema(), and to_arrow() all iterate this column list, so endianness, bitfields, and
// plain fields are handled by one uniform mechanism.

#include "soatins/column_traits.hpp"
#include "soatins/describe.hpp"
#include "soatins/portability.hpp"

#include <array>
#include <cstddef>
#include <tuple>
#include <utility>
#include <vector>

namespace soatins {

// ---- One column descriptor: a stateless type with elem / arrow info / name() / get(const T&). ----

template <class T, std::size_t K>
struct plain_col {
    static constexpr auto MP = nt_member_ptr<T, K>;
    using field_type = nt_pointee_t<MP>;
    using traits = column_traits<field_type>;
    using elem = typename traits::elem;
    static constexpr arrow_kind kind = traits::kind;
    static constexpr int fixed_width = traits::fixed_width;
    static constexpr bool variable = traits::variable;
    static const char* name() { return nt_names<T>[K]; }
    NANOTINS_HD static elem get(const T& r) { return traits::get(r.*MP); }
};

template <class T, std::size_t K, unsigned J>
struct bits_col {
    static constexpr auto MP = nt_member_ptr<T, K>;
    using bits_type = nt_pointee_t<MP>;
    using elem = bits_subfield_elem<bits_type, J>;
    static constexpr arrow_kind kind = kind_for_scalar<elem>();
    static constexpr int fixed_width = 0;
    static constexpr bool variable = false;
    static const char* name() { return std::tuple_element_t<J, typename bits_type::fields_tuple>::name.c_str(); }
    NANOTINS_HD static elem get(const T& r) {
        const bits_type& b = r.*MP;
        return static_cast<elem>((b.word_host() >> bits_type::shift_at(J)) & bits_type::mask_at(J));
    }
};

// ---- Flatten members -> tuple<column descriptor types...>. ----

template <class A, class B>
struct tuple_cat_t;
template <class... A, class... B>
struct tuple_cat_t<std::tuple<A...>, std::tuple<B...>> {
    using type = std::tuple<A..., B...>;
};

template <class...>
struct concat_all {
    using type = std::tuple<>;
};
template <class A>
struct concat_all<A> {
    using type = A;
};
template <class A, class B, class... R>
struct concat_all<A, B, R...> {
    using type = typename concat_all<typename tuple_cat_t<A, B>::type, R...>::type;
};

template <class T, std::size_t K, class Bits, unsigned... J>
std::tuple<bits_col<T, K, J>...> bits_cols_impl(std::integer_sequence<unsigned, J...>);
template <class T, std::size_t K, class Bits>
using bits_cols_t = decltype(bits_cols_impl<T, K, Bits>(std::make_integer_sequence<unsigned, Bits::field_count>{}));

template <bool IsBits, class T, std::size_t K>
struct member_cols_h {
    using type = std::tuple<plain_col<T, K>>;
};
template <class T, std::size_t K>
struct member_cols_h<true, T, K> {
    using type = bits_cols_t<T, K, nt_pointee_t<nt_member_ptr<T, K>>>;
};
template <class T, std::size_t K>
using member_cols_t = typename member_cols_h<is_bits_v<nt_pointee_t<nt_member_ptr<T, K>>>, T, K>::type;

template <class T, std::size_t... K>
auto columns_of_fn(std::index_sequence<K...>) -> typename concat_all<member_cols_t<T, K>...>::type;
template <class T>
using columns_of = decltype(columns_of_fn<T>(std::make_index_sequence<nt_field_count<T>>{}));

template <class T>
inline constexpr std::size_t column_count = std::tuple_size_v<columns_of<T>>;
template <class T, std::size_t I>
using col_at = std::tuple_element_t<I, columns_of<T>>;

// Compile-time visitor: f.template operator()<I, Col>() for each column.
template <class T, class F>
void for_each_column(F&& f) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (f.template operator()<I, col_at<T, I>>(), ...);
    }(std::make_index_sequence<column_count<T>>{});
}

// ---- Owning host SoA: one column buffer per flattened column. ----

template <class Cols>
struct soa_storage;
template <class... C>
struct soa_storage<std::tuple<C...>> {
    using type = std::tuple<std::vector<typename C::elem>...>;
};

// A pack of raw per-column pointers (one elem* per flattened column) — the device-capturable view of a
// SoA. std::tuple of pointers is trivially copyable, so it is safe to capture into a bulk/device lambda.
template <class Cols>
struct soa_ptrs_h;
template <class... C>
struct soa_ptrs_h<std::tuple<C...>> {
    using type = std::tuple<typename C::elem*...>;
};
template <class T>
using soa_ptrs = typename soa_ptrs_h<columns_of<T>>::type;

// Sentinel capacity selecting the runtime-extent (vector-backed) SoA. soa<T> == soa<T, soa_dynamic>.
inline constexpr std::size_t soa_dynamic = static_cast<std::size_t>(-1);

// Fixed-capacity storage (the *real* SoA): one std::array<elem, N> per flattened column. POD when the
// elems are POD (be<>/le<>/fixed_string/arithmetic all are), so the whole block is trivially copyable —
// contiguous per-column buffers you can reference, H2D as a unit, or borrow into Arrow zero-copy.
template <class Cols, std::size_t N>
struct soa_array_storage;
template <class... C, std::size_t N>
struct soa_array_storage<std::tuple<C...>, N> {
    using type = std::tuple<std::array<typename C::elem, N>...>;
};

// soa<T, N>: fixed-capacity SoA (default) OR soa<T> == soa<T, soa_dynamic>: runtime-extent (vector).
// Both share the reflection (columns_of / col_at) and the same store()/raw() fold — only the backing
// container differs. The fixed form carries an *occupied-rows counter* (size_); append() fills the next
// slot and reports when full, so a sink can flush-and-reset (see column_sink). The unfilled tail
// [size_, N) is never read. NOTE: default-construct a fixed soa (soa<T,N> s;) so the trivial column
// arrays stay UNINITIALIZED (zero-copy, zero-init); do NOT value-init (soa<T,N> s{};) or make_unique it,
// which would zero N*stride bytes for nothing.
template <class T, std::size_t N = soa_dynamic>
class soa {
public:
    using cols = columns_of<T>;
    static constexpr std::size_t ncols = column_count<T>;
    static constexpr std::size_t capacity = N;

    std::size_t size() const { return size_; }
    bool full() const { return size_ >= N; }
    std::size_t space() const { return N - size_; }
    void clear() { size_ = 0; }

    // Scatter one row into slot i (the be<>/bits<> conversions happen in each Col::get).
    NANOTINS_HD void store(std::size_t i, const T& row) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((std::get<I>(columns_)[i] = col_at<T, I>::get(row)), ...);
        }(std::make_index_sequence<ncols>{});
    }

    // Append at the occupied counter; returns true once the SoA is full (i.e. it is now flush time).
    bool append(const T& row) {
        store(size_, row);
        ++size_;
        return size_ >= N;
    }

    template <std::size_t I>
    const auto& column() const {
        return std::get<I>(columns_);
    }

    // A POD pack of raw per-column pointers into this SoA's buffers — trivially copyable, so a bulk kernel
    // can capture it and scatter rows in parallel via `scatter()`, and a soa_view can borrow it zero-copy.
    soa_ptrs<T> raw() {
        soa_ptrs<T> p;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((std::get<I>(p) = std::get<I>(columns_).data()), ...);
        }(std::make_index_sequence<ncols>{});
        return p;
    }

private:
    typename soa_array_storage<cols, N>::type columns_;  // uninitialized for trivial elems (no zero-init)
    std::size_t size_ = 0;
};

// Runtime-extent specialization: the original vector-backed SoA. Kept for the one site that sizes to an
// exact runtime total after a count pass (the bulk prefix-sum scatter); fixed soa<T,N> is the default.
template <class T>
class soa<T, soa_dynamic> {
public:
    using cols = columns_of<T>;
    static constexpr std::size_t ncols = column_count<T>;

    void resize(std::size_t n) {
        size_ = n;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (std::get<I>(columns_).resize(n), ...);
        }(std::make_index_sequence<ncols>{});
    }

    std::size_t size() const { return size_; }

    NANOTINS_HD void store(std::size_t i, const T& row) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((std::get<I>(columns_)[i] = col_at<T, I>::get(row)), ...);
        }(std::make_index_sequence<ncols>{});
    }

    template <std::size_t I>
    const auto& column() const {
        return std::get<I>(columns_);
    }

    soa_ptrs<T> raw() {
        soa_ptrs<T> p;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((std::get<I>(p) = std::get<I>(columns_).data()), ...);
        }(std::make_index_sequence<ncols>{});
        return p;
    }

private:
    typename soa_storage<cols>::type columns_;
    std::size_t size_ = 0;
};

// A non-owning columnar view: a soa_ptrs<T> pack (one elem* per flattened column) + a row count. The
// zero-copy READ primitive — over a fixed soa<T,N>'s buffers (soa.raw(), soa.size()), over borrowed Lance
// page buffers, or as the device-side view a kernel scatters into. Owns nothing; the buffers must outlive
// it. column<I>() yields the I-th column's elem* (read [0, size())).
template <class T>
class soa_view {
public:
    using cols = columns_of<T>;
    static constexpr std::size_t ncols = column_count<T>;

    soa_view() = default;
    soa_view(soa_ptrs<T> ptrs, std::size_t n) : ptrs_(ptrs), size_(n) {}

    std::size_t size() const { return size_; }
    const soa_ptrs<T>& raw() const { return ptrs_; }

    template <std::size_t I>
    auto column() const {
        return std::get<I>(ptrs_);  // elem* into the borrowed buffer
    }

private:
    soa_ptrs<T> ptrs_{};
    std::size_t size_ = 0;
};

// Scatter one row into a soa_ptrs pack at index i — the device-safe twin of soa<T>::store, reading raw
// pointers instead of the owning soa. The be<>/le<>/bits<> conversions happen in each Col::get, so any
// described struct's SoA is fillable by a bulk kernel with zero per-field boilerplate.
template <class T>
NANOTINS_HD void scatter(const soa_ptrs<T>& p, std::size_t i, const T& row) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((std::get<I>(p)[i] = col_at<T, I>::get(row)), ...);
    }(std::make_index_sequence<column_count<T>>{});
}

}  // namespace soatins
