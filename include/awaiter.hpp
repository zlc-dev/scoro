#pragma once

#include "timer.hpp"
#include "coro.hpp"
#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <print>
#include <utility>

namespace coro {

template<typename Clock>
struct TimerAwaiter {

    explicit TimerAwaiter(const std::chrono::time_point<Clock>& wake_time)
        : m_wake_time(wake_time)
    {}

    bool await_ready() const noexcept {
        return Clock::now() >= m_wake_time;
    }

    void await_suspend(std::coroutine_handle<> h) const noexcept {
        get_running_timer<Clock>().add_task(
            m_wake_time, 
            [](void* address) {
                std::coroutine_handle<>::from_address(address).resume();
            },
            h.address(), 
            [](void* address) {
                std::coroutine_handle<>::from_address(address).destroy();
            }
        );
    }

    void await_resume() const noexcept {}

private:
    std::chrono::time_point<Clock> m_wake_time;
};


template<typename Clock, typename Duration>
TimerAwaiter<Clock> sleep_until(const std::chrono::time_point<Clock, Duration>& time) {
    return TimerAwaiter<Clock> { std::chrono::time_point_cast<typename Clock::duration>(time) };
}

template<typename Rep, typename Period, typename Clock = std::chrono::steady_clock>
TimerAwaiter<Clock> sleep_for(const std::chrono::duration<Rep, Period>& dur) {
    return TimerAwaiter<Clock> { Clock::now() + dur };
}

template<typename R, typename Clock>
struct TimeoutAwaiter {
private:

    struct State {
        enum {
            ePending = 0,
            eValid,
            eTimeout
        };

        alignas(alignof(R)) std::byte buf[sizeof(R)];
        std::atomic_int state { 0 };
        std::coroutine_handle<> caller;

        ~State() {
            if (state.load(std::memory_order_acquire) == eValid) {
                static_cast<R*>(buf)->~R();
            }  
        }
    };

public:
    template<typename Awaitable>
    TimeoutAwaiter(Awaitable&& awaitable, const std::chrono::time_point<Clock>& timeout)
        : m_state{std::make_shared<State>()}, 
        m_timeout{timeout}, 
        m_future {             
            [] (Awaitable a, std::shared_ptr<State> state) -> TrivialFuture { 
                co_await std::suspend_always {}; // suspend here, resume in await_suspend
                new(state->buf) R(co_await a);
                int expected = State::ePending;
                if (state->state.compare_exchange_strong(
                    expected, State::eValid, 
                    std::memory_order_acq_rel, std::memory_order_relaxed
                )) {
                    state->caller.resume();
                }
            } (std::move(awaitable), m_state)
        }
    {}

    bool await_ready() {
        if (Clock::now() >= m_timeout) [[unlikely]] {
            m_state->state.store(State::eTimeout);
            return true;
        } else {
            return false;
        }
    }

    void await_suspend(std::coroutine_handle<> h) {
        m_state->caller = h;
        m_future.get_coroutine().resume();
        get_running_timer<Clock>().add_task(
            m_timeout,
            [](void* a) {
                auto& state = *static_cast<std::shared_ptr<State>*>(a);
                int expected = State::ePending;
                if (state->state.compare_exchange_strong(
                    expected, State::eTimeout, 
                    std::memory_order_acq_rel, std::memory_order_relaxed
                )) {
                    state->caller.resume();
                }
                delete static_cast<std::shared_ptr<State>*>(a);
            },
            new std::shared_ptr<State>{m_state},
            [](void* a) {
                delete static_cast<std::shared_ptr<State>*>(a);
            }
        );
    }

    std::optional<R> await_resume() {
        switch(m_state->state.load(std::memory_order_acquire)) {
            case State::ePending: std::unreachable();
            case State::eTimeout: return std::nullopt;
            case State::eValid: return std::move(*reinterpret_cast<R*>(m_state.buf));
        }
        std::unreachable();
    }

private:
    std::shared_ptr<State> m_state;
    std::chrono::time_point<Clock> m_timeout;
    TrivialFuture m_future;
};

template<typename Clock>
struct TimeoutAwaiter<void, Clock> {
private:

    struct State {
        enum {
            ePending = 0,
            eValid,
            eTimeout
        };
        std::atomic_int state { ePending };
        std::coroutine_handle<> caller;
    };

public:
    template<typename Awaitable>
    TimeoutAwaiter(Awaitable&& awaitable, const std::chrono::time_point<Clock>& timeout)
        : m_state{std::make_shared<State>()}, 
        m_timeout{timeout},
        m_future { 
            [] (Awaitable a, std::shared_ptr<State> state) -> TrivialFuture { 
                co_await std::suspend_always {}; // suspend here, resume in await_suspend
                co_await a;
                int expected = State::ePending;
                if (state->state.compare_exchange_strong(
                    expected, State::eValid, 
                    std::memory_order_acq_rel, std::memory_order_relaxed
                )) {
                    state->caller.resume();
                }
            } (std::move(awaitable), m_state)
        }
    {}

    bool await_ready() {
        if (Clock::now() >= m_timeout) [[unlikely]] {
            m_state->state.store(State::eTimeout);
            return true;
        } else {
            return false;
        }
    }
    void await_suspend(std::coroutine_handle<> h) {
        m_state->caller = h;
        m_future.get_coroutine().resume();
        get_running_timer<Clock>().add_task(
            m_timeout,
            [](void* a) {
                auto& state = *static_cast<std::shared_ptr<State>*>(a);
                int expected = State::ePending;
                if (state->state.compare_exchange_strong(
                    expected, State::eTimeout, 
                    std::memory_order_acq_rel, std::memory_order_relaxed
                )) {
                    state->caller.resume();
                }
                delete static_cast<std::shared_ptr<State>*>(a);
            },
            new std::shared_ptr<State>{m_state},            
            [](void* a) {
                delete static_cast<std::shared_ptr<State>*>(a);
            }
        );
    }

    bool await_resume() {
        switch(m_state->state.load(std::memory_order_acquire)) {
            case State::ePending: std::unreachable();
            case State::eTimeout: return false;
            case State::eValid: return true;
        }
        std::unreachable();
    }

private:
    std::shared_ptr<State> m_state;
    std::chrono::time_point<Clock> m_timeout;
    TrivialFuture m_future;
};

template<typename Awaiter, typename Clock, typename Dur>
auto with_timeout(Awaiter&& awaiter, const std::chrono::time_point<Clock, Dur>& atime) {
    using R = decltype(std::declval<Awaiter>().await_resume());
    return TimeoutAwaiter<R, Clock> { 
        std::forward<Awaiter>(awaiter), 
        std::chrono::time_point_cast<typename Clock::duration>(atime) 
    };
}

template<typename Awaiter, typename Rep, typename Period>
auto with_timeout(Awaiter&& awaiter, const std::chrono::duration<Rep, Period>& dur) {
    return with_timeout(std::forward<Awaiter>(awaiter), std::chrono::steady_clock::now() + dur);
}

} // namespace coro
