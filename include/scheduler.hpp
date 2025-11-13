#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <iostream>
#include <print>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace coro {

template<typename T>
concept is_scheduler = requires () {
    T{}.submit(std::coroutine_handle<> {});
};


class TrivialScheduler {
public:
    void submit(std::coroutine_handle<> handle) {
        handle.resume();
    }
};

static_assert(is_scheduler<TrivialScheduler>);

template<is_scheduler Scheduler>
struct SchedAwaiter {
    bool await_ready() {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        if (!handle.done())
            Scheduler{}.submit(handle);
    }

    void await_resume() {}
};

template<is_scheduler Scheduler = TrivialScheduler>
inline SchedAwaiter<Scheduler> sched() {
    return {};
}

} // namespace coro
