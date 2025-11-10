#pragma once

#include "timer.hpp"
#include "coro.hpp"
#include <chrono>
#include <coroutine>
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
            eValid
        };

        alignas(alignof(R)) std::byte buf[sizeof(R)];
        std::atomic_int state { 0 };
        std::coroutine_handle<> caller;

        ~State() {
            if (state.load(std::memory_order_acquire) == ePending) {
                static_cast<R*>(buf)->~R();
            }  
        }
    };

public:
    template<typename Awaitable>
    TimeoutAwaiter(Awaitable&& awaitable, const std::chrono::time_point<Clock>& timeout)
        : m_state{}, m_timeout{timeout} 
    {
        with_callback(
            std::forward<Awaitable>(awaitable), 
            [state = m_state](R&& value) {
                new(state->buf) R(std::move(value));
                int expected = State::ePending;
                if (!state->state.compare_exchange_strong(
                    expected, State::eValid, 
                    std::memory_order_acq_rel, std::memory_order_relaxed
                )) 
                    return;
                if(state->caller) state->caller.resume();
            }
        );

        get_running_timer<Clock>().add_task(
            timeout,
            [](void* a) {
                auto& state = *static_cast<std::shared_ptr<State>*>(a);
                if(state->state.load(std::memory_order_acquire) == State::ePending && state->caller) 
                    state->caller.resume();
                delete &state;
            },
            new std::shared_ptr<State>{m_state},
            [](void* a) {
                delete static_cast<std::shared_ptr<State>*>(a);
            }
        );
    }

    bool await_ready() {
        return Clock::now() >= m_timeout;
    }

    void await_suspend(std::coroutine_handle<> h) {
        m_state->caller = h;
    }

    std::optional<R> await_resume() {
        switch(m_state->state.load(std::memory_order_acquire)) {
            case State::ePending: return std::nullopt;
            case State::eValid: return std::move(*reinterpret_cast<R*>(m_state.buf));
        }
        std::unreachable();
    }

private:
    std::shared_ptr<State> m_state;
    std::chrono::time_point<Clock> m_timeout;
};

template<typename Clock>
struct TimeoutAwaiter<void, Clock> {
private:

    struct State {
        enum {
            ePending = 0,
            eValid
        };
        std::atomic_int state { ePending };
        std::coroutine_handle<> caller;
    };

public:
    template<typename Awaitable>
    TimeoutAwaiter(Awaitable&& awaitable, const std::chrono::time_point<Clock>& timeout)
        : m_state{std::make_shared<State>()}, m_timeout{timeout} 
    {
        with_callback(
            std::move(awaitable), 
            [state = m_state]() {
                int expected = State::ePending;
                if (!state->state.compare_exchange_strong(
                    expected, State::eValid, 
                    std::memory_order_acq_rel, std::memory_order_relaxed
                )) 
                    return;
                if(state->caller) state->caller.resume();
            }
        );

        get_running_timer<Clock>().add_task(
            timeout,
            [](void* a) {
                auto& state = *static_cast<std::shared_ptr<State>*>(a);
                if(state->state.load(std::memory_order_acquire) == State::ePending && state->caller) 
                    state->caller.resume();
                delete &state;
            },
            new std::shared_ptr<State>{m_state}
        );
    }

    bool await_ready() {
        return Clock::now() >= m_timeout;
    }

    void await_suspend(std::coroutine_handle<> h) {
        m_state->caller = h;
    }

    bool await_resume() {
        switch(m_state->state.load(std::memory_order_acquire)) {
            case State::ePending: return false;
            case State::eValid: return true;
        }
        std::unreachable();
    }

private:
    std::shared_ptr<State> m_state;
    std::chrono::time_point<Clock> m_timeout;
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
