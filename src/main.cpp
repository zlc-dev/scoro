#include "awaitable.hpp"
#include "coro.hpp"
#include "io/service.hpp"
#include "ptr/rc_ptr.hpp"
#include "sync.hpp"
#include "timer.hpp"
#include <chrono>
#include <coroutine>
#include <exception>
#include <iostream>
#include <print>
#include <semaphore>
#include <stdexcept>
#include <type_traits>
#include <unistd.h>
#include <utility>

using namespace std::chrono_literals;
using namespace coro;

Future<int, true> fib(int x) {
    if (x < 0) {
        co_return co_await fib(x + 2) - co_await fib(x + 1);
    } else if (x < 2) {
        co_return x;
    } else {
        co_return co_await fib(x - 1) + co_await fib(x - 2);
    }
}

Future<int, true> test_success() {
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


Future<int&, true> test_ref() {
    static int x = 100;
    co_return x;
}

Future<std::exception_ptr> test_return_exception_ptr() {
    co_return std::make_exception_ptr(std::runtime_error("test"));
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
    auto [r0, r1, r2] = (co_await wait_all(
        get_global_timer().sleep_for(100ms),
        test_ref(),
        fib(8)
    )).view();
    std::println("wait all");
    std::println("{}", r1);
}

WaitableFuture<void> test_sem() {
    auto sem = make_rc<coro::CountingSemaphore>( 3 );
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

CancelableWaitableFuture<bool> test_cancel() {
    auto& promise = co_await this_promise<CancelableWaitableFuture<bool>::promise_type>();
    auto [idx, res] = co_await wait_any(promise.wait_cancel(), get_global_timer().sleep_for(300ms));
    if (idx == 0) {
        std::println("test_cancel: canceled");
        co_return false;
    }
    co_return true;
}

CancelableWaitableFuture<void, true> test_wait_each() {
    auto& promise = co_await this_promise<CancelableWaitableFuture<void, true>::promise_type>();
    auto cancelable_task = test_cancel().detach();
    co_await get_global_timer().with_timeout_for(promise.wait_cancel(), 100ms);
    auto sleep = get_global_timer().sleep_for_cancelable(500ms);
    auto tasks = wait_each(sleep, cancelable_task, promise.wait_cancel(), test_return_exception_ptr());
    for(int i = 0; i < 3; ) {
        auto ret = co_await tasks;
        const auto [idx, res] = std::move(ret.value());
        if (idx == 0) {
            if (res.get<0>()) {
                std::println("500ms");
                if (cancelable_task.cancel()) {
                    std::println("cancelable_task timeout");
                }
            } else std::println("sleep cacneled");
            ++i;
        } else if (idx == 1) {
            if (res.get<1>()) std::println("task finish");
            ++i;
        } else if (idx == 2) {
            std::println("test_wait_each: cacneled");
            cancelable_task.cancel();
            sleep.cancel();
        } else if (idx == 3) {
            try {
                std::rethrow_exception(res.get<3>());
            } catch (const std::exception& e) {
                std::println("wait each 3 : {}", e.what());
            }
            ++i;
        }
    }
    co_return;
}

static io::Service service {};

Future<void> test_rw() {
    char ch = 0;
    while(ch != 'q') {
        auto [idx, res] = co_await wait_any(
            service.read(STDIN_FILENO, &ch, 1, 0),
            get_global_timer().sleep_for(3s)
        );
        if (idx == 1) {
            co_await service.nop(); // to wake service
            std::println("read timeout");
            break;
        } else if (idx == 0) {
            if (res.get<0>() < 0) {
                exit(res.get<0>());
            }
        }
        co_await service.write(STDOUT_FILENO, &ch, 1, 0);
    }
}


int main() {


    service.run(test_rw());

    auto t0 = test_wait_each();
    t0.wait_for(700ms);
    t0.wait();
    t0.get();

    test_sem().wait();
    auto t1 = test_waitall();
    t1.wait();
    t1.get();

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
