#include "coro.hpp"
#include "scheduler.hpp"
#include "timer.hpp"
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
        co_await get_global_scheduler().sched();
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

int main() {

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
