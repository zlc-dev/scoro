#pragma once

#include "waitable_atomic.hpp"
#include <atomic>
#include <cassert>
#include <coroutine>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <print>
#include <type_traits>
#include <utility>

namespace coro {

// No ownership, no return values, no exception storage.
struct TrivialPromise {
public:
    inline TrivialPromise() noexcept {}
    std::coroutine_handle<TrivialPromise> get_return_object() { return std::coroutine_handle<TrivialPromise>::from_promise(*this);}
    inline std::suspend_never initial_suspend() noexcept { return {}; }
    inline std::suspend_never final_suspend() noexcept { return {}; }
    inline void unhandled_exception() {
        try { 
            std::rethrow_exception(std::current_exception()); 
        } catch (const std::exception& e) { 
            std::println(std::cerr, "unhandled exception: {}", e.what()); 
        } catch (...) {
            std::println(std::cerr, "unhandled exception");
        }
    }
    inline void return_void() {}
};

struct [[maybe_unused]] TrivialFuture {
    using promise_type = TrivialPromise;
    TrivialFuture(std::coroutine_handle<promise_type> h) {}
};

template<
    typename Awaitable, 
    typename Callback, 
    typename ECallback = void(*)(std::exception_ptr), 
    typename FCallback = void(*)()
>
TrivialFuture with_callback(
    Awaitable awaitable,
    Callback callback,
    ECallback exception_callback = static_cast<void(*)(std::exception_ptr)>([](std::exception_ptr) {}),
    FCallback finally_callback = static_cast<void(*)()>([]{})
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
template<typename T>
struct Promise;

struct PromiseBase {
public:
    
    using InitialWaiter = std::suspend_never;

    struct FinalWaiter {
        inline bool await_ready() noexcept { return false; }
        template<typename T>
        inline std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise<T>> h) noexcept {
            std::coroutine_handle<> ret = h.promise().get_caller() ? h.promise().get_caller() : std::noop_coroutine();
            return ret;
        }
        inline void await_resume() noexcept {}
    };

public:
    inline PromiseBase() noexcept {}

    inline InitialWaiter initial_suspend() noexcept { return {}; }
    inline FinalWaiter final_suspend() noexcept { return {}; }
    inline void unhandled_exception() {
        m_exception = std::current_exception();
    }

    inline void set_caller(std::coroutine_handle<> caller) {
        m_caller_coroutine = caller;
    }

    inline std::coroutine_handle<> get_caller() const {
        return m_caller_coroutine;
    }

protected:
    inline void throw_exception() const {
        if (m_exception) std::rethrow_exception(m_exception);
    }

private:
    std::exception_ptr m_exception {};
    std::coroutine_handle<> m_caller_coroutine {};
};

template <typename T>
struct Promise: public PromiseBase {

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    void return_value(T&& value) {
        m_value.emplace(std::move(value));
    }

    void return_value(const T& value) {
        m_value.emplace(value);
    }

    T get() {
        throw_exception();
        return std::move(m_value.value());
    }

private:
    std::optional<T> m_value { std::nullopt };
};

template<>
struct Promise<void>: public PromiseBase {

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    void return_void() {}

    void get() {
        throw_exception();
    }
};

template<typename T>
struct [[nodiscard]] Future {
public:
    using promise_type = Promise<T>;

    Future(std::coroutine_handle<promise_type> coroutine)
        : m_coroutine(coroutine) {};

    Future(const Future&) = delete;
    Future(Future&& oth)
    : m_coroutine(std::exchange(oth.m_coroutine, nullptr)) {
    }

    ~Future() {
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

    T await_resume() {
        return m_coroutine.promise().get();
    }

    T get() {
        return m_coroutine.promise().get();
    }

protected:
    std::coroutine_handle<promise_type> m_coroutine;
};

template<typename T>
struct WaitablePromise;

struct WaitablePromiseBase {
public:

    using InitialWaiter = std::suspend_never;

    struct FinalWaiter {
        inline bool await_ready() noexcept { return false; }
        template<typename T>
        inline std::coroutine_handle<> await_suspend(std::coroutine_handle<WaitablePromise<T>> h) noexcept {
            h.promise().set_valid();
            std::coroutine_handle<> ret = h.promise().get_caller() ? h.promise().get_caller() : std::noop_coroutine();
            return ret;
        }
        inline void await_resume() noexcept {}
    };

public:
    WaitablePromiseBase() = default;
    
    inline InitialWaiter initial_suspend() noexcept { return {}; }
    inline FinalWaiter final_suspend() noexcept { return {}; }
    inline void unhandled_exception() {
        m_exception = std::current_exception();
    }

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
    inline void set_caller(std::coroutine_handle<> caller) {
        m_caller = caller;
    }

    inline std::coroutine_handle<> get_caller() const {
        return m_caller;
    }

protected:
    inline void throw_exception() const {
        if (m_exception) std::rethrow_exception(m_exception);
    }

    inline void set_valid() {
        m_state.store(1, std::memory_order_release);
        m_state.notify_all();
    }

private:
    std::exception_ptr m_exception {};
    waitable_atomic_int m_state {};
    std::coroutine_handle<> m_caller {};
};

template <typename T>
struct WaitablePromise: public WaitablePromiseBase {

    std::coroutine_handle<WaitablePromise> get_return_object() {
        return std::coroutine_handle<WaitablePromise>::from_promise(*this);
    }

    void return_value(T&& value) {
        m_value.emplace(std::move(value));
    }

    void return_value(const T& value) {
        m_value.emplace(value);
    }

    T get() {
        throw_exception();
        return std::move(m_value.value());
    }

private:
    std::optional<T> m_value { std::nullopt };
};

template<>
struct WaitablePromise<void>: public WaitablePromiseBase {
    std::coroutine_handle<WaitablePromise> get_return_object() {
        return std::coroutine_handle<WaitablePromise>::from_promise(*this);
    }

    void return_void() {}

    void get() {
        throw_exception();
    }
};


// Supports co_await
// destruct without co_await is dangerous !
template <typename T>
struct [[nodiscard]] DetachedWaitableFuture {
public:    
    using promise_type = WaitablePromise<T>;

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
template<typename T>
struct [[nodiscard]] WaitableFuture {
public:
    using promise_type = WaitablePromise<T>;

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

    bool valid() {
        return m_coroutine.promise().valid();
    }

    T get() {
        return m_coroutine.promise().get();
    }

    void wait() {
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

    DetachedWaitableFuture<T> detach() && {
        return { std::exchange(m_coroutine, nullptr) };
    }

private:
    std::coroutine_handle<promise_type> m_coroutine;
};

template<typename Awaitable, typename R = decltype(std::declval<Awaitable>().await_resume())>
WaitableFuture<R> make_waitable(Awaitable awaitable) {
    co_return co_await awaitable;
}

} // namespace coro
