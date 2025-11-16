#include "awaiter.hpp"
#include "coro.hpp"
#include "looper.hpp"
#include "timer.hpp"
#include <chrono>
#include <coroutine>
#include <exception>
#include <print>
#include <semaphore>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace std::chrono_literals;
using namespace coro;

Future<int> fib(int x) {
    if (x < 0) {
        co_return co_await fib(x + 2) - co_await fib(x + 1);
    } else if (x < 2) {
        co_return x;
    } else {
        co_return co_await fib(x - 1) + co_await fib(x - 2);
    }
}

Future<int> test_success() {
    co_await get_global_timer().sleep_for(500ms);
    co_return 42;
}

Future<void> test_fail() {
    if(co_await get_global_timer().with_timeout_for(get_global_timer().sleep_for(1000ms), 200ms)) {
        std::println("{}", 42);
    } else {
        throw std::runtime_error("timeout");
    }
    co_return;
}

void test_timeout() {
    WaitableFuture<int> future = make_waitable(test_success());
    if(!future.wait_for(200ms)) {
        std::println("time out");
        forget(std::move(future).detach());
        return;
    }
    try {
        std::println("{}", future.get());
    } catch (const std::exception& e) {
        std::println(std::cerr, "error: {}", e.what());
    }
}

CancelableFuture<void> test_cancel() {
    using promise_type = CancelableFuture<void>::promise_type;
    auto self = co_await this_coroutine<promise_type>();
    auto [idx, res] = co_await wait_any(
        get_global_timer().sleep_for(1s), 
        canceled(self)
    );
    if (idx == 1) {
        std::println("canceled");
    }
}

WaitableFuture<void> test_cancel2() {
    auto cancelable = test_cancel();
    auto [idx, res] = co_await wait_any(
        get_global_timer().sleep_for(500ms), 
        cancelable
    );
    if (idx != 1) {
        cancelable.cancel();
    }
}

WaitableFuture<void> test_waitall() {
    auto t0 = std::chrono::steady_clock::now();
    auto [r0, r1, r2] = co_await wait_all(
        get_global_timer().sleep_for(100ms), 
        get_global_timer().sleep_for(200ms),
        fib(8)
    );
    std::println("{}", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0));
    if (r2) {
        std::println("{}", r2.value());
    }
}

int main() {

    auto t1 = test_cancel2();
    t1.wait();

    auto t2 = test_waitall();
    t2.wait();

    test_timeout();
    std::counting_semaphore<> sem{0};
    with_callback(
        test_success(),
        [](int x) { std::println("{}", x); },
        [](std::exception_ptr exception) {
            try { 
                std::rethrow_exception(exception); 
            } catch (const std::exception& e) {
                std::println(std::cerr, "error: {}", e.what());
            }
        },
        [&]() {
            sem.release();
        }
    );

    with_callback(
        test_fail(),
        []() { std::println("???"); },
        [](std::exception_ptr exception) {
            try { 
                std::rethrow_exception(exception); 
            } catch (const std::exception& e) {
                std::println(std::cerr, "error: {}", e.what());
            }
        },
        [&]() {
            sem.release();
        }
    );

    sem.acquire();
    sem.acquire();

    return 0;
}
