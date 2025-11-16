#pragma once

#include "coro.hpp"
#include "meta.hpp"
#include "expected.hpp"
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace coro {

template<typename Promise>
struct ThisCoroAwaiter {
    bool await_ready() {
        return false;
    }

    bool await_suspend(std::coroutine_handle<Promise> h) {
        m_this = h;
        return false;
    }

    std::coroutine_handle<Promise> await_resume() {
        return m_this;
    }

private:
    std::coroutine_handle<Promise> m_this { nullptr };
};

template<typename Promise = void>
ThisCoroAwaiter<Promise> this_coroutine() {
    return {};
}

template<typename Promise>
std::coroutine_handle<Promise> cast_coroutine(std::coroutine_handle<> handle) {
    return std::coroutine_handle<Promise>::from_address(handle.address());
}

template<concepts::awaitable<void>... Awaitable>
struct WaitAnyAwaiter {
private:
    using AwaitableResults = typelist_castto_t<std::variant,
        typelist_unique_t<
            typelist_remove_t<
                TypeList<std::monostate, std::exception_ptr, decltype(std::declval<Awaitable>().await_resume())...>,
                void
            >
        >
    >;

    struct State {
        std::atomic_int ready_index { -1 };
        AwaitableResults result { std::monostate{} };
    };

public:

    WaitAnyAwaiter(Awaitable&&... awaitable)
        : m_awaitables(std::forward<Awaitable>(awaitable)...),
        m_state(std::make_shared<State>())
    {}

    bool await_ready() {
        auto checker = [this]<typename Awaitable_>(
            Awaitable_& awaitable, 
            int index
        ) -> bool {
            using Value = decltype(std::declval<Awaitable_>().await_resume());
            if (!awaitable.await_ready()) return false;
            m_state->ready_index = index;
            try {
                if constexpr (std::is_same_v<Value, void>)
                    awaitable.await_resume();
                else
                    m_state->result.template emplace<Value>(awaitable.await_resume());
            } catch (...) {
                m_state->result.template emplace<std::exception_ptr>(std::current_exception());
            }
            return true;
        };

        return [this, checker]<size_t... Idx>(std::index_sequence<Idx...>) {
            return (checker(std::get<Idx>(m_awaitables), Idx) || ...);
        }(std::make_index_sequence<sizeof...(Awaitable)>());
    }

    void await_suspend(std::coroutine_handle<> handle) {
        auto runner = []<typename Awaitable_>(
            Awaitable_&& awaitable, 
            int index, 
            std::shared_ptr<State> state, 
            std::coroutine_handle<> handle
        ) -> TrivialFuture {
            using Value = decltype(std::declval<Awaitable_>().await_resume());
            if constexpr (std::is_same_v<Value, void>) {
                std::exception_ptr exception {};
                try {
                    co_await awaitable;
                } catch (...) {
                    exception = std::current_exception();
                }
                int expected = -1;
                if (!state->ready_index.compare_exchange_strong(
                    expected, index, 
                    std::memory_order_release, std::memory_order_relaxed)) {
                    co_return;
                }
                if (exception != nullptr) {
                    state->result.template emplace<std::exception_ptr>(std::move(exception));
                }
            } else {
                alignas(Value) std::byte ret_buf [sizeof(Value)];
                std::exception_ptr exception {};
                try {
                    auto ret = co_await awaitable;
                    new(ret_buf) Value(std::move(ret));
                } catch (...) {
                    exception = std::current_exception();
                }
                int expected = -1;
                if (!state->ready_index.compare_exchange_strong(
                    expected, index, 
                    std::memory_order_release, std::memory_order_relaxed)) {
                    co_return;
                }
                if (exception != nullptr) {
                    state->result.template emplace<std::exception_ptr>(std::move(exception));
                } else {
                    state->result.template emplace<Value>(std::move(*reinterpret_cast<Value*>(ret_buf)));
                    if constexpr (!std::is_trivially_destructible_v<Value>)
                        reinterpret_cast<Value*>(ret_buf)->~Value();
                }
            }
            handle.resume();
        };

        [this, handle, runner]<size_t... Idx>(std::index_sequence<Idx...>) {
            (runner(std::forward<Awaitable>(std::get<Idx>(m_awaitables)), Idx, m_state, handle), ...);
        }(std::make_index_sequence<sizeof...(Awaitable)>());
    }

    std::pair<int, AwaitableResults> await_resume() {
        return { m_state->ready_index, std::move(m_state->result) };
    }

private:
    std::tuple<Awaitable...> m_awaitables;
    std::shared_ptr<State> m_state;
};

template<concepts::awaitable<void>... Awaitable>
WaitAnyAwaiter<Awaitable...> wait_any(Awaitable&&... awaitable) {
    return { std::forward<Awaitable>(awaitable)... };
}

template<concepts::awaitable<void>... Awaitable>
struct WaitAllAwaiter {
private:

    template<typename T>
    struct ToState {
        using type = std::conditional_t<
            std::is_same_v<T, void>,
            std::variant<std::monostate, std::exception_ptr>,
            typelist_castto_t<std::variant, 
                typelist_unique_t<TypeList<std::monostate, T, std::exception_ptr>>
            >
        >;
    };

    template<typename T>
    struct ToResult {
        using type = tl::expected<T, std::exception_ptr>;
    };

public:

    using AwaitableResults = typelist_castto_t<std::tuple,
        typelist_map_t<ToState,
            TypeList<decltype(std::declval<Awaitable>().await_resume())...>
        >
    >;

    using Results = typelist_castto_t<std::tuple,
        typelist_map_t<ToResult,
            TypeList<decltype(std::declval<Awaitable>().await_resume())...>
        >
    >;

    struct State {
        std::atomic_size_t waiting_count {sizeof...(Awaitable)};
        AwaitableResults result {};
    };

    WaitAllAwaiter(Awaitable&&... awaitable)
        : m_awaitables(std::forward<Awaitable>(awaitable)...),
        m_state(std::make_shared<State>())
    {}

    bool await_ready() {
        auto checker = [this]<size_t Idx, typename Awaitable_>(
            std::in_place_index_t<Idx>,
            Awaitable_& awaitable
        ) -> bool {
            using Value = decltype(std::declval<Awaitable_>().await_resume());
            if (!awaitable.await_ready()) return false;
            m_state->waiting_count -= 1;
            try {
                if constexpr (std::is_same_v<Value, void>) {
                    awaitable.await_resume();
                } else {
                    std::get<Idx>(m_state->result).template emplace<Value>(awaitable.await_resume());
                }
            } catch (...) {
                std::get<Idx>(m_state->result).template emplace<std::exception_ptr>(std::current_exception());
            }
            return true;
        };

        return [this, checker]<size_t... Idx>(std::index_sequence<Idx...>) {
            return ((checker(std::in_place_index<Idx>, std::get<Idx>(m_awaitables))) && ...);
        }(std::make_index_sequence<sizeof...(Awaitable)>());
    }

    void await_suspend(std::coroutine_handle<> handle) {
        auto runner = []<size_t Idx, typename Awaitable_>(
            std::in_place_index_t<Idx>,
            Awaitable_&& awaitable,
            std::shared_ptr<State> state, 
            std::coroutine_handle<> handle
        ) -> TrivialFuture {
            using Value = decltype(std::declval<Awaitable_>().await_resume());
            try {
                if constexpr (std::is_same_v<Value, void>) {
                    co_await awaitable;
                } else {
                    auto ret = co_await awaitable;
                    std::get<Idx>(state->result).template emplace<Value>(std::move(ret));
                }
            } catch (...) {
                std::get<Idx>(state->result).template emplace<std::exception_ptr>(std::current_exception());
            }
            if (state->waiting_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                handle.resume();
            }
        };

        [this, handle, runner]<size_t... Idx>(std::index_sequence<Idx...>) {
            ((runner(std::in_place_index<Idx>, std::forward<Awaitable>(std::get<Idx>(m_awaitables)), m_state, handle)), ...);
        }(std::make_index_sequence<sizeof...(Awaitable)>());
    }

    Results await_resume() {
        return [this]<size_t... Idx>(std::index_sequence<Idx...>) {
            return std::make_tuple(variant_to_expected(std::move(std::get<Idx>(m_state->result)))...);
        }(std::make_index_sequence<sizeof...(Awaitable)>());
    }

private:

    template<class T, class E>
    static tl::expected<T, E> variant_to_expected(std::variant<std::monostate, T, E>&& v) {
        switch (v.index()) {
            case 0:
                std::unreachable();
            case 1:
                return std::move(std::get<1>(v));
            case 2:
                return tl::unexpected(std::move(std::get<2>(v)));
        }
        std::unreachable();
    }

    template<class E>
    static tl::expected<void, E> variant_to_expected(std::variant<std::monostate, E>&& v) {
        switch (v.index()) {
            case 0:
                return {};
            case 1:
                return tl::unexpected(std::move(std::get<1>(v)));
        }
        std::unreachable();
    }

    std::tuple<Awaitable...> m_awaitables;
    std::shared_ptr<State> m_state;
};

template<concepts::awaitable<void>... Awaitable>
WaitAllAwaiter<Awaitable...> wait_all(Awaitable&&... awaitable) {
    return { std::forward<Awaitable>(awaitable)... };
}

} // namespace coro
