#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <iostream>
#include <mutex>
#include <optional>
#include <print>
#include <queue>
#include <type_traits>
#include <vector>
#include "awaitable.hpp"
#include "coro.hpp"
#include "ptr/intrusive_ptr.hpp"
#include "ptr/rc_ptr.hpp"

namespace coro {

using callback_t = void(*)(void* arg);

template<typename Clk>
class Timer {
public:
    using Clock = Clk;

    struct Task {
        std::chrono::time_point<Clk> execute_time;
        callback_t callback;
        void* arg;
        callback_t cancel;
    };

    Timer() = default;
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    void add_task(const std::chrono::time_point<Clk>& expire_time, callback_t cb, void* arg = nullptr, callback_t cancel = nullptr) {
        std::lock_guard<std::mutex> _locker {m_mutex};
        m_tasks.push(Task{expire_time, cb, arg, cancel});
        m_cv.notify_one();
    }

    template<typename Rep, typename Period>
    void add_task_after(const std::chrono::duration<Rep, Period>& delay, callback_t cb, void* arg = nullptr, callback_t cancel = nullptr) {
        add_task(Clk::now() + delay, cb, arg, cancel);
    }

    void run() {
        std::unique_lock<std::mutex> locker { m_mutex };
        while (!m_stop.load(std::memory_order_acquire)) {
            while (!m_tasks.empty() && m_tasks.top().execute_time <= Clk::now()) {
                Task task = m_tasks.top();
                m_tasks.pop();
                locker.unlock();
                try {
                    task.callback(task.arg);
                } catch (const std::exception& e) {
                    std::println(std::cerr, "unhandled exception: {}", e.what());
                } catch (...) {
                    std::println(std::cerr, "unhandled unknown exception");
                }
                locker.lock();
            }

            if (m_tasks.empty()) {
                m_cv.wait(locker);
            } else if (m_tasks.top().execute_time > Clk::now()) {
                m_cv.wait_until(locker, m_tasks.top().execute_time - std::chrono::milliseconds(1));
            }
        }
    }

    void stop() {
        std::lock_guard<std::mutex> _locker { m_mutex };
        while (!m_tasks.empty()) {
            if(m_tasks.top().cancel) m_tasks.top().cancel(m_tasks.top().arg);
            m_tasks.pop();
        }
        m_stop.store(true, std::memory_order_release);
        m_cv.notify_all();
    }

private:

    struct TaskCompare {
        bool operator()(const Task& lhs, const Task& rhs) const noexcept {
            return lhs.execute_time > rhs.execute_time;
        }
    };

    std::priority_queue<Task, std::vector<Task>, TaskCompare> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic_bool m_stop;
};


template<typename Clk>
class TimerRunner: public Timer<Clk> {
public:
    using Clock = typename Timer<Clk>::Clock;

    TimerRunner(): Timer<Clk>(), m_runner([this](){ Timer<Clk>::run(); }) {}
    
    ~TimerRunner() {
        Timer<Clk>::stop();
        if (m_runner.joinable())
            m_runner.join();
    }

private:
    std::thread m_runner;
};

namespace detail {
    
    template<typename T>
    struct TimeoutAwaitableState {
        enum {
            ePending = 0,
            eValid,
            eError,
            eTimeout
        };

        alignas(alignof(T)) std::byte buf[sizeof(T)];
        std::exception_ptr exception { nullptr };
        std::atomic_int state { 0 };
        std::coroutine_handle<> caller { nullptr };

        ~TimeoutAwaitableState() {
            if (state.load(std::memory_order_acquire) == eValid) {
                reinterpret_cast<T*>(buf)->~T();
            }  
        }
    };

    template<>
    struct TimeoutAwaitableState<void> {
        enum {
            ePending = 0,
            eValid,
            eError,
            eTimeout
        };

        std::exception_ptr exception { nullptr };
        std::atomic_int state { 0 };
        std::coroutine_handle<> caller { nullptr };
    };
}

template<typename Timer>
class TimerWrapper: public Timer {

    struct SleepAwaitable {
        using Clock = typename Timer::Clock;

        auto operator co_await() && {
            struct SleepAwaitableInner {
                using Clock = typename Timer::Clock;

                bool await_ready() const noexcept {
                    return Clock::now() >= m_wake_time;
                }

                void await_suspend(std::coroutine_handle<> h) const noexcept {
                    m_timer.add_task(
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

                std::chrono::time_point<Clock> m_wake_time;
                TimerWrapper<Timer>& m_timer;
            };

            return SleepAwaitableInner(m_wake_time, m_timer);
        }

        std::chrono::time_point<Clock> m_wake_time;
        TimerWrapper<Timer>& m_timer;
    };

    struct CancelableSleepAwaitable {
        using Clock = typename Timer::Clock;

        struct ControlBox: intrusive_ref_counter_mt<ControlBox> {
            enum State {
                ePending,
                eCanceled,
                eConsumed
            };
            std::coroutine_handle<> waiter { nullptr };
            std::atomic<State> state { ePending };
        };

        CancelableSleepAwaitable(
            std::chrono::time_point<Clock> wake_time,
            TimerWrapper<Timer>& timer
        ): m_wake_time(wake_time), m_timer(timer), m_cb(make_intrusive<ControlBox>()) {}

        bool cancel() {
            typename ControlBox::State expected = ControlBox::ePending;
            if(m_cb->state.compare_exchange_strong(
                expected, ControlBox::eCanceled, 
                std::memory_order_acq_rel, std::memory_order_relaxed
            )) {
                if(m_cb->waiter) {
                    m_cb->waiter.resume();
                    m_cb->waiter = nullptr;
                }
                return true;
            }
            return false;
        }

        bool await_ready() const noexcept {
            return Clock::now() >= m_wake_time || m_cb->state.load(std::memory_order_acquire) != ControlBox::ePending;
        }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            m_cb->waiter = h;
            intrusive_ptr_add_ref(m_cb.get());
            m_timer.add_task(
                m_wake_time,
                [](void* address) {
                    intrusive_ptr<ControlBox> cb { static_cast<ControlBox*>(address) };
                    intrusive_ptr_release(cb.get());
                    typename ControlBox::State expected = ControlBox::ePending;
                    if (cb->state.compare_exchange_strong(
                        expected, ControlBox::eConsumed, 
                        std::memory_order_acq_rel, std::memory_order_relaxed
                    )) {
                        if(cb->waiter) {
                            cb->waiter.resume();
                            cb->waiter = nullptr;
                        }
                    }
                },
                m_cb.get(), 
                [](void* address) {
                    intrusive_ptr<ControlBox> cb { static_cast<ControlBox*>(address) };
                    intrusive_ptr_release(cb.get());
                    typename ControlBox::State expected = ControlBox::ePending;
                    if(cb->state.compare_exchange_strong(
                        expected, ControlBox::eCanceled, 
                        std::memory_order_acq_rel, std::memory_order_relaxed
                    )) {
                        if(cb->waiter) {
                            cb->waiter.resume();
                            cb->waiter = nullptr;
                        }
                    }
                }
            );
        }

        bool await_resume() const noexcept {
            return m_cb->state.load(std::memory_order_acquire) == ControlBox::eConsumed;
        }

        std::chrono::time_point<Clock> m_wake_time;
        TimerWrapper<Timer>& m_timer;
        intrusive_ptr<ControlBox> m_cb;
    };

    template<typename R>
    struct TimeoutAwaitable {
    private:

        using State = typename detail::TimeoutAwaitableState<R>;
        using Clock = typename Timer::Clock;

    public:
        using return_type = std::conditional_t<std::is_same_v<R, void>, bool, std::optional<R>>;

        template<typename Awaitable>
        TimeoutAwaitable(Awaitable&& awaitable, const std::chrono::time_point<Clock>& timeout, TimerWrapper<Timer>& timer)
            : m_state{std::make_shared<State>()}, 
            m_timeout{timeout}, 
            m_future {             
                [] (Awaitable a, std::shared_ptr<State> state) -> coro::TrivialFuture { 
                    co_await std::suspend_always {}; // suspend here, resume in await_suspend
                    int next_state = State::eValid;
                    try {
                        if constexpr (std::is_same_v<R, void>) 
                            co_await a;
                        else {
                            R r = co_await a;
                            new(state->buf) R(std::move(r));
                        }
                    } catch (...) {
                        state->exception = std::current_exception();
                        next_state = State::eError;
                    }
                    int expected = State::ePending;
                    if (state->state.compare_exchange_strong(
                        expected, next_state, 
                        std::memory_order_acq_rel, std::memory_order_relaxed
                    )) {
                        state->caller.resume();
                    }
                } (std::move(awaitable), m_state)
            },
            m_timer(timer)
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
            m_timer.add_task(
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

        return_type await_resume() {
            switch(m_state->state.load(std::memory_order_acquire)) {
                case State::ePending: std::unreachable();
                case State::eTimeout:
                    if constexpr (std::is_same_v<R, void>)
                        return false;
                    else
                        return std::nullopt;
                case State::eValid: 
                    if constexpr (std::is_same_v<R, void>)
                        return true;
                    else
                        return std::move(*reinterpret_cast<R*>(m_state->buf));
                case State::eError: std::rethrow_exception(m_state->exception);
            }
            std::unreachable();
        }

    private:
        std::shared_ptr<State> m_state;
        std::chrono::time_point<Clock> m_timeout;
        coro::TrivialFuture m_future;
        TimerWrapper<Timer>& m_timer;
    };

    template<typename Awaitable>
    using TimeoutFuture = Future<typename TimeoutAwaitable<decltype(std::declval<Awaitable>().await_resume())>::return_type>;

public:
    using Clock = typename Timer::Clock;
    using Timer::Timer;

    auto sleep_until(const std::chrono::time_point<Clock>& wake_time) {
        return SleepAwaitable { wake_time, *this };
    }

    template<typename Rep, typename Period>
    auto sleep_for(const std::chrono::duration<Rep, Period>& dur) {
        return sleep_until(Clock::now() + dur);
    }

    auto sleep_until_cancelable(const std::chrono::time_point<Clock>& wake_time) {
        return CancelableSleepAwaitable { wake_time, *this };
    }

    template<typename Rep, typename Period>
    auto sleep_for_cancelable(const std::chrono::duration<Rep, Period>& dur) {
        return sleep_until_cancelable(Clock::now() + dur);
    }

    template<typename Awaitable>
    TimeoutFuture<Awaitable> with_timeout_until(Awaitable&& awaitable, const std::chrono::time_point<Clock>& timeout) {
        using Ret = decltype(std::declval<Awaitable>().await_resume());
        auto r = co_await TimeoutAwaitable<Ret> { awaitable_cast_strict(std::forward<Awaitable>(awaitable)), timeout, *this };
        co_return std::move(r);
    }

    template<typename Awaitable, typename Rep, typename Period>
    auto with_timeout_for(Awaitable&& awaitable, const std::chrono::duration<Rep, Period>& duration) {
        return with_timeout_until(awaitable_cast_strict(std::forward<Awaitable>(awaitable)), Clock::now() + duration);
    }

};

inline TimerWrapper<TimerRunner<std::chrono::steady_clock>>& get_global_timer() {
    static TimerWrapper<TimerRunner<std::chrono::steady_clock>> timer {};
    return timer;
}

} // namespace coro
