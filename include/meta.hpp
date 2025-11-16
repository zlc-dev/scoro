#pragma once

#include <type_traits>

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
    using type = std::conditional_t<
        (std::is_same_v<T, Ts> || ...), 
        tail,
        typelist_concat_t<TypeList<T>, tail>
    >;
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
