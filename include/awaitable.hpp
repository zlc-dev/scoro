#pragma once

#include "container/queue.hpp"
#include "coro.hpp"
#include "meta.hpp"
#include "expected.hpp"
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <ostream>
#include <print>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace  {

template<typename T>
struct no_rref_storage_t {
    using type = T;
};

template<typename T>
struct no_rref_storage_t<T &&> {
    using type = T;
};

template<typename T>
struct no_rref_storage_t<T &> {
    using type = T &;
};

}

template<typename T>
void print_func() {
    std::println(__PRETTY_FUNCTION__);
}

namespace coro {

template<typename Promise>
struct ThisPromiseAwaitable {
    bool await_ready() {
        return false;
    }

    bool await_suspend(std::coroutine_handle<Promise> h) {
        m_this = &h.promise();
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
struct WaitAnyAwaitable {
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

    WaitAnyAwaitable(Awaitable&&... awaitable)
        : m_awaitables(std::forward<Awaitable>(awaitable)...),
        m_state(std::make_shared<State>())
    {
    }

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
            Awaitable_&& awaitable_, 
            int index, 
            std::shared_ptr<State> state, 
            std::coroutine_handle<> handle
        ) -> TrivialFuture {
            using Value = decltype(std::declval<Awaitable_>().await_resume());
            Awaitable_ awaitable = std::forward<Awaitable_>(awaitable_);
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
                handle.resume();
            } else {
                std::optional<Value> ret {};
                std::exception_ptr exception {};
                try {
                    ret.emplace(co_await awaitable);
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
                } else if (ret) {
                    state->result.template emplace<Value>(std::move(ret.value()));
                } else {
                    state->result.template emplace<std::exception_ptr>(std::make_exception_ptr(std::runtime_error("null exception")));
                }

                handle.resume();
            }
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


template<typename Awaitable>
requires concepts::awaitable<Awaitable, void>
static Awaitable awaitable_cast_strict(Awaitable&& awaitable);

template<typename Awaitable>
static auto awaitable_cast_strict(Awaitable&& awaitable) {
    if constexpr (requires { std::forward<Awaitable>(awaitable).operator co_await(); }) {
        return awaitable_cast_strict(std::forward<Awaitable>(awaitable).operator co_await());
    } else if constexpr (requires { operator co_await(std::forward<Awaitable>(awaitable)); }){
        return awaitable_cast_strict(operator co_await(std::forward<Awaitable>(awaitable)));
    } else {
        static_assert(false, "is not awaitable");
    }
}

template<typename Awaitable>
requires concepts::awaitable<Awaitable, void>
static Awaitable awaitable_cast_strict(Awaitable&& awaitable) {
    return std::forward<Awaitable>(awaitable);
};

template<typename... Awaitable>
auto wait_any(Awaitable&&... awaitable) {
    return WaitAnyAwaitable<decltype(awaitable_cast_strict(std::forward<Awaitable>(awaitable)))...> { 
        awaitable_cast_strict(std::forward<Awaitable>(awaitable))... 
    };
}

template<concepts::awaitable<void>... Awaitable>
struct WaitAllAwaitable {
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

public:
    WaitAllAwaitable(Awaitable&&... awaitable)
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
            Awaitable_&& awaitable_,
            std::shared_ptr<State> state, 
            std::coroutine_handle<> handle
        ) -> TrivialFuture {
            using Value = decltype(std::declval<Awaitable_>().await_resume());
            Awaitable_ awaitable = std::forward<Awaitable_>(awaitable_);
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

template<typename... Awaitable>
WaitAllAwaitable<Awaitable...> wait_all(Awaitable&&... awaitable) {
    return WaitAllAwaitable<decltype(awaitable_cast_strict(std::forward<Awaitable>(awaitable)))...> { 
        awaitable_cast_strict(std::forward<Awaitable>(awaitable))... 
    };
}

template<concepts::awaitable<void>... Awaitable>
struct WaitEachAwaitable {
private:
    using AwaitableResults = typelist_castto_t<std::variant,
        typelist_unique_t<
            typelist_remove_t<
                TypeList<std::monostate, std::exception_ptr, decltype(std::declval<Awaitable>().await_resume())...>,
                void
            >
        >
    >;

    using id_result = std::pair<size_t, AwaitableResults>;

    struct State {
        std::atomic_size_t remaining { sizeof...(Awaitable) };
        std::atomic<void*> coroutine_addr { nullptr };
        nct::AtomicQueue<id_result> results {};
    };

public:

    WaitEachAwaitable(Awaitable&&... awaitable) : m_state(std::make_shared<State>())
    {
        auto runner = []<typename Awaitable_>(
            Awaitable_&& awaitable_, 
            int index, 
            std::shared_ptr<State> state
        ) -> TrivialFuture {
            using Value = decltype(std::declval<Awaitable_>().await_resume());
            Awaitable_ awaitable = std::forward<Awaitable_>(awaitable_);
            if constexpr (std::is_same_v<Value, void>) {
                std::exception_ptr exception {};
                try {
                    co_await awaitable;
                } catch (...) {
                    exception = std::current_exception();
                }
                if (exception != nullptr) {
                    state->results.enqueue(index, std::move(exception));
                } else {
                    state->results.enqueue(index, std::monostate {});
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
                    state->results.enqueue(index, std::move(exception));
                } else if (ret) {
                    state->results.enqueue(index, std::move(ret.value()));
                } else {
                    state->results.enqueue(index, std::make_exception_ptr(std::runtime_error("null exception")));
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
            (runner(std::forward<Awaitable_>(awaitable_), Idx, m_state), ...);
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

    std::optional<id_result> await_resume() {
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
