#pragma once

#include <functional>
#include <type_traits>
#include <variant>

namespace detail::meta {


template<typename, auto U>
inline static constexpr auto dependent_value = U;

template<typename T>
inline constexpr bool dependent_false = dependent_value<T, false>;

template<typename T>
struct ReferenceStorage {
    using type = T;
};

template<typename T>
struct ReferenceStorage<T&> {
    using type = std::reference_wrapper<T>;
};

template<typename T>
struct ReferenceStorage<T&&> {
    using type = T;
};

template<typename T>
using reference_storage_t = typename ReferenceStorage<T>::type;

template<typename T>
struct DecayRvalue {
    using type = T;
};

template<typename T>
struct DecayRvalue<T&> {
    using type = T&;
};

template<typename T>
struct DecayRvalue<T&&> {
    using type = T;
};

template<typename T>
using decay_rvalue_t = typename DecayRvalue<T>::type;

template<typename T>
using void_map_t = std::conditional_t<std::is_same_v<T, void>, std::monostate, T>;

template<typename... T>
struct TypeList {};

template<typename L, typename R>
struct TypeListConcat {};

template<typename... Ts, typename... Us>
struct TypeListConcat<TypeList<Ts...>, TypeList<Us...>> {
    using type = TypeList<Ts..., Us...>;
};

template<typename L, typename R>
using typelist_concat_t = typename TypeListConcat<L, R>::type;

template<typename T, typename U>
struct TypeListRemove {};

template<typename U>
struct TypeListRemove<TypeList<>, U> {
    using type = TypeList<>;
};

template<typename T, typename... Ts, typename U>
struct TypeListRemove<TypeList<T, Ts...>, U> {
private:
    using tail = typename TypeListRemove<TypeList<Ts...>, U>::type;
public:
    using type = std::conditional_t<
        std::is_same_v<T, U>, 
        tail, 
        typelist_concat_t<TypeList<T>, tail>
    >;
};

template<typename T, typename U>
using typelist_remove_t = typename TypeListRemove<T, U>::type;

template<typename T>
struct TypeListUnique {};

template<>
struct TypeListUnique<TypeList<>> {
public:
    using type = TypeList<>;
};

template<typename T, typename... Ts>
struct TypeListUnique<TypeList<T, Ts...>> {
private:
    using tail = typename TypeListUnique<TypeList<Ts...>>::type;
public:
    using type = typelist_concat_t<TypeList<T>, typelist_remove_t<tail, T>>;
};

template<typename T>
using typelist_unique_t = typename TypeListUnique<T>::type;

template<template <typename...> typename C, typename T>
struct TypeListCastTo {};

template<template <typename...> typename C, typename... Ts>
struct TypeListCastTo<C, TypeList<Ts...>> {
    using type = C<Ts...>;
};

template<template <typename...> typename C, typename T>
using typelist_castto_t = typename TypeListCastTo<C, T>::type;

template<template<typename> typename M, typename T>
struct TypeListMap {};

template<template<typename> typename M, typename... Ts>
struct TypeListMap<M, TypeList<Ts...>> {
public:
    using type = TypeList<typename M<Ts>::type...>;
};

template<template <typename> typename M, typename T>
using typelist_map_t = typename TypeListMap<M, T>::type;

template<typename>
struct TypeListSize;

template<typename... Ts>
struct TypeListSize<TypeList<Ts...>>
    : std::integral_constant<size_t, sizeof...(Ts)> {};

template<typename T>
inline constexpr size_t typelist_size_v = TypeListSize<T>::value;

template<size_t N, typename List>
    requires (N < typelist_size_v<List>)
struct TypeListGet;

template<size_t N, typename... Ts>
struct TypeListGet<N, TypeList<Ts...>> {
    using type = std::tuple_element_t<N, std::tuple<Ts...>>;
};

template<size_t N, typename T>
using typelist_get_t = typename TypeListGet<N, T>::type;

}
