#pragma once

#include "waitable_atomic.hpp"
#include <atomic>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <print>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace coro {

namespace concepts {

namespace detail {

    template<typename T>
    concept await_suspend_result = 
        std::same_as<T, void> 
        || std::same_as<T, bool> 
        || std::convertible_to<T, std::coroutine_handle<>>;

} // namespace detail

template<typename T, typename Promise>
concept awaitable = requires (T t, std::coroutine_handle<Promise> h) {
    { t.await_ready() } -> std::convertible_to<bool>;
    { t.await_suspend(h) } -> detail::await_suspend_result;
    t.await_resume();
};

} // namespace concepts

// No ownership, no return values, no exception storage.
struct TrivialPromise {
public:
    inline TrivialPromise() noexcept {}
    std::coroutine_handle<TrivialPromise> get_return_object() { return std::coroutine_handle<TrivialPromise>::from_promise(*this);}
    inline std::suspend_never initial_suspend() noexcept { return {}; }
    inline std::suspend_never final_suspend() noexcept { return {}; }
    inline void unhandled_exception() noexcept { std::rethrow_exception(std::current_exception()); }
    inline void return_void() {}
};

struct [[maybe_unused]] TrivialFuture {
    using promise_type = TrivialPromise;
    TrivialFuture(std::coroutine_handle<promise_type> h): m_coroutine(h) {}
    TrivialFuture(const TrivialFuture& oth): m_coroutine(oth.m_coroutine) {}
    std::coroutine_handle<promise_type> get_coroutine() {
        return m_coroutine;
    }

private:
    std::coroutine_handle<promise_type> m_coroutine;
};

template<typename Awaitable, typename Callback>
TrivialFuture with_callback(
    Awaitable awaitable,
    Callback callback
) noexcept {
    using R = decltype(std::declval<Awaitable>().await_resume());

    static_assert(std::is_invocable_v<decltype(callback), R> || 
              (std::is_same_v<R, void> && std::is_invocable_v<decltype(callback)>),
              "Callback must match awaitable result type");

    if constexpr (std::is_same_v<R, void>) {
        co_await std::move(awaitable);
        callback();
    } else {
        auto result = co_await std::move(awaitable);
        callback(std::move(result));
    }
}

template<
    typename Awaitable, typename Callback, typename ECallback
>
TrivialFuture with_callback(
    Awaitable awaitable,
    Callback callback,
    ECallback exception_callback
) noexcept {
    using R = decltype(std::declval<Awaitable>().await_resume());

    static_assert(std::is_invocable_v<decltype(callback), R> || 
              (std::is_same_v<R, void> && std::is_invocable_v<decltype(callback)>),
              "Callback must match awaitable result type");

    static_assert(std::is_invocable_v<decltype(exception_callback), std::exception_ptr>,
                "ExceptionCallback must accept std::exception_ptr");

    try {
        if constexpr (std::is_same_v<R, void>) {
            co_await std::move(awaitable);
            callback();
        } else {
            auto result = co_await std::move(awaitable);
            callback(std::move(result));
        }
    } catch (...) {
        exception_callback(std::current_exception());
    }
}

template<
    typename Awaitable, typename Callback, typename ECallback, typename FCallback>
TrivialFuture with_callback(
    Awaitable awaitable,
    Callback callback,
    ECallback exception_callback,
    FCallback finally_callback
) noexcept {
    using R = decltype(std::declval<Awaitable>().await_resume());

    static_assert(std::is_invocable_v<decltype(callback), R> || 
              (std::is_same_v<R, void> && std::is_invocable_v<decltype(callback)>),
              "Callback must match awaitable result type");

    static_assert(std::is_invocable_v<decltype(exception_callback), std::exception_ptr>,
                "ExceptionCallback must accept std::exception_ptr");

    static_assert(std::is_invocable_v<decltype(finally_callback)>,
                "FinallyCallback must be callable");

    try {
        if constexpr (std::is_same_v<R, void>) {
            co_await std::move(awaitable);
            callback();
        } else {
            auto result = co_await std::move(awaitable);
            callback(std::move(result));
        }
    } catch (...) {
        exception_callback(std::current_exception());
    }
    finally_callback();
}

template<typename Awaitable>
TrivialFuture forget(Awaitable awaitable) noexcept {
    co_await awaitable;
}

// Holds ownership, supports return values,
// Stores exceptions and rethrows them on get() or await_resume().
template<typename T, bool Nothrow>
struct Promise;

template<bool Nothrow>
struct PromiseBase;

template<>
struct PromiseBase<true> {
public:
    
    using InitialWaiter = std::suspend_never;

    struct FinalWaiter {
        inline bool await_ready() noexcept { return false; }
        template<typename Promise>
        inline std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
            std::coroutine_handle<> ret = h.promise().get_caller() ? h.promise().get_caller() : std::noop_coroutine();
            return ret;
        }
        inline void await_resume() noexcept {}
    };

public:

    inline InitialWaiter initial_suspend() noexcept { return {}; }
    inline FinalWaiter final_suspend() noexcept { return {}; }
    inline void unhandled_exception() noexcept { std::rethrow_exception(std::current_exception()); }

    inline void set_caller(std::coroutine_handle<> caller) {
        m_caller_coroutine = caller;
    }

    inline std::coroutine_handle<> get_caller() const {
        return m_caller_coroutine;
    }

private:
    std::coroutine_handle<> m_caller_coroutine {};
};


template<>
struct PromiseBase<false> : PromiseBase<true> {
public:
    inline void unhandled_exception() {
        m_exception = std::current_exception();
    }

protected:
    inline void throw_exception() const {
        if (m_exception) std::rethrow_exception(m_exception);
    }

private:
    std::exception_ptr m_exception {};
};

template <typename T, bool Nothrow>
struct Promise: public PromiseBase<Nothrow> {

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    void return_value(T&& value) {
        m_value.emplace(std::move(value));
    }

    void return_value(const T& value) {
        m_value.emplace(value);
    }

    T get() noexcept(Nothrow) {
        if constexpr (!Nothrow)
            PromiseBase<Nothrow>::throw_exception();
        return std::move(m_value.value());
    }

private:
    std::optional<T> m_value { std::nullopt };
};

template<bool Nothrow>
struct Promise<void, Nothrow>: public PromiseBase<Nothrow> {

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    void return_void() {}

    void get() noexcept(Nothrow) {
        if constexpr (!Nothrow)
            PromiseBase<Nothrow>::throw_exception();
    }
};

template<typename T, bool Nothrow = false>
struct [[nodiscard]] Future {
public:
    using promise_type = Promise<T, Nothrow>;

    Future(std::coroutine_handle<promise_type> coroutine) noexcept
        : m_coroutine(coroutine) {};

    Future(const Future&) = delete;
    Future(Future&& oth) noexcept
    : m_coroutine(std::exchange(oth.m_coroutine, nullptr)) {
    }

    ~Future() noexcept {
        if (m_coroutine) {
            m_coroutine.destroy();
        }
    }

    bool await_ready() noexcept {
        return m_coroutine.done();
    }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        m_coroutine.promise().set_caller(h);
    }

    T await_resume() noexcept(Nothrow) {
        return m_coroutine.promise().get();
    }

    T get() noexcept(Nothrow) {
        return m_coroutine.promise().get();
    }

    bool done() const noexcept {
        return m_coroutine.done();
    }

protected:
    std::coroutine_handle<promise_type> m_coroutine;
};

template<typename T, bool Nothrow>
struct WaitablePromise;

template<bool Nothrow>
struct WaitablePromiseBase: PromiseBase<Nothrow> {
public:

    struct FinalWaiter {
        inline bool await_ready() noexcept { return false; }
        template<typename Promise>
        inline std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
            h.promise().set_valid();
            std::coroutine_handle<> ret = h.promise().get_caller() ? h.promise().get_caller() : std::noop_coroutine();
            return ret;
        }
        inline void await_resume() noexcept {}
    };

public:
    WaitablePromiseBase() = default;

    inline FinalWaiter final_suspend() noexcept { return {}; }

    inline bool valid() const noexcept {
        return m_state.load() != 0;
    }

    inline void wait() const noexcept {
        m_state.wait(0, std::memory_order_acquire);
    }

    template <typename Rep, typename Period>
    bool wait_for(const std::chrono::duration<Rep, Period>& dur) const noexcept {
        return m_state.wait_for(0, dur, std::memory_order_acquire);
    }

    template <typename Clock, typename Dur>
    bool wait_until(const std::chrono::time_point<Clock, Dur>& atime) const noexcept {
        return m_state.wait_until(0, atime, std::memory_order_acquire);
    }

protected:
    inline void set_valid() noexcept {
        m_state.store(1, std::memory_order_release);
        m_state.notify_all();
    }

private:
    waitable_atomic_int m_state {};
};

template <typename T, bool Nothrow>
struct WaitablePromise: public WaitablePromiseBase<Nothrow> {

    std::coroutine_handle<WaitablePromise> get_return_object() {
        return std::coroutine_handle<WaitablePromise>::from_promise(*this);
    }

    void return_value(T&& value) {
        m_value.emplace(std::move(value));
    }

    void return_value(const T& value) {
        m_value.emplace(value);
    }

    T get() noexcept(Nothrow) {
        if constexpr (!Nothrow)
            WaitablePromiseBase<Nothrow>::throw_exception();
        return std::move(m_value.value());
    }

private:
    std::optional<T> m_value { std::nullopt };
};

template<bool Nothrow>
struct WaitablePromise<void, Nothrow>: public WaitablePromiseBase<Nothrow> {
    std::coroutine_handle<WaitablePromise> get_return_object() {
        return std::coroutine_handle<WaitablePromise>::from_promise(*this);
    }

    void return_void() {}

    void get() noexcept(Nothrow) {
        if constexpr (!Nothrow)
            WaitablePromiseBase<Nothrow>::throw_exception();
    }
};


// Supports co_await
// destruct without co_await is dangerous !
template <typename T, bool Nothrow>
struct [[nodiscard]] DetachedWaitableFuture {
public:    
    using promise_type = WaitablePromise<T, Nothrow>;

    DetachedWaitableFuture(std::coroutine_handle<promise_type> coroutine)
    : m_coroutine(coroutine) {}

    ~DetachedWaitableFuture() {
        if (m_coroutine) {
            m_coroutine.destroy();
        }
    }

    DetachedWaitableFuture(DetachedWaitableFuture&& oth)
    : m_coroutine(std::exchange(oth.m_coroutine, nullptr)) {}

    inline bool await_ready() noexcept { return m_coroutine.done(); }

    inline void await_suspend(std::coroutine_handle<> h) noexcept {
        m_coroutine.promise().set_caller(h);
    }

    inline T await_resume() {
        return m_coroutine.promise().get();
    }
private:
    std::coroutine_handle<promise_type> m_coroutine;
};

// Supports wait/wait_for/wait_until.
// Blocks on destruction unless cancel() is called.
template<typename T, bool Nothrow = false>
struct [[nodiscard]] WaitableFuture {
public:
    using promise_type = WaitablePromise<T, Nothrow>;

    WaitableFuture(std::coroutine_handle<promise_type> coroutine)
    : m_coroutine(coroutine) {}

    WaitableFuture(const WaitableFuture&) = delete;

    WaitableFuture(WaitableFuture&& oth)
    : m_coroutine(std::exchange(oth.m_coroutine, nullptr)) {}

    ~WaitableFuture() {
        if (m_coroutine) {
            wait();
            m_coroutine.destroy();
        }
    }

    bool valid() const noexcept {
        return m_coroutine.promise().valid();
    }
    
    bool done() const noexcept {
        return m_coroutine.done();
    }

    T get() noexcept(Nothrow) {
        return m_coroutine.promise().get();
    }

    void wait() const noexcept {
        m_coroutine.promise().wait();
    }

    template <typename Rep, typename Period>
    bool wait_for(const std::chrono::duration<Rep, Period>& dur) const noexcept {
        return m_coroutine.promise().wait_for(dur);
    }

    template <typename Clock, typename Dur>
    bool wait_until(const std::chrono::time_point<Clock, Dur>& atime) const noexcept {
        return m_coroutine.promise().wait_until(atime);
    }

    DetachedWaitableFuture<T, Nothrow> detach() && {
        return { std::exchange(m_coroutine, nullptr) };
    }

protected:
    std::coroutine_handle<promise_type> m_coroutine;
};

template<
    typename Awaitable, bool Nothrow = false,
    typename R = decltype(std::declval<Awaitable>().await_resume())
>
WaitableFuture<R, Nothrow> make_waitable(Awaitable awaitable) {
    co_return co_await awaitable;
}

struct CancelablePromiseBase {
public:

    inline void cancel() noexcept {
        int expected_state = m_state.load(std::memory_order_acquire);
        for(;;) {
            switch (expected_state) {
                case eUnused:
                case ePending: 
                    if (m_state.compare_exchange_strong(expected_state, eCanceled, 
                        std::memory_order_acq_rel, std::memory_order_relaxed)) 
                    {
                        return;
                    }
                    break;
                case eCanceled:
                    return;
                case eWaiting:
                    if (m_state.compare_exchange_strong(expected_state, eCanceled, 
                        std::memory_order_acq_rel, std::memory_order_relaxed)) 
                    {
                        m_waiter.resume();
                        return;
                    }
                    break;
                default:
                    return;
            }
        }
    }

    inline auto wait_cancel() {
        struct CancelAwaitable {
            CancelAwaitable(CancelablePromiseBase& promise)
                : m_promise(promise) {}

            CancelAwaitable(const CancelAwaitable&) = delete;

            CancelAwaitable(CancelAwaitable&& oth) noexcept: m_promise(oth.m_promise) {};

            bool await_ready() {
                return m_promise.get_canceled();
            }

            bool await_suspend(std::coroutine_handle<> handle) {
                return m_promise.set_cancel_waiter(handle);
            }

            void await_resume() {}
        private:
            CancelablePromiseBase& m_promise;
        };
        int state = eUnused;
        if(!m_state.compare_exchange_strong(state, ePending, 
            std::memory_order_acq_rel, std::memory_order_relaxed)
        ) {
            if (state == eCanceled) {
                return std::optional<CancelAwaitable>{std::nullopt};
            } else {
                throw std::runtime_error("Repeated wait for cancellation");
            }
        }
        return std::optional<CancelAwaitable>{ CancelAwaitable{ *this } };
    }

private:

    inline bool set_cancel_waiter(std::coroutine_handle<> waiter) noexcept {
        m_waiter = waiter;
        int expected_state = ePending;
        return m_state.compare_exchange_strong(
            expected_state, eWaiting, 
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    inline bool get_canceled() const noexcept {
        return m_state.load(std::memory_order_acquire) == eCanceled;
    }

    enum CancelState {
        eUnused,
        ePending,
        eCanceled,
        eWaiting
    };

    std::atomic_int m_state { eUnused };
    std::coroutine_handle<> m_waiter { nullptr };
};

template<typename T, bool Nothrow = false>
struct CancelablePromise: public Promise<T, Nothrow>, private CancelablePromiseBase {
public:
    using Promise<T, Nothrow>::Promise;

    using CancelablePromiseBase::cancel;
    using CancelablePromiseBase::wait_cancel;

};

template<typename T, bool Nothrow = false>
struct CancelableFuture: public Future<T, Nothrow> {
    using promise_type = CancelablePromise<T, Nothrow>;
    using Future<T>::Future;

    void cancel() noexcept {
        std::coroutine_handle<promise_type> handle = 
            std::coroutine_handle<promise_type>::from_address(Future<T>::m_coroutine.address());
        promise_type& promise = handle.promise();
        promise.cancel();
    }
};


template<typename T, bool Nothrow = false>
struct CancelableWaitablePromise: public WaitablePromise<T, Nothrow>, private CancelablePromiseBase {
public:
    using WaitablePromise<T, Nothrow>::WaitablePromise;

    using CancelablePromiseBase::cancel;
    using CancelablePromiseBase::wait_cancel;
};


template<typename T, bool Nothrow = false>
struct CancelableWaitableFuture: public WaitableFuture<T, Nothrow> {
    using promise_type = CancelableWaitablePromise<T>;
    using WaitableFuture<T, Nothrow>::WaitableFuture;

    void cancel() noexcept {
        std::coroutine_handle<promise_type> handle = 
            std::coroutine_handle<promise_type>::from_address(WaitableFuture<T, Nothrow>::m_coroutine.address());
        promise_type& promise = handle.promise();
        promise.cancel();
    }
};

} // namespace coro
