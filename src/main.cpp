#include "awaitable.hpp"
#include "coro.hpp"
#include "io/service.hpp"
#include "looper.hpp"
#include "ptr/intrusive_ptr.hpp"
#include "scheduler.hpp"
#include "sync.hpp"
#include "timer.hpp"
#include <chrono>
#include <coroutine>
#include <exception>
#include <future>
#include <iostream>
#include <print>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

using namespace std::chrono_literals;
using namespace coro;

Future<int> fib(int x) {
    co_await sched<LooperScheduler>();
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

WaitableFuture<void> test_sem() {
    auto sem = make_intrusive<coro::CountingSemaphore>( 3 );
    sem->release(-3);
    with_callback(
        get_global_timer().sleep_for(200ms), 
        [=] mutable { std::println("task 1 finish"); sem->release(); }
    );
    with_callback(
        get_global_timer().sleep_for(100ms), 
        [=] mutable { std::println("task 2 finish"); sem->release(); }
    );
    co_await sem->acquire();
    co_await sem->acquire();
    with_callback(
        std::suspend_never {}, 
        [=] mutable { std::println("task 3 finish"); sem->release(); }
    );
    co_await get_global_timer().sleep_for(100ms);
    co_await sem->acquire();

    with_callback(
        get_global_timer().sleep_for(300ms), 
        [=] mutable { std::println("task 4 finish"); sem->release(); }
    );
    if (!co_await get_global_timer().with_timeout_for(sem->acquire(), 100ms)) {
        std::println("sem time out");
    }
    std::println("wait finish");
}

CancelableWaitableFuture<void> test_cancel() {
    co_await sched<LooperScheduler>();
    auto& promise = co_await this_promise<CancelableWaitableFuture<void>::promise_type>();
    auto cancel_waiter = promise.wait_cancel();
    if (!cancel_waiter) {
        std::println("canceled");
        co_return;
    }
    auto [idx, res] = co_await wait_any(std::move(cancel_waiter.value()), std::suspend_always {});
    if (idx == 0) {
        std::println("canceled");
    }
    co_return;
}


WaitableFuture<void> test_wait_each() {
    auto tasks = wait_each(get_global_timer().sleep_for(500ms), get_global_timer().sleep_for(1s));
    auto start = std::chrono::steady_clock::now();
    while(auto ret = co_await tasks) {
        auto [idx, res] = std::move(ret.value());
        if (idx == 0) {
            std::println("500ms");
        } else if (idx == 1) {
            std::println("1s");
        }
    }
    auto end = std::chrono::steady_clock::now();
    std::println("tot duration: {}", std::chrono::duration_cast<std::chrono::milliseconds>(end - start));
    co_return;
}

// static io::Service service {};

// Future<void> test_rw() {
//     char ch = 0;
//     while(ch != 'q') {
//         auto [idx, res] = co_await wait_any(
//             service.read(STDIN_FILENO, &ch, 1, 0),
//             get_global_timer().sleep_for(5s)
//         );
//         if (idx == 1) {
//             co_await service.nop(); // to wake service
//             std::println("read timeout");
//             break;
//         } else if (idx == 0) {
//             if (std::get<int>(res) < 0) {
//                 exit(std::get<int>(res));
//             }
//         }
//         co_await service.write(STDOUT_FILENO, &ch, 1, 0);
//     }
// }

int main() {

    // service.run(test_rw());

    test_wait_each().wait();

    auto t1 = test_cancel();
    std::this_thread::sleep_for(300ms);
    while (!t1.wait_for(500ms)) {
        t1.cancel();
    }
    t1.get();

    test_sem().wait();
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
        [&]() { std::println("???");},
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
