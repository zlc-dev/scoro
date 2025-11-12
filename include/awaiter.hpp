#pragma once

#include "scheduler.hpp"
#include "coro.hpp"
#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <memory>
#include <print>
#include <type_traits>
#include <utility>

namespace coro {

template<typename Promise>
struct ThisCoroAwaiter {
    bool await_ready() {
        return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
        m_this = h;
        return h;
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

} // namespace coro
