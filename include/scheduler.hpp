#pragma once

#include <coroutine>
#include <print>

namespace coro {

namespace concepts {
    template<typename T>
    concept scheduler = requires () {
        T{}.submit(std::coroutine_handle<> {});
    };
}

class TrivialScheduler {
public:
    void submit(std::coroutine_handle<> handle) {
        handle.resume();
    }
};

static_assert(concepts::scheduler<TrivialScheduler>);

template<concepts::scheduler Scheduler>
struct SchedAwaitable {
    bool await_ready() {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        if (!handle.done())
            Scheduler{}.submit(handle);
    }

    void await_resume() {}
};

template<concepts::scheduler Scheduler = TrivialScheduler>
inline SchedAwaitable<Scheduler> sched() {
    return {};
}

} // namespace coro
