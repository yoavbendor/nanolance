#pragma once

// Bridges boost::describe (host-only, not CUDA-annotated) into plain `std` constexpr objects that
// device code can consume: a tuple of member pointers and an array of names. boost stays on the host;
// the `store` fold below touches only std::tuple / std::array / member pointers.

#include <array>
#include <cstddef>
#include <tuple>

#include <boost/describe/members.hpp>
#include <boost/mp11/algorithm.hpp>
#include <boost/mp11/list.hpp>

namespace soatins {

// describe_members yields boost::describe::detail::list<...>, so match any L<D...> rather than
// hardcoding mp_list.
template <class T>
inline constexpr auto nt_members = [] {
    using Md = boost::describe::describe_members<T, boost::describe::mod_public>;
    return []<template <class...> class L, class... D>(L<D...>) {
        return std::tuple{D::pointer...};
    }(Md{});
}();

template <class T>
inline constexpr auto nt_names = [] {
    using Md = boost::describe::describe_members<T, boost::describe::mod_public>;
    return []<template <class...> class L, class... D>(L<D...>) {
        return std::array<const char*, sizeof...(D)>{D::name...};
    }(Md{});
}();

template <class T>
inline constexpr std::size_t nt_field_count = std::tuple_size_v<decltype(nt_members<T>)>;

// The K-th member pointer as a constexpr value usable as a template NTTP.
template <class T, std::size_t K>
inline constexpr auto nt_member_ptr = std::get<K>(nt_members<T>);

// Type of the member that pointer-to-member `MP` designates.
template <class C, class M>
M nt_pointee(M C::*);
template <auto MP>
using nt_pointee_t = decltype(nt_pointee(MP));

}  // namespace soatins
