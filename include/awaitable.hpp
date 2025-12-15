#pragma once

#include "container/queue.hpp"
#include "detail/meta.hpp"
#include "expected.hpp"
#include "coro.hpp"
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <print>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace coro {

namespace {
    struct Error {
        std::exception_ptr ep;
    };
}

template<typename Promise>
struct ThisPromiseAwaitable {
    bool await_ready() {
        return false;
    }

    bool await_suspend(std::coroutine_handle<Promise> h) {
        m_this = static_cast<Promise*>(&h.promise());
        return false;
    }

    Promise& await_resume() {
        return *m_this;
    }

private:
    Promise* m_this { nullptr };
};

template<typename Promise>
ThisPromiseAwaitable<Promise> this_promise() {
    return {};
}

template<typename Promise>
std::coroutine_handle<Promise> cast_coroutine(std::coroutine_handle<> handle) {
    return std::coroutine_handle<Promise>::from_address(handle.address());
}

template<concepts::awaitable<void>... Awaitable>
struct AwaitableResultAny {
private:
    using awaitable_list_t = detail::meta::TypeList<Awaitable...>;

    using results_variant_t = detail::meta::typelist_castto_t<std::variant,
        detail::meta::typelist_unique_t<
            detail::meta::TypeList<
                std::monostate, 
                std::conditional_t<
                    (noexcept(std::declval<Awaitable>().await_resume()) && ...), 
                    std::monostate, 
                    Error
                >,
                detail::meta::void_map_t<
                    detail::meta::reference_storage_t<decltype(std::declval<Awaitable>().await_resume())>
                >...
            >
        >
    >;

    template<size_t N>
    inline static constexpr bool is_awaitable_noexcept = noexcept(
        std::declval<detail::meta::typelist_get_t<N, awaitable_list_t>>().await_resume()
    );

    template<size_t N>
    inline static constexpr bool is_return_lref = std::is_lvalue_reference_v<decltype(
        std::declval<detail::meta::typelist_get_t<N, awaitable_list_t>>().await_resume()
    )>;

    using results_typelist_t = detail::meta::TypeList<
            detail::meta::reference_storage_t<decltype(std::declval<Awaitable>().await_resume())>...
        >;

public:
    AwaitableResultAny(): m_results()
    {}

    template<size_t N, typename... Args>
    void emplace(Args&&... args) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        if constexpr (!std::is_same_v<V, void>) {
            m_results.template emplace<V>(std::forward<Args>(args)...);
        } else {
            m_results.template emplace<std::monostate>();
        }
    }

    template<size_t N>
    requires (!is_awaitable_noexcept<N>)
    void emplace_error(std::exception_ptr&& ep) {
        m_results.template emplace<Error>(std::move(ep));
    }

    template<size_t N>
    decltype(auto) get() const& noexcept(is_awaitable_noexcept<N>) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        if constexpr (!is_awaitable_noexcept<N>) {
            if (std::holds_alternative<Error>(m_results)) {
                std::rethrow_exception(std::get<Error>(m_results).ep);
            }
        }
        if constexpr (is_return_lref<N>) {
            return std::get<V>(m_results).get();
        } else if constexpr (!std::is_same_v<V, void>) {
            return std::get<V>(m_results);
        }
    }

    template<size_t N>
    decltype(auto) get() & noexcept(is_awaitable_noexcept<N>) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        if constexpr (!is_awaitable_noexcept<N>) {
            if (std::holds_alternative<Error>(m_results)) {
                std::rethrow_exception(std::get<Error>(m_results).ep);
            }
        }
        if constexpr (is_return_lref<N>) {
            return std::get<V>(m_results).get();
        } else if constexpr (!std::is_same_v<V, void>) {
            return std::get<V>(m_results);
        }
    }

    template<size_t N>
    decltype(auto) get() && noexcept(is_awaitable_noexcept<N>) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        if constexpr (!is_awaitable_noexcept<N>) {
            if (std::holds_alternative<Error>(m_results)) {
                std::rethrow_exception(std::get<Error>(m_results).ep);
            }
        }
        if constexpr (is_return_lref<N>) {
            return std::move(std::get<V>(m_results)).get();
        } else if constexpr (!std::is_same_v<V, void>) {
            return std::move(std::get<V>(m_results));
        }
    }

private:
    results_variant_t m_results;
};

template<concepts::awaitable<void>... Awaitable>
struct WaitAnyAwaitable {
private:

    using AwaitableResults = AwaitableResultAny<Awaitable...>;

    struct State {
        std::atomic_int ready_index { -1 };
        std::atomic<void*> coroutine_addr { nullptr };
        AwaitableResults result {};
    };

public:

    WaitAnyAwaitable(Awaitable&&... awaitable)
        : m_state(std::make_shared<State>())
    {
        auto runner = []<typename Awaitable_, size_t Idx>(
            Awaitable_&& awaitable_, 
            std::in_place_index_t<Idx>,
            std::shared_ptr<State> state
        ) -> TrivialFuture {
            using Value = decltype(std::declval<Awaitable_>().await_resume());
            using MappedValue = detail::meta::reference_storage_t<Value>;
            Awaitable_ awaitable = std::forward<Awaitable_>(awaitable_);
            if constexpr (noexcept(std::declval<Awaitable_>().await_resume())) {
                
                if constexpr (std::is_same_v<Value, void>) {
                    co_await awaitable;
                    int expected = -1;
                    if (!state->ready_index.compare_exchange_strong(
                        expected, Idx, 
                        std::memory_order_release, std::memory_order_relaxed)) {
                        co_return;
                    }
                } else if constexpr (std::is_lvalue_reference_v<Value>) {
                    std::remove_reference_t<Value>* ret = nullptr;
                    ret = &(co_await awaitable);
                    int expected = -1;
                    if (!state->ready_index.compare_exchange_strong(
                        expected, Idx, 
                        std::memory_order_release, std::memory_order_relaxed)) {
                        co_return;
                    }
                    state->result.template emplace<Idx>(*ret);
                } else {
                    std::optional<MappedValue> ret {};
                    ret.emplace(co_await awaitable);
                    int expected = -1;
                    if (!state->ready_index.compare_exchange_strong(
                        expected, Idx, 
                        std::memory_order_release, std::memory_order_relaxed)) {
                        co_return;
                    }
                    state->result.template emplace<Idx>(std::move(ret.value()));
                }
            } else {
                if constexpr (std::is_same_v<Value, void>) {
                    std::exception_ptr exception {};
                    try {
                        co_await awaitable;
                    } catch (...) {
                        exception = std::current_exception();
                    }
                    int expected = -1;
                    if (!state->ready_index.compare_exchange_strong(
                        expected, Idx, 
                        std::memory_order_release, std::memory_order_relaxed)) {
                        co_return;
                    }
                    if (exception != nullptr) {
                        state->result.template emplace_error<Idx>(std::move(exception));
                    }
                } else if constexpr (std::is_lvalue_reference_v<Value>) {
                    std::remove_reference_t<Value>* ret = nullptr;
                    std::exception_ptr exception {};
                    try {
                        ret = &(co_await awaitable);
                    } catch (...) {
                        exception = std::current_exception();
                    }
                    int expected = -1;
                    if (!state->ready_index.compare_exchange_strong(
                        expected, Idx, 
                        std::memory_order_release, std::memory_order_relaxed)) {
                        co_return;
                    }
                    if (exception != nullptr) {
                        state->result.template emplace_error<Idx>(std::move(exception));
                    } else {
                        state->result.template emplace<Idx>(*ret);
                    }
                } else {
                    std::optional<MappedValue> ret {};
                    std::exception_ptr exception {};
                    try {
                        ret.emplace(co_await awaitable);
                    } catch (...) {
                        exception = std::current_exception();
                    }
                    int expected = -1;
                    if (!state->ready_index.compare_exchange_strong(
                        expected, Idx, 
                        std::memory_order_release, std::memory_order_relaxed)) {
                        co_return;
                    }
                    
                    if (exception != nullptr) {
                        state->result.template emplace_error<Idx>(std::move(exception));
                    } else {
                        state->result.template emplace<Idx>(std::move(ret.value()));
                    }
                }
            }
            void *coroutine_addr = state->coroutine_addr.exchange(nullptr, std::memory_order_acq_rel);
            if (coroutine_addr) {
                std::coroutine_handle<>::from_address(coroutine_addr).resume();
            }
        };

        [this, runner]<size_t... Idx, typename... Awaitable_>(std::index_sequence<Idx...>, Awaitable_&&... awaitable_) {
            (runner(std::forward<Awaitable>(awaitable_), std::in_place_index<Idx>, m_state), ...);
        }(std::make_index_sequence<sizeof...(Awaitable)>(), std::forward<Awaitable>(awaitable)...);
    }

    bool await_ready() {
        return m_state->ready_index.load(std::memory_order_acquire) >= 0;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        m_state->coroutine_addr.store(handle.address(), std::memory_order_release);
        if (m_state->ready_index.load(std::memory_order_acquire) >= 0) {
            void *coroutine_addr = m_state->coroutine_addr.exchange(nullptr, std::memory_order_acq_rel);
            if (coroutine_addr) {
                std::coroutine_handle<>::from_address(coroutine_addr).resume();
            }
        }
    }

    std::pair<int, AwaitableResults> await_resume() {
        return { m_state->ready_index, std::move(m_state->result) };
    }

private:
    std::shared_ptr<State> m_state;
};


template<typename... Awaitable>
auto wait_any(Awaitable&&... awaitable) {
    return WaitAnyAwaitable<decltype(awaitable_cast_strict(std::forward<Awaitable>(awaitable)))...> { 
        awaitable_cast_strict(std::forward<Awaitable>(awaitable))... 
    };
}

template<concepts::awaitable<void>... Awaitable>
struct AwaitableResultAllView;

template<concepts::awaitable<void>... Awaitable>
struct AwaitableResultAll {
private:
    using awaitable_list_t = detail::meta::TypeList<Awaitable...>;

    using results_typelist_t = detail::meta::TypeList<
        detail::meta::reference_storage_t<decltype(std::declval<Awaitable>().await_resume())>...
    >;

    template<typename A>
    struct ReslutMapper {
        using T = detail::meta::reference_storage_t<decltype(std::declval<A>().await_resume())>;
        using type = detail::meta::typelist_castto_t<std::variant, 
            detail::meta::typelist_unique_t<
                detail::meta::TypeList<
                    std::monostate, detail::meta::void_map_t<T>,
                    std::conditional_t<
                        noexcept(std::declval<A>().await_resume()),
                        std::monostate,
                        Error
                    >
                >
            >
        >;
    };

    using results_tuple_t = detail::meta::typelist_castto_t<
        std::tuple, 
        detail::meta::typelist_map_t<ReslutMapper, awaitable_list_t>
    >;
    
    template<size_t N>
    inline static constexpr bool is_awaitable_noexcept = noexcept(
        std::declval<detail::meta::typelist_get_t<N, awaitable_list_t>>().await_resume()
    );

    template<size_t N>
    inline static constexpr bool is_return_lref = std::is_lvalue_reference_v<decltype(
        std::declval<detail::meta::typelist_get_t<N, awaitable_list_t>>().await_resume()
    )>;

public:

    AwaitableResultAll(): m_results(detail::meta::dependent_value<Awaitable, std::in_place_type<std::monostate>>...) {}

    template<size_t N, typename... Args>
    void emplace(Args&&... args) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        std::get<N>(m_results).template emplace<V>( std::forward<Args>(args)... );
    }

    template<size_t N>
    requires (!is_awaitable_noexcept<N>)
    void emplace_error(std::exception_ptr&& ep) {
        std::get<N>(m_results).template emplace<Error>(std::move(ep));
    }

    template<size_t N>
    decltype(auto) get() & noexcept(is_awaitable_noexcept<N>) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        if constexpr (!is_awaitable_noexcept<N>) {
            if (std::holds_alternative<Error>(std::get<N>(m_results))) {
                std::rethrow_exception(std::get<Error>(std::get<N>(m_results)).ep);
            }
        }
        if constexpr (is_return_lref<N>) {
            return std::get<V>(std::get<N>(m_results)).get();
        } else if constexpr (!std::is_same_v<V, void>) {
            return std::get<V>(std::get<N>(m_results));
        }
    }

    template<size_t N>
    decltype(auto) get() const & noexcept(is_awaitable_noexcept<N>) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        if constexpr (!is_awaitable_noexcept<N>) {
            if (std::holds_alternative<Error>(std::get<N>(m_results))) {
                std::rethrow_exception(std::get<Error>(std::get<N>(m_results)).ep);
            }
        }
        if constexpr (is_return_lref<N>) {
            return std::get<V>(std::get<N>(m_results)).get();
        } else if constexpr (!std::is_same_v<V, void>) {
            return std::get<V>(std::get<N>(m_results));
        }
    }
    
    template<size_t N>
    decltype(auto) get() && noexcept(is_awaitable_noexcept<N>) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        if constexpr (!is_awaitable_noexcept<N>) {
            if (std::holds_alternative<Error>(std::get<N>(m_results))) {
                std::rethrow_exception(std::get<Error>(std::get<N>(m_results)).ep);
            }
        }
        if constexpr (is_return_lref<N>) {
            return std::move(std::get<V>(std::get<N>(m_results))).get();
        } else if constexpr (!std::is_same_v<V, void>) {
            return std::move(std::get<V>(std::get<N>(m_results)));
        }
    }

    AwaitableResultAllView<Awaitable...>&& view() &&;

private:
    results_tuple_t m_results;    
};

template<typename T>
struct Expected {
public:
    template<typename... Args>
    Expected(std::in_place_type_t<T>, Args&&... args)
        : m_value(std::in_place_type<T>, std::forward<Args>(args)...) {}

    Expected(std::in_place_type_t<Error>, std::exception_ptr&& ep)
        : m_value(std::in_place_type<Error>, std::move(ep)) {}


    T& get() & {
        if (std::holds_alternative<Error>(m_value)) {
            std::rethrow_exception(std::get<Error>(m_value).ep);
        }
        return std::get<T>(m_value);
    }

    const T& get() const& {
        if (std::holds_alternative<Error>(m_value)) {
            std::rethrow_exception(std::get<Error>(m_value).ep);
        }
        return std::get<T>(m_value);
    }

    T&& get() && {
        if (std::holds_alternative<Error>(m_value)) {
            std::rethrow_exception(std::get<Error>(m_value).ep);
        }
        return std::move(std::get<T>(m_value));
    }

private:
    std::variant<T, Error> m_value;
};

template<typename T>
struct Expected<T&> {
public:
    Expected(std::in_place_type_t<T&>, T& t)
        : m_value(std::in_place_type<T*>, &t) {}

    Expected(std::in_place_type_t<Error>, std::exception_ptr&& ep)
        : m_value(std::in_place_type<Error>, std::move(ep)) {}

    const T& get() const {
        if (std::holds_alternative<Error>(m_value)) {
            std::rethrow_exception(std::get<Error>(m_value).ep);
        }
        return *std::get<T*>(m_value);
    }

    T& get() {
        if (std::holds_alternative<Error>(m_value)) {
            std::rethrow_exception(std::get<Error>(m_value).ep);
        }
        return *std::get<T*>(m_value);
    }

private:
    std::variant<T*, Error> m_value;
};

template<typename T>
struct Expected<T&&> {
public:
    Expected(std::in_place_type_t<T&&>, T&& t)
        : m_value(std::in_place_type<T>, std::move(t)) {}

    Expected(std::in_place_type_t<Error>, std::exception_ptr&& ep)
        : m_value(std::in_place_type<Error>, std::move(ep)) {}

    T&& get() {
        if (std::holds_alternative<Error>(m_value)) {
            std::rethrow_exception(std::get<Error>(m_value).ep);
        }
        return std::move(std::get<T>(m_value));
    }

private:
    std::variant<T, Error> m_value;
};

template<>
struct Expected<void> {
public:
    Expected()
        : m_error(nullptr) {}

    Expected(std::exception_ptr&& ep)
        : m_error(std::move(ep)) {}

    void get() {
        if (m_error) {
            std::rethrow_exception(m_error);
        }
    }

private:
    std::exception_ptr m_error;
};

template<concepts::awaitable<void>... Awaitable>
struct AwaitableResultAllView: AwaitableResultAll<Awaitable...>{

    using base = AwaitableResultAll<Awaitable...>;

    using awaitable_list_t = detail::meta::TypeList<Awaitable...>;

    template<size_t N>
    using raw_result_t = decltype(
        std::declval<detail::meta::typelist_get_t<N, awaitable_list_t>>().await_resume()
    );

    template<size_t N>
    inline static constexpr bool is_awaitable_noexcept = noexcept(
        std::declval<detail::meta::typelist_get_t<N, awaitable_list_t>>().await_resume()
    );

    template<size_t N>
    decltype(auto) get() && noexcept {
        using R = raw_result_t<N>;
        if constexpr (is_awaitable_noexcept<N>) {
            if constexpr (std::is_same_v<R, void>) {
                base::template get<N>();
                return std::monostate {};
            } else if constexpr (std::is_rvalue_reference_v<R>) {
                return std::move(base::template get<N>());
            } else if constexpr (std::is_lvalue_reference_v<R>){
                return base::template get<N>();
            } else {
                return R { std::move(base::template get<N>()) };
            }
        } else {
            if constexpr (std::is_same_v<R, void>) {
                try {
                    base::template get<N>();
                    return Expected<void> {};
                } catch(...) {
                    return Expected<void> { std::current_exception() };
                }
            } else {
                try {
                    return Expected<R> { std::in_place_type<R>, std::forward<R>(base::template get<N>())};
                } catch (...) {
                    return Expected<R> { std::in_place_type<Error>, std::current_exception() };
                }
            }
        }
    }
};

template<concepts::awaitable<void>... Awaitable>
AwaitableResultAllView<Awaitable...>&& AwaitableResultAll<Awaitable...>::view() && {
    return std::move(*static_cast<AwaitableResultAllView<Awaitable...>*>(this));
}

} // namespace coro

namespace std {
    template<coro::concepts::awaitable<void>... Awaitable>
    struct tuple_size<coro::AwaitableResultAllView<Awaitable...>> : integral_constant<size_t, sizeof...(Awaitable)> {};
    
    template<size_t I, coro::concepts::awaitable<void>... Awaitable>
    struct tuple_element<I, coro::AwaitableResultAllView<Awaitable...>> {
        using type = decltype(declval<coro::AwaitableResultAllView<Awaitable...>>().template get<I>());
    };
} // namespace std

namespace coro {

template<concepts::awaitable<void>... Awaitable>
struct WaitAllAwaitable {
private:
    using AwaitableResults = AwaitableResultAll<Awaitable...>;

    struct State {
        std::atomic_size_t waiting_count {sizeof...(Awaitable)};
        std::atomic<void*> coroutine_addr { nullptr };
        AwaitableResults result {};
    };

public:
    WaitAllAwaitable(Awaitable&&... awaitable)
        : m_state(std::make_shared<State>())
    {
        auto runner = []<typename Awaitable_, size_t Idx>(
            Awaitable_&& awaitable_,
            std::in_place_index_t<Idx>,
            std::shared_ptr<State> state
        ) -> TrivialFuture {
            using Value = decltype(std::declval<Awaitable_>().await_resume());
            Awaitable_ awaitable = std::forward<Awaitable_>(awaitable_);
            if constexpr (noexcept(std::declval<Awaitable_>().await_resume())) {
                if constexpr (std::is_same_v<Value, void>) {
                    co_await awaitable;
                } else {
                    state->result.template emplace<Idx>(co_await awaitable);
                }
            } else {
                try {
                    if constexpr (std::is_same_v<Value, void>) {
                        co_await awaitable;
                    } else {
                        state->result.template emplace<Idx>(co_await awaitable);
                    }
                } catch (...) {
                    state->result.template emplace_error<Idx>(std::current_exception());
                }
            }
            if (state->waiting_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {   
                void *coroutine_addr = state->coroutine_addr.exchange(nullptr, std::memory_order_acq_rel);
                if (coroutine_addr) {
                    std::coroutine_handle<>::from_address(coroutine_addr).resume();
                }
            }
        };

        [this, runner]<size_t... Idx, typename... Awaitable_>(std::index_sequence<Idx...>, Awaitable_&&... awaitable_) {
            ((runner(std::forward<Awaitable>(awaitable_), std::in_place_index<Idx>, m_state)), ...);
        }(std::make_index_sequence<sizeof...(Awaitable)>(), std::forward<Awaitable>(awaitable)...);
    }

    bool await_ready() {
        return m_state->waiting_count.load(std::memory_order_acquire) == 0;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        m_state->coroutine_addr.store(handle.address(), std::memory_order_release);
        if (m_state->waiting_count.load(std::memory_order_acquire) == 0) {
            void *coroutine_addr = m_state->coroutine_addr.exchange(nullptr, std::memory_order_acq_rel);
            if (coroutine_addr) {
                std::coroutine_handle<>::from_address(coroutine_addr).resume();
            }
        }
    }

    AwaitableResults await_resume() {
        return std::move(m_state->result);
    }

private:
    std::shared_ptr<State> m_state;
};

template<typename... Awaitable>
auto wait_all(Awaitable&&... awaitable) {
    return WaitAllAwaitable<decltype(awaitable_cast_strict(std::forward<Awaitable>(awaitable)))...> { 
        awaitable_cast_strict(std::forward<Awaitable>(awaitable))... 
    };
}

template<concepts::awaitable<void>... Awaitable>
struct AwaitableResultEach {
private:
    using awaitable_list_t = detail::meta::TypeList<Awaitable...>;

    using results_variant_t = detail::meta::typelist_castto_t<std::variant,
        detail::meta::typelist_unique_t<
            detail::meta::TypeList<
                std::conditional_t<
                    (noexcept(std::declval<Awaitable>().await_resume()) && ...), 
                    std::monostate, 
                    Error
                >,
                detail::meta::void_map_t<
                    detail::meta::reference_storage_t<decltype(std::declval<Awaitable>().await_resume())>
                >...
            >
        >
    >;

    template<size_t N>
    inline static constexpr bool is_awaitable_noexcept = noexcept(
        std::declval<detail::meta::typelist_get_t<N, awaitable_list_t>>().await_resume()
    );

    template<size_t N>
    inline static constexpr bool is_return_lref = std::is_lvalue_reference_v<decltype(
        std::declval<detail::meta::typelist_get_t<N, awaitable_list_t>>().await_resume()
    )>;

    using results_typelist_t = detail::meta::TypeList<
            detail::meta::reference_storage_t<decltype(std::declval<Awaitable>().await_resume())>...
        >;

public:
    template<size_t N, typename... Args>
    AwaitableResultEach(std::in_place_index_t<N>, Args&&... args):
        m_results(
            std::in_place_type<detail::meta::void_map_t<detail::meta::typelist_get_t<N, results_typelist_t>>>, 
            std::forward<Args>(args)...
        )
    {
    }

    AwaitableResultEach(std::in_place_type_t<Error>, std::exception_ptr&& ep):
        m_results(std::in_place_type<Error>, std::move(ep))
    {
    }

    template<size_t N>
    decltype(auto) get() const& noexcept(is_awaitable_noexcept<N>) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        if constexpr (!is_awaitable_noexcept<N>) {
            if (std::holds_alternative<Error>(m_results)) {
                std::rethrow_exception(std::get<Error>(m_results).ep);
            }
        }
        if constexpr (is_return_lref<N>) {
            return std::get<V>(m_results).get();
        } else if constexpr (!std::is_same_v<V, void>) {
            return std::get<V>(m_results);
        }
    }

    template<size_t N>
    decltype(auto) get() & noexcept(is_awaitable_noexcept<N>) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        if constexpr (!is_awaitable_noexcept<N>) {
            if (std::holds_alternative<Error>(m_results)) {
                std::rethrow_exception(std::get<Error>(m_results).ep);
            }
        }
        if constexpr (is_return_lref<N>) {
            return std::get<V>(m_results).get();
        } else if constexpr (!std::is_same_v<V, void>) {
            return std::get<V>(m_results);
        }
    }

    template<size_t N>
    decltype(auto) get() && noexcept(is_awaitable_noexcept<N>) {
        using V = detail::meta::typelist_get_t<N, results_typelist_t>;
        if constexpr (!is_awaitable_noexcept<N>) {
            if (std::holds_alternative<Error>(m_results)) {
                std::rethrow_exception(std::get<Error>(m_results).ep);
            }
        }
        if constexpr (is_return_lref<N>) {
            return std::move(std::get<V>(m_results)).get();
        } else if constexpr (!std::is_same_v<V, void>) {
            return std::move(std::get<V>(m_results));
        }
    }

private:
    results_variant_t m_results;
};

template<concepts::awaitable<void>... Awaitable>
struct WaitEachAwaitable {
private:
    using id_result_t = std::pair<size_t, AwaitableResultEach<Awaitable...>>;

    struct State {
        std::atomic_size_t remaining { sizeof...(Awaitable) };
        std::atomic<void*> coroutine_addr { nullptr };
        nct::AtomicQueue<id_result_t> results {};
    };

public:
    using result_t = AwaitableResultEach<Awaitable...>;

    WaitEachAwaitable(Awaitable&&... awaitable) : m_state(std::make_shared<State>())
    {
        auto runner = []<typename Awaitable_, size_t Idx>(
            Awaitable_&& awaitable_, 
            std::in_place_index_t<Idx>,
            std::shared_ptr<State> state
        ) noexcept -> TrivialFuture {
            using Value = decltype(std::declval<Awaitable_>().await_resume());
            using MappedValue = detail::meta::reference_storage_t<Value>;
            constexpr bool nothrow = noexcept(std::declval<Awaitable_>().await_resume());
            Awaitable_ awaitable = std::forward<Awaitable_>(awaitable_);
            if constexpr (nothrow) {
                if constexpr (std::is_same_v<Value, void>) {
                    co_await awaitable;
                    state->results.enqueue(Idx, result_t{std::in_place_index<Idx>});
                } else if constexpr (std::is_lvalue_reference_v<Value>) {
                    state->results.enqueue(Idx, result_t{std::in_place_index<Idx>, MappedValue { co_await awaitable }});
                } else {
                    state->results.enqueue(Idx, result_t{std::in_place_index<Idx>, co_await awaitable});
                }
            } else {
                if constexpr (std::is_same_v<Value, void>) {
                    std::exception_ptr exception {};
                    try {
                        co_await awaitable;
                    } catch (...) {
                        exception = std::current_exception();
                    }
                    if (exception != nullptr) {
                        state->results.enqueue(Idx, result_t{std::in_place_type<Error>, std::move(exception)});
                    } else {
                        state->results.enqueue(Idx, result_t{std::in_place_index<Idx>});
                    }
                } else if constexpr (std::is_lvalue_reference_v<Value>) {
                    std::remove_reference_t<Value>* ret = nullptr;
                    std::exception_ptr exception {};
                    try {
                        ret = &(co_await awaitable);
                    } catch (...) {
                        exception = std::current_exception();
                    }
                    if (exception != nullptr) {
                        state->results.enqueue(Idx, result_t{std::in_place_type<Error>, std::move(exception)});
                    } else {
                        state->results.enqueue(Idx, result_t{std::in_place_index<Idx>, MappedValue { *ret }});
                    }
                } else {
                    std::optional<Value> ret {};
                    std::exception_ptr exception {};
                    try {
                        ret.emplace(co_await awaitable);
                    } catch (...) {
                        exception = std::current_exception();
                    }
                    if (exception != nullptr) {
                        state->results.enqueue(Idx, result_t{std::in_place_type<Error>, std::move(exception)});
                    } else {
                        state->results.enqueue(Idx, result_t{std::in_place_index<Idx>, std::move(ret.value())});
                    }
                }
            }
            state->remaining.fetch_sub(1, std::memory_order_release);
            // 一定要先存结果再尝试唤醒，使用release内存序进行保证
            if (void* coroutine_addr = state->coroutine_addr.exchange(nullptr, std::memory_order_acq_rel); coroutine_addr) {
                std::coroutine_handle<> handle = std::coroutine_handle<>::from_address(coroutine_addr);
                handle.resume();
            }
        };

        [this, runner]<size_t... Idx, typename... Awaitable_>(std::index_sequence<Idx...>, Awaitable_&&... awaitable_) {
            (runner(std::forward<Awaitable_>(awaitable_), std::in_place_index<Idx>, m_state), ...);
        }(std::make_index_sequence<sizeof...(Awaitable)>(), std::forward<Awaitable>(awaitable)...);
    }

    bool await_ready() {
        // 如果剩下的任务数量为0或结果队列非空，则协程不休眠
        return m_state->remaining.load(std::memory_order_acquire) == 0 || !m_state->results.maybe_empty();
    }

    void await_suspend(std::coroutine_handle<> handle) {
        m_state->coroutine_addr.store(handle.address(), std::memory_order_release);
        // maybe_empty只可能在正在插入时误判，而唤醒在插入之后，所以这里如果误判了也不会漏唤醒，因为正在插入的线程稍后会唤醒
        if (!m_state->results.maybe_empty()) {
            // 如果队列非空（这是准确的)，则尝试唤醒正在等待的handle，exchange保证不会重复唤醒
            if (void* coroutine_addr = m_state->coroutine_addr.exchange(nullptr, std::memory_order_acq_rel); coroutine_addr) {
                std::coroutine_handle<> handle = std::coroutine_handle<>::from_address(coroutine_addr);
                handle.resume();
            }
        }
    }

    std::optional<id_result_t> await_resume() {
        return m_state->results.try_dequeue();
    }

public:
    std::shared_ptr<State> m_state;
};

template<typename... Awaitable>
auto wait_each(Awaitable&&... awaitable) {
    return WaitEachAwaitable<decltype(awaitable_cast_strict(std::forward<Awaitable>(awaitable)))...> { 
        awaitable_cast_strict(std::forward<Awaitable>(awaitable))... 
    };
}

} // namespace coro
